# RSUSB-over-broker contract

What a broker-backed implementation of librealsense's RSUSB `usb_*` abstraction must do so that librealsense
behaves exactly as it does with the in-process libusb path. All paths below are relative to
`third_party/librealsense/` unless they start with `libusb/` (= `build/third-party/libusb/libusb/libusb/`) or
`tools/` / `macos/` (= repo root). Line numbers refer to the working tree with the port patch applied.

This document was written in and for the realsense-viewer-macos repository, and its paths are that repository's. In
a copy of this folder elsewhere they do not exist; the contract itself does not depend on them. The client it
describes is `librealsense/macos-usb-broker.patch` next to this document, and the build is `build-daemon.sh`. Who
may use the broker and how versions are negotiated changed with the shared camera service: DESIGN.md section 13.

Terms: **client** = the librealsense code in the viewer process. **broker** = the root daemon. **messenger** =
`usb_messenger` (one per `usb_device::open()`). **request** = `usb_request` (one async bulk/interrupt transfer slot).

---

## 0. The interface at a glance

Only two entry points construct anything: `usb_enumerator::query_devices_info()` and
`usb_enumerator::create_usb_device(info)` (src/usb/usb-enumerator.h:18-19). Everything else hangs off the
returned `rs_usb_device`:

```
usb_enumerator ─► usb_device ─► usb_interface ─► usb_endpoint
                        └─ open(iface) ─► usb_messenger ─► create_request(ep) ─► usb_request
```

Nothing in src/, common/ or tools/realsense-viewer calls libusb or IOKit outside src/libusb, so implementing these
classes (plus the two static functions) is sufficient.

---

## 1. Classes, exact semantics, and the libusb implementation

### 1.1 Types (src/usb/usb-types.h)

- `usb_status` (:48-64): values equal libusb's error codes except OTHER: SUCCESS 0, IO -1, INVALID_PARAM -2,
  ACCESS -3, NO_DEVICE -4, NOT_FOUND -5, BUSY -6, TIMEOUT -7, OVERFLOW -8, PIPE -9, INTERRUPTED -10, NO_MEM -11,
  NOT_SUPPORTED -12, OTHER -13 (libusb's OTHER is -99, libusb/libusb.h:1344).
  Mapping is `libusb_status_to_rs` (src/libusb/handle-libusb.h:20-40): identity for -1..-12, everything else
  (incl. -99 and unknown) -> OTHER. The broker may send raw libusb codes; the client must map them with this
  exact function. `usb_status_to_string.at(sts)` is used by callers (e.g. uvc-device.cpp:526, command-transfer.h:55),
  so the client must never return a value outside the enum (it would throw `std::out_of_range`).
- `endpoint_direction` (:66-70): WRITE 0, READ 0x80. `endpoint_type` (:72-78): CONTROL 0, ISOCHRONOUS 1, BULK 2,
  INTERRUPT 3.
- `usb_device_info` (:147-171): `id, vid, pid, mi, unique_id, serial, conn_spec, cls`. `operator==` (:173-177)
  compares id, vid, pid, mi, unique_id, conn_spec (not serial, not cls).
- `usb_descriptor` (:220-225): `{length, type, data}`; `data` holds the whole raw descriptor (incl. bLength/type).

### 1.2 usb_enumerator (src/libusb/enumerator-libusb.cpp)

`get_device_path(dev)` (:15-34): `"<bus>-<p1>.<p2>...<pn>-<address>"`, all decimal:
- bus = `libusb_get_bus_number` (darwin: locationID >> 24), ports = `libusb_get_port_numbers` (max 8),
  joined with `.`, address = `libusb_get_device_address`. No port -> `"<bus>--<addr>"`.
- The address can change when the device re-enumerates (hand-back, HWRST, DFU), so the id changes then.
  Capture-mode re-enumeration does not change it (libusb/os/darwin_usb.c:2195-2199).

`query_devices_info()` (:80-103):
- Creates a fresh `usb_context` (libusb_init + libusb_get_device_list, context-libusb.cpp:13-63). If init
  fails 10x it returns an empty list (no throw).
- For every device in `libusb_get_device_list` order, `get_subdevices(dev, desc)` (:36-78): for every config
  index `c < bNumConfigurations` (`libusb_get_config_descriptor(dev, c)`; on failure LOG_WARNING and skip), for
  every interface `i` in `config->interface[]` order, using `altsetting[0]` only:
  - skip if `bInterfaceSubClass == 2` (any class, :54-55);
  - skip if `bInterfaceClass == 0xFE` (application specific, DFU twin, :60-61);
  - emit `{ id = unique_id = path, conn_spec = desc.bcdUSB, vid = idVendor, pid = idProduct, mi = i (the
    index in config->interface[], which equals bInterfaceNumber for RealSense), cls = altsetting[0].bInterfaceClass,
    serial = "" }`.
- It lists every USB device on the system (no VID filter). The broker returns only VID 0x8086/0x38E5 devices.
  This is invisible to librealsense: every consumer filters by PID (src/platform/platform-utils.cpp:17-40,
  fw-update-factory.cpp:20-49), and `list_changed` is order-insensitive (src/platform/backend-device-group.h:24-45).
  Keep libusb's device order and the per-device interface order anyway (`devices.front()` is used when grouping,
  e.g. d400-factory.cpp:1225, fw-update-factory.cpp:68).

`create_usb_device(info)` (:105-132):
- Fresh `usb_context`; first device whose path `== info.id`; builds `usb_device_libusb(device, desc, info, ctx)`.
  Returns `nullptr` if not found (or on construction exception). No I/O, no capture, no open.
- The device object stores the **caller's** `info` verbatim (device-libusb.cpp:13, `_info(info)`), not a
  recomputed one (`mi`/`cls` are whatever interface the caller picked). The client must do the same.
- Each call yields an independent object with its own libusb context and event thread. One physical camera is
  typically opened by several objects at once (depth UVC mi 0, RGB UVC mi 3, HID, maybe fw-update/HWM).

### 1.3 usb_device (src/usb/usb-device.h:17-27, src/libusb/device-libusb.{h,cpp})

All getters are pure local reads of a snapshot taken in the constructor. They are called very often (the UVC
parser calls `get_descriptors()` once per descriptor, uvc-parser.cpp:149,177,198,367), so the client must cache
everything at `create_usb_device` time; no IPC per getter.

- `get_info()`: the stored caller info.
- `get_interfaces()`: interfaces in `config->interface[]` order, all configs concatenated (device-libusb.cpp:19-34).
- `get_interface(n)`: first interface whose `get_number() == n`, else `nullptr` (:76-83).
- `get_descriptors()` (:15-63): the flat list, in this exact order:
  1. device descriptor: `{bLength, bDescriptorType, raw 18 bytes}` (memcpy of `libusb_device_descriptor`, which
     is wire-identical on little-endian for bLength 18);
  2. for each config, for each interface, for each altsetting j (`num_altsetting`): the interface descriptor
     (`memcpy(&altsetting, bLength)`; wire-identical for bLength 9; a larger bLength would copy struct bytes,
     never seen on RealSense), followed by each descriptor in that altsetting's `extra` bytes, split by their own
     bLength (loop :54-62; a 0 bLength would loop forever: the broker must stop on bLength < 2 or overrun).
  - NOT included: config descriptor, endpoint descriptors and everything libusb attaches to endpoint `extra`
    (SS companion, CS_ENDPOINT, and any IAD that follows an endpoint), config-level `extra`.
  - libusb's parser (libusb/descriptor.c) puts class-specific descriptors that follow an interface descriptor
    into that altsetting's `extra` until the next INTERFACE/ENDPOINT/CONFIG/DEVICE descriptor. The broker must
    produce this list with libusb itself (same loop), not by re-parsing raw bytes differently.
- `open(n)` (:85-107): see 1.6. Returns `nullptr` on ANY failure (unknown interface, libusb_open error, capture
  ACCESS, claim failure); never throws. The status is not visible to callers.

### 1.4 usb_interface (src/usb/usb-interface.h, src/libusb/interface-libusb.{h,cpp})

Built from `altsetting[0]` only (interface-libusb.cpp:12-20):
- `get_number/get_class/get_subclass` = bInterfaceNumber/Class/SubClass.
- `get_endpoints()`: altsetting[0] endpoints in descriptor order.
- `first_endpoint(dir, type = BULK)` (:27-38): first endpoint with `get_type() == type && get_direction() == dir`,
  else `nullptr`.
- `get_associated_interfaces()` (libusb-only, interface-libusb.h:28): built in device-libusb.cpp:35-46: walking
  interfaces in order, a VIDEO/VIDEO_CONTROL interface becomes "current"; every later VIDEO/VIDEO_STREAMING
  interface is appended to the current one. (Not the VC header's baInterfaceNr list.) The client must rebuild
  exactly this rule; `open(vc)` claims the VC interface plus all its associated VS interfaces.

### 1.5 usb_endpoint (src/usb/usb-endpoint.h, src/libusb/endpoint-libusb.h)

- `get_address()` = bEndpointAddress.
- `get_type()` = `(endpoint_type)bmAttributes` - the RAW byte, not masked with 3 (endpoint-libusb.h:21). Send raw
  bmAttributes and cast the same way (a SuperSpeed interrupt endpoint with usage bits set would no longer match
  INTERRUPT; must behave identically).
- `get_direction()` = `address >= 0x80 ? READ : WRITE`.
- `get_interface_number()` = owning interface number.
- Endpoints are matched by address in all transfers; the object identity is irrelevant to libusb.

### 1.6 open() and the handle (src/libusb/handle-libusb.h, port-patched)

`usb_device_libusb::open(n)` -> `get_handle(n)` -> `handle_libusb(ctx, device, interface)` (:45-99):
1. under the global `darwin_handle_mutex` (:119-123; serializes every libusb open/capture/close in the process):
   `libusb_open` (fail -> throw -> `nullptr`);
2. APPLE only (:61-72): if `libusb_kernel_driver_active(h, n) == 1` (only the first interface is checked) then
   `libusb_detach_kernel_driver(h, n)`; if that returns ACCESS: close + throw ("macOS requires administrator
   privileges"). Any other detach error is ignored and the claim is attempted. Auto-detach is NOT enabled on
   macOS, so release never re-attaches; the capture stays until the process exits (and beyond, see 4.4).
3. claim `n`, then each associated interface (:85-96). `claim_interface` (:141-177): on BUSY retry 2 more times
   with 10 ms sleeps; any failure -> release everything claimed so far + `libusb_close` + throw -> `nullptr`.
4. `_context->start_event_handler()` (refcounted per context; context-libusb.cpp:81-98).

Destruction (`~handle_libusb`, :101-108; happens when the last messenger/streamer reference drops):
stop_event_handler, then (under the mutex) release associated interfaces, release `n`, `libusb_close`.
No hand-back, no re-attach.

Required broker behaviour:
- One libusb device handle per physical device, opened once; capture right after `libusb_open` and before any
  interface is claimed (capture re-opens only the capturing handle, darwin_usb.c:2195-2199/2078-2138; other
  handles of the same device would be left stale - the reason for the port's single-capture rule).
- Per client messenger: claim `n` + associated interfaces on that shared handle, release them when the
  messenger closes. Release on darwin is only `USBInterfaceClose` (no re-enumeration) when auto-detach is off.
- Exclusivity: with libusb every messenger is its own handle and claiming an interface already claimed by another
  handle fails on darwin with `kIOReturnExclusiveAccess` -> ACCESS (darwin_usb.c:1911-1915, 361-362) -> `nullptr`.
  The broker should keep this: an interface may be held by one messenger at a time, across and within clients;
  a second OPEN of a held interface fails (client returns `nullptr`). librealsense never overlaps opens of the
  same interface (HID: hid-device.cpp:86-99 vs 121-200 are sequential; HWM bulk opens per command under the
  locked_transfer mutex). Verify on hardware that same-process double claim really fails; if it does not, allow it.
- Capture decision mirrors step 2 (capture iff a kernel driver is active on the opened interface); the lease and
  hand-back rules are the fixed decision (lease per client connection, hand back after the last lease ends plus
  grace). Messenger close never hands back.

### 1.7 usb_messenger (src/usb/usb-messenger.h, src/libusb/messenger-libusb.cpp)

All methods are synchronous and thread-safe; several threads call them concurrently on different messengers of
the same device (UVC dispatcher threads, HID dispatcher, HWM callers, fw update). A long call (up to 5 s) must not
block other messengers or completions: the broker must run sync ops concurrently (not on one serial queue per
connection), e.g. per-messenger serial queues.

`control_transfer(request_type, request, value, index, buffer, length, transferred&, timeout_ms)` (:36-47):
- `libusb_control_transfer` on EP0. Direction from `request_type & 0x80`. IN: up to `length` bytes come back
  into `buffer`; OUT: `length` bytes are sent; `buffer` may be NULL with `length` 0 (DFU detach/final download,
  fw-update-device.cpp:57,285).
- Success: `transferred = bytes actually transferred` (short IN reads are success), return SUCCESS.
- Failure: return mapped status; `transferred` is NOT written (callers pre-initialize it, e.g. uvc-device.cpp:507).
- `timeout_ms == 0` means infinite in libusb; current callers pass 100 (XU, uvc-device.cpp:17), 1000 (HID feature
  report, hid-device.cpp:259; DFU detach), 5000 (PU/CT/probe-commit after the port change, uvc-device.cpp:19;
  DFU status/download) or 100 (DFU state/upload, fw-update-device.cpp:22). The broker may cap timeouts (e.g. 10 s)
  but must honour them; the client must return TIMEOUT, not hang, if the broker does not answer.
- darwin: every EP0 request goes through `DeviceRequestAsyncTO` on the device, regardless of recipient
  (darwin_usb.c:2528-2574); a claimed interface is not required by libusb, so the broker must enforce that
  `LOBYTE(wIndex)` is an interface held by that messenger (all librealsense control requests are class requests
  to an interface: 0x21/0xA1, see 2.x).

`bulk_transfer(endpoint, buffer, length, transferred&, timeout_ms)` (:49-71):
- type BULK -> `libusb_bulk_transfer`, INTERRUPT -> `libusb_interrupt_transfer`, anything else -> return OTHER
  without I/O (client-side check). Direction from the endpoint address.
- Success: `transferred = actual_length`, SUCCESS. Failure: mapped status, `transferred` not written (libusb sets
  its own `actual_length` even on timeout, but librealsense discards it).
- Used only by `command_transfer_usb` (command-transfer.h:34-74): OUT write of the HWM packet, then IN read of
  1024 bytes, both with `timeout_ms` (5000 from hw-monitor.cpp:91).

`reset_endpoint(endpoint, timeout_ms)` (:23-34): `libusb_clear_halt(address)`; the timeout is ignored (callers even
pass `RS2_USB_ENDPOINT_DIRECTION_READ` = 128 as timeout, uvc-streamer.cpp:157,198). darwin:
`ClearPipeStallBothEnds` on the claimed interface owning the endpoint; NOT_FOUND if no claimed interface of the
handle has it (darwin_usb.c:2057-2076).

`create_request(endpoint)` (:73-78): local object only (`usb_request_libusb`), no I/O, cannot fail. It keeps the
raw `libusb_device_handle*` (no ownership): librealsense guarantees requests die before their messenger
(uvc-device.cpp:705-715 then 198; hid-device.cpp:111-116). The broker must still clean up requests of a closed
messenger (cancel, defer free until completion).

`submit_request(request)` (:80-95):
- returns INVALID_PARAM if the native transfer has no handle; else `set_active(true)`,
  `libusb_submit_transfer`; on error `set_active(false)` and return the mapped status (BUSY if already in flight,
  NO_DEVICE once the device is gone, NOT_FOUND if the endpoint is not on a claimed interface, ...).
- The result MUST be synchronous and truthful. Callers resubmit from their completion callback and only stop on a
  submit error (uvc-streamer.cpp:135-137, hid-device.cpp:181-183, uvc-device.cpp:689-694). If the client returned
  SUCCESS and failed later, every failure would produce a completion that resubmits again: an endless loop after
  unplug. Acceptable implementations: a synchronous SUBMIT round trip, or an asynchronous submit whose failure is
  impossible except for device-gone, combined with a client-side sticky NO_DEVICE state set before the failing
  completion is delivered. Synchronous is simpler (~800 submits/s at full load).

`cancel_request(request)` (:97-107): `libusb_cancel_transfer`; NOT_FOUND (not in flight or already cancelling) ->
SUCCESS; other errors -> mapped status. Asynchronous: the completion still arrives later (status CANCELLED, or
COMPLETED if it raced). On darwin a bulk/interrupt cancel is `AbortPipe` and aborts EVERY transfer queued on that
endpoint (darwin_usb.c:2616-2656), so all in-flight requests of the endpoint complete as CANCELLED. The broker gets
this for free by calling `libusb_cancel_transfer`; the client must tolerate completions of requests it did not
cancel.

### 1.8 usb_request / usb_request_base / usb_request_callback (src/usb/usb-request.h, src/libusb/request-libusb.*)

`usb_request_base` (:43-65) owns: `_endpoint`, `_callback` (shared_ptr to `usb_request_callback`),
`_client_data` (uninitialized, unused by callers), `_buffer` (std::vector).
- `set_buffer(v)` (:52-57): copies `v` into `_buffer`, then `set_native_buffer(_buffer.data())` and
  `set_native_buffer_length(_buffer.size())`. With libusb the transfer writes straight into `_buffer`.
- `get_buffer()` returns `const std::vector<uint8_t>&` = `_buffer`. Callers read the payload from it after
  completion: uvc-streamer.cpp:123,130 (reads `[0]` and memcpys the WHOLE `_buffer.size()`), hid-device.cpp:160-177,
  uvc-device.cpp:685.
- Broker client: the data must be in `_buffer` before the callback runs: copy `actual_length` bytes from the
  request's shared memory into `_buffer.data()` on completion (bytes beyond `actual_length` keep stale content,
  as with libusb). `set_native_buffer`/`get_native_buffer` can just track the pointer/length; the shm region is
  sized to `get_native_buffer_length()` and (re)created while the request is idle.
- `get_actual_length()`: bytes received by the last completion (libusb `transfer->actual_length`; 0 for
  cancelled/error/no-device unless partial data arrived). Valid from the completion until the next submit.
- `get_native_request()`: libusb-internal; the client may return its own object or `this`.
- All librealsense async requests are IN (bulk IN for UVC, interrupt IN for UVC status and HID). The broker can
  reject OUT async requests.

Completion (`internal_callback`, request-libusb.cpp:20-40, port-patched):
- Every completion is delivered, whatever its status (completed, timed out, stall, overflow, cancelled, error,
  no device); the status itself is not exposed by `usb_request`. Sequence: under `completion_mutex`, if the
  request was orphaned free the transfer and its saved buffer and stop; else take `get_shared()` (weak_ptr ->
  shared_ptr, may be null) and `set_active(false)`; then, outside the lock, `get_callback()->callback(response)`.
- `usb_request_callback::callback` (:86-90) holds the callback's own mutex while calling the user function;
  `cancel()` (:81-84) takes the same mutex and nulls the function, so `cancel()` blocks until a running callback
  returns, and after it returns no further user call happens. Callers cancel the callback object first, then
  the requests (uvc-streamer.cpp:183-192, hid-device.cpp:107-112, uvc-device.cpp:711-712).
- Thread: libusb runs it on the usb_context's event thread (one per `create_usb_device` object,
  context-libusb.cpp:81-98); callbacks only post to a dispatcher and return. Broker client: run them on a client
  serial queue that is NOT the XPC connection's target queue (a sync XPC call issued from the target queue
  deadlocks), and never while holding locks the destructor or submit path take.

Destruction (`~usb_request_libusb`, :60-76): if active -> `libusb_cancel_transfer`; poll up to 10 x 10 ms for
`_active == false`; if still active, under `completion_mutex` set `user_data = nullptr` and move `_buffer` into
`orphaned_buffers` so the late completion frees both. The destructor never blocks longer than ~100 ms and never
frees an in-flight transfer.
Broker client equivalent: on destroy, if active send CANCEL, wait up to 100 ms for the completion event, else mark
the request orphaned locally (ignore its future completion) and tell the broker to free the transfer and shm once it
completes. The broker must never free a libusb transfer or its buffer while in flight.

`_active` is an atomic set true before submit, false on completion or failed submit (request-libusb.h:37).

---

## 2. Callers and how they use the abstraction

### 2.1 Backend glue
- `create_backend()` -> `rs_backend_linux` (src/libuvc/rsusb-backend-linux.cpp:13-21; used on macOS with
  FORCE_RSUSB_BACKEND, scripts/build.sh:38). Device watcher = `polling_device_watcher`.
- `rs_backend` (src/rsusb-backend/rsusb-backend.cpp:31-66): `query_usb_devices` = `query_devices_info()`;
  `create_usb_device` wraps `usb_enumerator::create_usb_device` in `command_transfer_usb`; uvc/hid go through
  `query_uvc_devices_info`, `query_hid_devices_info`, `create_rsuvc_device`, `create_rshid_device`.
- `polling_device_watcher` (src/polling-device-watcher.h:30-49): every 2000 ms (src/backend.h:19) calls
  query_uvc + query_usb + query_hid = 3 x `query_devices_info()`. Plus `rs_uvc_device::check_connection()`
  (uvc-device.cpp:921-930, from stream_on/close/get_profiles) and 2 more per `create_rsuvc_device`/
  `create_rshid_device`. The broker must answer ENUMERATE cheaply (libusb's device list in a long-lived context
  plus cached config descriptors needs no device I/O).

### 2.2 Derived infos (no broker involvement, but they depend on byte-identical usb infos)
- uvc (uvc-device.cpp:46-66): every info with `cls == VIDEO` (i.e. each VideoControl interface, VS were skipped):
  `id = device_path = info.id`, vid, pid, mi, unique_id, `usb_conn_spec = conn_spec`.
- hid (hid-device.cpp:18-41): every info with `cls == HID` yields 3 infos (`id` = gyro/accel/custom),
  `vid`/`pid` = uppercase hex strings, `unique_id = device_path = info.unique_id`.
- usb (HWM bulk / DFU): the raw usb infos. d400: `d400_try_fetch_usb_device` (src/ds/d400/d400-private.cpp:13-...)
  picks the info with the same unique_id and mi 3 or 4 (vendor-specific HWM interface); fw-update
  (fw-update-factory.cpp:20-49) picks infos whose pid is a recovery PID.

### 2.3 UVC device (src/uvc/uvc-device.cpp)
- `create_rsuvc_device` (:68-82): enumerate, `create_usb_device` for the matching id, `uvc_parser` (descriptors).
- Power D0 (:181-193): `_messenger = open(mi)` (VC + its VS interfaces); `nullptr` keeps D3 and the caller throws
  "failed to set power state" (:206-207). Then `listen_to_interrupts` (:668-703): interrupt IN endpoint of the VC
  interface (`first_endpoint(READ, INTERRUPT)`), one request, 1024-byte buffer, resubmitted from the callback via the
  dispatcher.
- Power D3 (:194-201) / destructor (:94-103): `close_uvc_device` (:705-715: clear streamers, cancel interrupt
  callback, `cancel_request`, drop the request) then `_messenger.reset()` -> interfaces released.
- Controls: PU/CT `get_data_usb`/`set_data_usb` (:501-620; 0xA1/0x21, GET_*/SET_CUR, wValue `ctrl<<8`, wIndex
  `unit<<8 | mi`, 4 bytes, 5000 ms; requires `transferred == 4`); XU `uvc_get_ctrl`/`uvc_set_ctrl` (:622-666; same
  but `len` bytes and 100 ms; NO_DEVICE -> throw "usb device disconnected", other errors -> false).
- Streaming: `play_profile` (:430-481): `reset_endpoint(VS bulk IN)`, probe/commit `query_stream_ctrl`
  (:805-919: 0x21/0xA1, wValue `PROBE(1)/COMMIT(2) << 8`, wIndex = VS interface number, 26/34/48 bytes, 5000 ms,
  retried up to 5x except on SUCCESS/TIMEOUT/NO_DEVICE).
- Note: the HWM XU and all XU/PU/CT controls of the depth sensor go through the depth UVC messenger
  (`invoke_powered`, hw-monitor.h:75-79, uvc-option.cpp:116-156).

### 2.4 UVC streamer (src/uvc/uvc-streamer.cpp)
- Ctor (:18-34): `first_endpoint(READ)` of the VS interface (bulk IN); buffer = 1024 + dwMaxVideoFrameSize
  (~1.8 MB for 1280x720 YUY2).
- `init` (:113-147): `request_count` (2, uvc-device.h:42) requests sharing one callback; the callback posts to the
  streamer dispatcher, checks `actual_length == buffer[0] + dwMaxVideoFrameSize` (or compressed), memcpys the
  whole buffer into a frame, and resubmits.
- `start` (:150-174): `reset_endpoint`, submit all; a submit error throws.
- `stop` (:176-209): cancel callback, stop watchdog/allocation, `cancel_request` each, `_requests.clear()` (runs
  the request destructors -> up to 100 ms wait each), `reset_endpoint`. Watchdog (:98-109): `reset_endpoint` when
  no frame for 10 frame periods.

### 2.5 HID / IMU (src/hid/hid-device.cpp; hidapi removed by the port, so the IMU uses libusb)
- `get_hid_interface` (:317-328): first interface with class HID; `get_hid_endpoint` (:330-339): its
  `first_endpoint(READ, INTERRUPT)`.
- `set_feature_report` (:237-314): opens its own messenger on the HID interface per call (closed on return):
  GET_REPORT 0xA1/0x01, SET_REPORT 0x21/0x09, GET_REPORT, wValue `(3<<8)|report_id`, wIndex = HID interface,
  `sizeof(FEATURE_REPORT)` = 9 bytes packed (hid-types.h:76-85), 1000 ms. `open()` failure -> NO_DEVICE.
- `start_capture` (:121-200): `open(hid)` (nullptr -> throw), ONE interrupt IN request (USB_REQUEST_COUNT = 1,
  :12) of `sizeof(REALSENSE_HID_REPORT)` = 38 bytes; callback accepts `actual_length == 32 or 38`; resubmits via
  dispatcher. The submit result of the first submit is ignored (:196-197).
- `stop_capture` (:101-119): cancel callback, cancel requests, clear, stop thread, `_messenger.reset()`.

### 2.6 HWM
- HWM_OVER_XU=ON (CMake/lrs_options.cmake:18, CMake/global_config.cmake:42-44): D400 (except RS400_IMU_PID or no
  usb info) and D500 send HWM via `command_transfer_over_xu` (src/ds/d400/d400-device.cpp:657-669,
  src/ds/d500/d500-device.cpp:418-428). Otherwise `command_transfer_usb` (src/platform/command-transfer.h:34-74):
  first interface with class 0xFF, `open()` per command, bulk OUT (`first_endpoint(WRITE)`) then bulk IN 1024 B.
- XU path (src/platform/uvc-option.cpp:116-156): `set_xu(depth_xu, DS5_HWMONITOR)` with a 1024-byte buffer
  (command zero-padded), then if a response is required `get_xu` 1024 bytes; response length = u32 at offset 1020
  + 4. Wire: SET_CUR = control_transfer(0x21, 0x01, wValue 0x0100, wIndex 0x0300 | mi (mi 0 = depth), 1024, 100 ms);
  GET_CUR = (0xA1, 0x81, same wValue/wIndex). (`depth_xu = {subdevice 0, unit 3, node 2}`, ds-private.h:50,71;
  XU wire format uvc-device.cpp:622-666.)
- `require_response = false` commands (e.g. HWRST, ds-device-common.cpp:402-404) only do SET_CUR; the device then
  resets and disappears (in-flight transfers complete with NO_DEVICE).

### 2.7 Firmware update (src/fw-update/*, src/ds/d500/d500-fw-update-device.cpp)
- Entering DFU: HWM opcode DFU 0x1E (`enter_update_state`, d400-device.cpp:140-147) -> device re-enumerates with a
  recovery PID (new id).
- `update_device` ctor (fw-update-device.cpp:167-189): `open(0)` (FW_UPDATE_INTERFACE_NUMBER, :23); nullptr ->
  throw "access failed". Then DFU class requests on interface 0 (wIndex 0):

| request | bmRequestType | bRequest | wValue | length | timeout | where |
|---|---|---|---|---|---|---|
| GET_STATE | 0xA1 | 5 | 0 | 1 | 100 | :44 (ACCESS -> "Permission Denied" exception) |
| DETACH | 0x21 | 0 | 1000 | 0 (NULL) | 1000 | :57 |
| UPLOAD | 0xA1 | 2 | 0 | sizeof(dfu_fw_status_payload) | 100 | :79 |
| DNLOAD | 0x21 | 1 | block# | <=1024 (0 = final) | 5000 / 100 | :243, :285 |
| GET_STATUS | 0xA1 | 3 | 0 | sizeof(dfu_status_payload) | 5000 | :136, :303, d500 :61 |

  (CLEAR_STATUS 4 and ABORT 6 are defined, fw-update-device.h:48-56, but not sent.)
- Manifest: any control-transfer failure after the last block is treated as success (device reset,
  fw-update-device.cpp:293-316), so NO_DEVICE/IO after the device drops is expected.
- Recovery PIDs: D400 0x0ADB, 0x0ADC (src/ds/d400/d400-private.h:22-23); D555 0x0ADE, D585S 0x0ADD, D500 0x0CFD,
  D500 USB2 0x0CFE (src/ds/d500/d500-private.h:19-25). (MIPI 0xBBCD is not USB.)
- In DFU mode the device exposes a 0xFF and a 0xFE interface; the 0xFE one is hidden from infos
  (enumerator-libusb.cpp:56-61) but present in `get_interfaces()`.
- Unsigned flash update uses HWM FES 0x0B / FWB 0x0A (ds-device-common.cpp:271,294) - allowed.

---

## 3. Runtime selection and build hook

### 3.1 Smallest hook
The two static functions in src/libusb/enumerator-libusb.cpp:80 and :105 are the only factories. Add at the top of
each (macOS only):

```cpp
#ifdef __APPLE__
            if( auto broker = usb_broker::instance() )
                return broker->query_devices_info();          // resp. broker->create_usb_device( info )
#endif
```

- `usb_broker::instance()` (new, client side): `std::call_once`: connect to the Mach service
  `io.github.dolegadolegowski.realsense-usbd` with `XPC_CONNECTION_MACH_SERVICE_PRIVILEGED`, send HELLO, accept only
  a compatible protocol version (and an "authorized" answer). Any error (service missing/disabled, connection
  invalid, peer rejected by the code-signing/console-user policy, version mismatch) -> `nullptr`, and the process
  uses libusb for its whole lifetime. Decide once per process; never mix paths or fall back mid-session.
- After a successful HELLO, a later broker crash/restart must not switch to libusb: the client reconnects lazily;
  outstanding sync calls return NO_DEVICE, in-flight requests complete (actual_length 0) and further submits on old
  messengers return NO_DEVICE; old device objects stay dead; the watcher's next enumeration (new connection) shows
  the devices again (possibly with new ids after the broker's startup hand-back). If reconnection fails,
  `query_devices_info()` returns an empty list (as libusb does when init fails).
- No other file needs to change: uvc/hid/fw-update/command-transfer only see the abstract classes.

### 3.2 CMake
- src/CMakeLists.txt:16-18 includes src/libusb/CMakeLists.txt on UNIX; libusb linkage comes from
  src/libuvc/CMakeLists.txt (`target_link_libraries(${LRS_TARGET} PRIVATE usb)`). The port already links
  CoreFoundation, IOKit and Security into the `usb` INTERFACE target on APPLE (CMake/external_libusb.cmake:35-39).
  libxpc and libdispatch are in libSystem; nothing else to link.
- Append to src/libusb/CMakeLists.txt:

```cmake
if(APPLE)
    target_sources(${LRS_TARGET}
        PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/broker/..."   # client classes, connection, protocol header
    )
endif()
```

- Keep the wire-protocol header in one place both sides compile (e.g. inside src/libusb/broker/, included by the
  daemon's build via an include path) so the patch (now `librealsense/macos-usb-broker.patch`) stays self-contained.
- Static libusb for the daemon: build/libusb_install/lib/libusb-1.0.a, headers
  build/libusb_install/include/libusb-1.0 (libusb 1.0.30 sources at
  build/third-party/libusb/libusb/libusb).

---

## 4. libusb darwin facts the daemon must respect (libusb/os/darwin_usb.c, 1.0.30)

### 4.1 Open / capture / claim
- `darwin_open` (:1586-1638): first open of a cached device does `USBDeviceOpenSeize`; `kIOReturnExclusiveAccess`
  is tolerated (handle works, `is_open = false`). `open_count` is per cached device, shared by all handles and
  contexts in the process (cached devices are a process-global list, :66-67).
- Capture = `libusb_detach_kernel_driver` (:2902-2946): device-wide, interface ignored. Only when
  `capture_count == 0`: needs root or the `com.apple.vm.device-access` entitlement (else ACCESS, :2930-2934), then
  `darwin_reenumerate_device(h, true)` = `USBDeviceReEnumerate(kUSBReEnumerateCaptureDeviceMask)`, which does not
  re-enumerate but closes/re-opens THIS handle and re-claims its interfaces (`darwin_restore_state`,
  :2078-2138, :2195-2199). `capture_count++` on every call.
- `libusb_kernel_driver_active` (:2304-2328): 1 if the IOUSBHostInterface at that location has a child (driver),
  0 if not, NOT_FOUND if no such interface.
- Claim (`darwin_claim_interface`, :1836-1945): `USBInterfaceOpen` on the interface user client; errors map through
  `darwin_to_libusb`; `kIOReturnExclusiveAccess` (interface open elsewhere, also by another handle) -> ACCESS.
  Several interfaces may be claimed on ONE handle; endpoints are found by searching all claimed interfaces of the
  handle (`ep_to_pipeRef`, :468-509) - the broker must add its own per-messenger ownership check.
- Release (`darwin_release_interface`, :1947-1981) closes the interface user client. With auto-detach off
  (`darwin_capture_release_interface`, :2979-2996) it never re-attaches.
- `libusb_close` (:1640-1682): releases claimed interfaces, `USBDeviceClose` when `open_count` reaches 0. It does
  NOT release the capture and does not reset `capture_count`.

### 4.2 Hand-back
- `libusb_attach_kernel_driver` (:2948-2965): `capture_count--`; at 0 it re-enumerates without capture and busy-waits
  up to 10 s (:43, :2207-2221) for the re-attach notification, then compares descriptors and tries
  `darwin_restore_state` on the handle (NOT_FOUND on mismatch, :2228-2240). Two concurrent re-enumerations ->
  NOT_FOUND (:2158-2161).
- Proven path in this repo: after closing everything, re-enumerate through IOKit like
  tools/release/rs-macos-release.c:72-89 (`USBDeviceOpenSeize` + `USBDeviceReEnumerate(0)` + close) and check
  `drivers_attached` (:44-68). The device then gets a new IOService/sessionID, so libusb sees a detach + attach and
  a fresh cached device (capture_count 0). The id (address) may change.
- A captured device stays captured after the capturing process exits or crashes (rs-macos-release.c:4-7): the
  daemon must hand back on SIGTERM and sweep at startup (devices with VID 0x8086/0x38E5 whose interfaces have no
  driver and no lease) - but must not steal from a root in-process viewer that currently holds it (an open by that
  process makes `USBDeviceOpenSeize`/claim fail with ExclusiveAccess; skip such devices).

### 4.3 Transfers and events
- Sync transfers (libusb/sync.c:145-169, :207-231): COMPLETED -> length/0, TIMED_OUT -> TIMEOUT, STALL -> PIPE,
  NO_DEVICE -> NO_DEVICE, OVERFLOW -> OVERFLOW, ERROR and CANCELLED -> IO. Must not be called from the libusb event
  thread or a transfer callback; run them on worker queues.
- Bulk timeouts are handled by IOKit (`ReadPipeAsyncTO`); interrupt endpoints have no OS timeout and rely on
  libusb's timer (:2370-2390). Control: `DeviceRequestAsyncTO` with completion and no-data timeout = timeout.
- Status mapping of completions (`darwin_transfer_status`, :2698-2724): Success/Underrun -> COMPLETED, Aborted ->
  CANCELLED, PipeStalled -> STALL, Overrun -> OVERFLOW, TransactionTimeout -> TIMED_OUT, else ERROR.
  `darwin_to_libusb` for sync/submit errors (:353-380): NotOpen/NoDevice -> NO_DEVICE, ExclusiveAccess -> ACCESS,
  PipeStalled -> PIPE, BadArgument -> INVALID_PARAM, TransactionTimeout -> TIMEOUT, UnknownPipe -> NOT_FOUND, others
  -> OTHER.
- Threads: IOKit callbacks run on libusb's private CFRunLoop thread (`libusb_darwin_at`, :749-865) and only signal
  completion; user callbacks run in whichever thread calls `libusb_handle_events*`. The daemon needs one permanent
  event thread (`libusb_handle_events_completed` loop, woken by `libusb_interrupt_event_handler` on shutdown).
- Submit errors (libusb/io.c:1538-1570): BUSY if already in flight; backend errors returned synchronously.
- Cancel (libusb/io.c:1609-1640): NOT_FOUND if not in flight or already cancelling; the transfer still completes
  later. Control cancel = `USBDeviceAbortPipeZero` (:2600-2614); bulk/interrupt cancel = `AbortPipe` (whole pipe,
  :2616-2656).
- Hotplug: supported via IOKit notifications. On detach (`darwin_devices_detached`, :654-721) devices that are
  re-enumerating are kept; otherwise `usbi_disconnect_device` -> every in-flight transfer of open handles completes
  with NO_DEVICE, later submits/sync calls return NO_DEVICE. A long-lived context's `libusb_get_device_list`
  reflects hotplug state without rescanning.

### 4.4 Consequences for the broker
- Serialize open/capture/close/hand-back per device (in-process equivalent: `darwin_handle_mutex`).
- Never call `libusb_close` with transfers in flight: cancel, wait for completions, then release/close.
- Device loss (unplug, HWRST, DFU entry/exit): drop the device's state, complete everything with NO_DEVICE, keep
  answering further calls on stale ids with NO_DEVICE; leases on it end without a hand-back (the device is gone).

---

## 5. Policy-relevant wire formats

### 5.1 HWM packet (src/hw-monitor.cpp:29-60, src/hw-monitor.h:41-47)
```
off  size  field
0    2     u16 LE  length of the rest = total - 4
2    2     u16 LE  0xCDAB  (bytes AB CD)        IVCAM_MONITOR_MAGIC_NUMBER
4    4     u32 LE  opcode                         (build_command_from_data reads only byte 4, hw-monitor.cpp:62-68)
8    4     u32 LE  param1
12   4     u32 LE  param2
16   4     u32 LE  param3
20   4     u32 LE  param4
24   n     data
```
Max 1000 bytes of command (HW_MONITOR_COMMAND_SIZE); XU transfers are padded to 1024 (HW_MONITOR_BUFFER_SIZE).
Response: first 4 bytes = opcode echo/status, rest data; XU response size = u32 at offset 1020 + 4.
Raw user commands (rs-terminal / `send_receive_raw_data`, d400-device.cpp:88-91) use the same layout.

Where it travels:
- XU: `control_transfer(0x21, SET_CUR 0x01, wValue 0x0100, wIndex 0x0300 | mi, len 1024)`.
- bulk: OUT on the first endpoint of the class-0xFF interface (command-transfer.h:48).

Denied opcodes (src/ds/ds-private.h:90-99; neither is used by librealsense itself):
- `FEF = 0x0C` - erase full flash.
- `FPLOCK = 0x0E` - permanent lock of the lower quarter of the flash.

Recommended filter: any OUT control transfer with `bmRequestType == 0x21 && bRequest == 0x01` and any bulk OUT whose
payload has `len >= 8 && p[2] == 0xAB && p[3] == 0xCD`, deny if `p[4] in {0x0C, 0x0E}` (byte, so upper opcode bytes
cannot smuggle it). Do the check on the broker's own copy of the bytes. Return ACCESS (-3): the XU path then throws
"set_xu(ctrl=1) failed" (uvc-option.cpp:135-138), the bulk path "command transfer failed ... RS2_USB_STATUS_ACCESS".
Everything else stays allowed (FES 0x0B, FWB 0x0A, FSRU 0x0D, DFU 0x1E, HWRST 0x20, calibration writes 0x16, 0x51,
0x61, 0x62, D500 0xA6/0xA8, ...).

### 5.2 Control requests librealsense issues (for an allowlist)
All are class requests to an interface; none is standard, vendor, device- or endpoint-recipient:
- 0xA1 / 0x21 (UVC GET_* 0x81-0x87 / SET_CUR 0x01, uvc-types.h:127-136; HID GET_REPORT 0x01 / SET_REPORT 0x09,
  hid-types.h:32-43; DFU table in 2.7). `LOBYTE(wIndex)` is the interface (UVC/XU: `unit<<8 | mi`; probe/commit:
  VS interface; HID: HID interface; DFU: 0). The broker issues CLEAR_FEATURE(ENDPOINT_HALT) itself via
  `libusb_clear_halt`.
- Sizes: <= 1024 (XU/HWM, DFU blocks), 48 (probe/commit), 9 (HID feature), 4 (PU/CT), 6/1/sizeof payloads (DFU).

### 5.3 Device scope
VID 0x8086 or 0x38E5 (src/usb/usb-types.h:41-42), any PID incl. the recovery PIDs in 2.7. (Optional tightening:
librealsense PID tables in src/ds/d400/d400-private.h and src/ds/d500/d500-private.h.)

---

## 6. Checklist: client must reproduce

1. Infos: same strings, same order, same `mi`/`cls`/`conn_spec`, `serial` empty; only the VID filter differs.
2. `create_usb_device`: match by id only, keep caller's info, snapshot interfaces/endpoints/descriptors once,
   `nullptr` if absent; no I/O.
3. Descriptor list: device + per-altsetting interface descriptor + its extras (libusb parsing), nothing else.
4. Interfaces from altsetting 0; associated VS interfaces by the VC-then-VS order rule; raw bmAttributes as type.
5. `open()`: `nullptr` on any failure, claims VC + associated VS together, all-or-nothing.
6. Sync ops: status mapping as `libusb_status_to_rs`, `transferred` only on success, timeouts honoured, concurrent.
7. Requests: data in `_buffer` before the callback, `get_actual_length` valid, every completion delivered (incl.
   cancelled/error/no-device), `_active` cleared before the callback, callbacks off the XPC target queue.
8. Submit synchronous and truthful (NO_DEVICE after loss); cancel returns SUCCESS for not-in-flight; whole-pipe
   abort tolerated; destructor waits <= 100 ms then orphans, broker frees after completion.
9. Broker gone after HELLO: NO_DEVICE everywhere, no silent fallback to libusb in the same process.
