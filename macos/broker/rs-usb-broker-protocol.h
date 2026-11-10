/* License: Apache 2.0. See LICENSE file in root directory.
 *
 * Wire protocol between librealsense's RSUSB broker client (src/libusb/broker/, C++14) and the macOS USB broker
 * daemon realsense-usbd (daemon/ next to this file, C++17), the shared RealSense camera service of this Mac: one
 * daemon serves every application, whatever build or project it comes from (README.md next to this file). Plain C so
 * both sides compile the same file; the copy the client patch (librealsense/macos-usb-broker.patch) puts at
 * src/libusb/broker/rs-usb-broker-protocol.h of librealsense must stay byte-identical (the builds check).
 * Normative together with DESIGN.md; where CONTRACT.md disagrees, DESIGN.md section 11 wins.
 *
 * Compatibility (DESIGN 13): applications are never updated together with the daemon, so this protocol only grows.
 * A change that an existing client or daemon could misread gets a new RSUSB_PROTOCOL_VERSION, and the daemon keeps
 * serving every version from RSUSB_PROTOCOL_MIN_VERSION on: a newer daemon works with every older application. New
 * reply keys and new ops that old peers never send or simply ignore need no new version.
 *
 * Transport: libxpc dictionaries on the Mach service RSUSB_SERVICE_NAME (client: XPC_CONNECTION_MACH_SERVICE_PRIVILEGED).
 * Every client request carries RSUSB_KEY_OP and is sent with a reply (xpc_connection_send_message_with_reply) unless
 * marked "no reply". Daemon -> client messages that are not replies carry RSUSB_KEY_EVENT instead.
 *
 * Types: every integer is an xpc uint64 except RSUSB_KEY_STATUS, an xpc int64. "data" is an xpc data object, "string"
 * an xpc string, "array" an xpc array, "shmem" an xpc_shmem object. Both sides MUST read every key through a typed
 * getter (xpc_dictionary_get_value + xpc_get_type check) - never xpc_dictionary_get_uint64/_get_data alone, because
 * they return 0/NULL for a missing key and 0 is a valid value of if, rq, val, idx. Keys not listed for a message are
 * ignored by the receiver (the daemon ignores any shm a client sends). A request with a missing key or a key of the
 * wrong type is malformed: the daemon cancels the connection. Out-of-range values in a well-formed request are
 * answered with a status.
 *
 * Ids: h (messenger) and r (request) are assigned by the daemon per connection, monotonically increasing from 1,
 * 64-bit, never reused within a connection. tok (device) is never reused within a daemon run. A client keeps all
 * three scoped to its session (one HELLO on one connection) and never compares them across sessions.
 *
 * Status (RSUSB_KEY_STATUS) is the libusb error code of the operation the daemon performed for the client (libusb 1.0.30
 * numbering, rsusb_status below). The client maps it exactly like libusb_status_to_rs() in src/libusb/handle-libusb.h:
 * -1..-12 identity, everything else (incl. RSUSB_ST_OTHER = -99 and unknown values) -> RS2_USB_STATUS_OTHER.
 *
 * Message schemas ("->" request keys, "<-" reply keys):
 *
 * HELLO: must be the first message on a connection; any other op first, or a second HELLO, cancels the connection.
 *   -> op, pv (the protocol version the client speaks: its RSUSB_PROTOCOL_VERSION)
 *   <- st (0), pv, auth (rsusb_auth), build (string, <= RSUSB_MAX_BUILD_LEN, diagnostics),
 *      sv (the daemon's service version, a number that grows with every release of the daemon; absent from the
 *      daemons of service version 1, which admitted only the programs of one build)
 *   A daemon serves every pv from its RSUSB_PROTOCOL_MIN_VERSION to its RSUSB_PROTOCOL_VERSION: for such a pv it
 *   answers with the same pv and speaks that version on this connection; for any other pv it answers
 *   auth = RSUSB_AUTH_VERSION with pv = the newest version it speaks (the application then asks for an update of the
 *   camera service).
 *   The client uses the broker only if st == 0, auth == RSUSB_AUTH_OK and pv == its own RSUSB_PROTOCOL_VERSION.
 *   After auth != OK the daemon answers nothing else and cancels the connection after sending the reply.
 *   Only when the administrator restricted the service to a list of programs (optional AllowedCDHashes, DESIGN 13):
 *   a peer that is not on it gets no reply: its messages are dropped and the daemon cancels any connection still
 *   without HELLO after RSUSB_NEW_TIMEOUT_MS, so the client sees XPC_ERROR_CONNECTION_INVALID (final, no retry)
 *   about one second after connecting.
 *
 * ENUMERATE: snapshot of the allowed RealSense devices (DESIGN 2.4.1), sorted by IORegistry entry ID ascending,
 *   served from the daemon's cache (no device I/O).
 *   -> op
 *   <- st, devs (array, <= RSUSB_MAX_DEVICES, of device dictionaries):
 *        tok  device token: nonzero, unique per physical device instance for the daemon's lifetime; a re-enumerated
 *             device (hand-back, HWRST, DFU, re-plug) gets a new token
 *        path string "<bus>-<p1>.<p2>...-<address>" exactly as get_device_path() (src/libusb/enumerator-libusb.cpp),
 *             1..RSUSB_MAX_PATH_LEN bytes. The path is librealsense's device id and usually does NOT change when the
 *             device re-enumerates on Apple Silicon (a D455 alone on its bus comes back as 1-1-1): a new tok with an
 *             unchanged path is normal.
 *        dd   data, RSUSB_DEVICE_DESC_LEN bytes: the device descriptor in wire order (bLength..bNumConfigurations)
 *        ifs  array (<= RSUSB_MAX_INTERFACES per configuration) of interface dictionaries, config->interface[] order,
 *             configurations concatenated in index order. A device is listed only when every configuration was read
 *             successfully; an interface that exceeds a limit is left out (and logged), the device is still listed.
 *          cfg  configuration index c (0..bNumConfigurations-1); usb_device_info::mi = position within that cfg
 *          alts array (1..RSUSB_MAX_ALTSETTINGS) of altsetting dictionaries, altsetting[] order:
 *            d  data, RSUSB_INTERFACE_DESC_LEN bytes: bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting,
 *               bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface (libusb values)
 *            x  data, 0..RSUSB_MAX_EXTRA_LEN bytes: libusb's altsetting.extra, verbatim
 *          eps  data, 7 * n bytes (n <= RSUSB_MAX_ENDPOINTS) for altsetting[0]'s endpoints in order, each:
 *               bLength, bDescriptorType, bEndpointAddress, bmAttributes (raw), wMaxPacketSize (LE), bInterval
 *
 * LEASE: sent by create_usb_device() before it returns a device object. Takes this connection's lease on the device
 *   (DESIGN 2.7) without opening or capturing it; idempotent; ends only with the connection or the device.
 *   -> op, tok       <- st
 *   st: NO_DEVICE unknown/gone token or hand-back in progress, ACCESS camera disabled by policy, NO_MEM lease limit.
 *
 * OPEN: one messenger (usb_device::open). Claims interface `if` (of the active configuration) and its associated
 *   VIDEO_STREAMING interfaces (rule of device-libusb.cpp), all or nothing. Takes the lease if not yet held and
 *   captures the device from macOS on the first OPEN of a device instance.
 *   -> op, tok, if (bInterfaceNumber)
 *   <- st, h (messenger handle, present iff st == 0)
 *   st: NO_DEVICE unknown/gone token or hand-back in progress; NOT_FOUND no such interface in the active configuration;
 *   ACCESS an interface of the claim set is held by another messenger (any connection, including this one), capture
 *   refused, or camera disabled by policy; NO_MEM quota; BUSY only when libusb_claim_interface still fails with BUSY
 *   after the daemon's retries; otherwise the libusb error. If an interface of the claim set is being released by a
 *   CLOSE of this same connection, OPEN waits for that release instead of answering ACCESS.
 *
 * CLOSE: ends a messenger: cancels its bulk/interrupt transfers (never a control transfer), waits for all its transfers
 *   to complete (control transfers run to completion, at most the clamped timeout), releases its interfaces, frees its
 *   requests. Never ends the lease.
 *   -> op, h       <- st
 *
 * CONTROL: libusb_control_transfer on EP0.
 *   -> op, h, rt (bmRequestType), rq (bRequest), val (wValue), idx (wIndex), len (wLength <= RSUSB_MAX_CONTROL_LEN),
 *      to (timeout ms), data (OUT only, exactly len bytes; absent when len == 0 or for IN)
 *   <- st, n (bytes transferred, iff st == 0), data (IN only, exactly n bytes, iff st == 0)
 *   rt must be RSUSB_RT_CLASS_IF_OUT or RSUSB_RT_CLASS_IF_IN and LOBYTE(idx) an interface claimed by h (else ACCESS).
 *   No IN data is returned on failure (libusb in-process may copy partial IN data; librealsense discards it).
 *
 * BULK: synchronous transfer on a bulk or interrupt endpoint (bulk or interrupt chosen by the endpoint's raw
 *   bmAttributes in the daemon's snapshot: 2 bulk, 3 interrupt, anything else INVALID_PARAM; no separate INTERRUPT op).
 *   -> op, h, ep (bEndpointAddress), len (<= RSUSB_MAX_BULK_LEN), to, data (OUT only, exactly len bytes)
 *   <- st, n (iff st == 0), data (IN only, exactly n bytes, iff st == 0)
 *   ep must belong to an interface claimed by h (else NOT_FOUND).
 *
 * RESET_EP: libusb_clear_halt.   -> op, h, ep       <- st
 *
 * REQ_CREATE: allocates one async IN request (usb_request) and its page-aligned shared buffer.
 *   -> op, h, ep (IN endpoint with raw bmAttributes 2 or 3 of an interface claimed by h), len (1..RSUSB_MAX_REQUEST_LEN)
 *   <- st, r (request id), shm (shmem of at least len bytes)   (r and shm iff st == 0)
 *
 * REQ_SUBMIT: libusb_submit_transfer of the whole buffer, timeout 0. The reply is sent after the submit result is known;
 *   REQ_DONE for this submission may arrive before the reply.
 *   -> op, r       <- st (BUSY: already in flight; NO_DEVICE: device gone; NO_MEM: in-flight byte quota)
 *
 * REQ_CANCEL: libusb_cancel_transfer. st is libusb's result (NOT_FOUND when not in flight); REQ_DONE follows if it was.
 *   -> op, r       <- st
 *
 * REQ_DESTROY (no reply): forget r. If in flight the daemon cancels it, frees it after its completion and sends no
 *   REQ_DONE for it. Unknown r is ignored.
 *   -> op, r
 *
 * Events (daemon -> client, no reply expected):
 * REQ_DONE: one per completed submission, whatever the outcome; the first n bytes of shm hold the data.
 *   ev, r, xs (rsusb_transfer_status), n (actual_length)
 * DEVICE_GONE: the device behind tok disappeared; every op on its handles now fails with NO_DEVICE. Sent to every
 *   connection holding a handle or a lease on tok, before the REQ_DONE(NO_DEVICE) completions it causes.
 *   ev, tok
 *
 * Deadlines (client side): HELLO RSUSB_HELLO_TIMEOUT_MS; ENUMERATE, LEASE, REQ_CREATE, REQ_SUBMIT, REQ_CANCEL
 * RSUSB_CALL_TIMEOUT_MS; OPEN, CLOSE, RESET_EP RSUSB_DEVICE_CALL_TIMEOUT_MS (> the daemon's RSUSB_WATCHDOG_MS, so the
 * daemon answers NO_DEVICE first when a device wedges); CONTROL, BULK clamped `to` + RSUSB_REPLY_MARGIN_MS. A missed
 * CONTROL/BULK deadline is RSUSB_ST_TIMEOUT; a missed ENUMERATE deadline is an empty list; any other missed deadline
 * ends the session (the client cancels the connection, DESIGN 4.7.3).
 */
#ifndef RS_USB_BROKER_PROTOCOL_H
#define RS_USB_BROKER_PROTOCOL_H

#include <stdint.h>

#define RSUSB_SERVICE_NAME "io.github.dolegadolegowski.realsense-usbd"
#define RSUSB_PROTOCOL_VERSION 1u       /* the newest version this file describes; what a client built from it speaks */
#define RSUSB_PROTOCOL_MIN_VERSION 1u   /* the oldest version a daemon built from it still serves */

/* Keys */
#define RSUSB_KEY_OP "op"
#define RSUSB_KEY_EVENT "ev"
#define RSUSB_KEY_STATUS "st"
#define RSUSB_KEY_VERSION "pv"
#define RSUSB_KEY_AUTH "auth"
#define RSUSB_KEY_BUILD "build"
#define RSUSB_KEY_SERVICE_VERSION "sv"
#define RSUSB_KEY_DEVICES "devs"
#define RSUSB_KEY_TOKEN "tok"
#define RSUSB_KEY_PATH "path"
#define RSUSB_KEY_DEVICE_DESC "dd"
#define RSUSB_KEY_INTERFACES "ifs"
#define RSUSB_KEY_CONFIG "cfg"
#define RSUSB_KEY_ALTSETTINGS "alts"
#define RSUSB_KEY_INTERFACE_DESC "d"
#define RSUSB_KEY_EXTRA "x"
#define RSUSB_KEY_ENDPOINTS "eps"
#define RSUSB_KEY_INTERFACE "if"
#define RSUSB_KEY_HANDLE "h"
#define RSUSB_KEY_REQUEST_TYPE "rt"
#define RSUSB_KEY_REQUEST "rq"
#define RSUSB_KEY_VALUE "val"
#define RSUSB_KEY_INDEX "idx"
#define RSUSB_KEY_LENGTH "len"
#define RSUSB_KEY_TIMEOUT "to"
#define RSUSB_KEY_DATA "data"
#define RSUSB_KEY_TRANSFERRED "n"
#define RSUSB_KEY_ENDPOINT "ep"
#define RSUSB_KEY_REQUEST_ID "r"
#define RSUSB_KEY_SHMEM "shm"
#define RSUSB_KEY_TRANSFER_STATUS "xs"

enum rsusb_op
{
    RSUSB_OP_HELLO = 1,
    RSUSB_OP_ENUMERATE = 2,
    RSUSB_OP_OPEN = 3,
    RSUSB_OP_CLOSE = 4,
    RSUSB_OP_CONTROL = 5,
    RSUSB_OP_BULK = 6,
    RSUSB_OP_RESET_EP = 7,
    RSUSB_OP_REQ_CREATE = 8,
    RSUSB_OP_REQ_SUBMIT = 9,
    RSUSB_OP_REQ_CANCEL = 10,
    RSUSB_OP_REQ_DESTROY = 11,
    RSUSB_OP_LEASE = 12
};

enum rsusb_event
{
    RSUSB_EV_REQ_DONE = 1,
    RSUSB_EV_DEVICE_GONE = 2
};

enum rsusb_auth
{
    RSUSB_AUTH_OK = 0,
    RSUSB_AUTH_VERSION = 1,          /* client pv is not a version this daemon serves */
    RSUSB_AUTH_USER = 2,             /* not root and not the console user in a local GUI session */
    RSUSB_AUTH_CAMERA_DISABLED = 3,  /* com.apple.applicationaccess allowCamera not true (MDM / Screen Time) */
    RSUSB_AUTH_LIMIT = 4             /* RSUSB_MAX_CLIENTS reached */
};

/* libusb_error values (libusb.h); usb_status uses the same numbers except OTHER (-13 there). */
enum rsusb_status
{
    RSUSB_ST_SUCCESS = 0,
    RSUSB_ST_IO = -1,
    RSUSB_ST_INVALID_PARAM = -2,
    RSUSB_ST_ACCESS = -3,
    RSUSB_ST_NO_DEVICE = -4,
    RSUSB_ST_NOT_FOUND = -5,
    RSUSB_ST_BUSY = -6,
    RSUSB_ST_TIMEOUT = -7,
    RSUSB_ST_OVERFLOW = -8,
    RSUSB_ST_PIPE = -9,
    RSUSB_ST_INTERRUPTED = -10,
    RSUSB_ST_NO_MEM = -11,
    RSUSB_ST_NOT_SUPPORTED = -12,
    RSUSB_ST_OTHER = -99
};

/* libusb_transfer_status values */
enum rsusb_transfer_status
{
    RSUSB_XS_COMPLETED = 0,
    RSUSB_XS_ERROR = 1,
    RSUSB_XS_TIMED_OUT = 2,
    RSUSB_XS_CANCELLED = 3,
    RSUSB_XS_STALL = 4,
    RSUSB_XS_NO_DEVICE = 5,
    RSUSB_XS_OVERFLOW = 6
};

/* Device scope: any PID of RSUSB_VID_REALSENSE; for RSUSB_VID_INTEL only the PIDs of the build-generated list
 * (DESIGN 2.4.1). Devices with an interface of a refused class are never listed or opened. */
#define RSUSB_VID_INTEL 0x8086u
#define RSUSB_VID_REALSENSE 0x38E5u

/* Control requests accepted (class request, interface recipient) */
#define RSUSB_RT_CLASS_IF_OUT 0x21u
#define RSUSB_RT_CLASS_IF_IN 0xA1u

/* DFU interface (exempt from the HWM filter) */
#define RSUSB_CLASS_APP_SPECIFIC 0xFEu
#define RSUSB_SUBCLASS_DFU 0x01u

/* HWM packet (src/hw-monitor.cpp fill_usb_buffer): u16 length, bytes 2..3 magic AB CD, bytes 4..7 opcode (only byte 4
 * is compared), bytes 8..11 param1, 12..15 param2 (uint32 LE). Checked on every 0x21 OUT control transfer except to a
 * DFU interface, and on every bulk/interrupt OUT (DESIGN 3.5). A denied packet is answered with RSUSB_ST_ACCESS. */
#define RSUSB_HWM_MIN_LEN 5u
#define RSUSB_HWM_MAGIC_0 0xABu
#define RSUSB_HWM_MAGIC_1 0xCDu
#define RSUSB_HWM_OPCODE_OFFSET 4u
#define RSUSB_HWM_PARAM1_OFFSET 8u
#define RSUSB_HWM_PARAM2_OFFSET 12u
#define RSUSB_HWM_XU_UNIT 3u        /* depth XU (src/ds/ds-private.h depth_xu) = HIBYTE(wIndex) */
#define RSUSB_HWM_XU_SELECTOR 1u    /* DS5_HWMONITOR = HIBYTE(wValue) */
#define RSUSB_HWM_OP_FWB 0x0Au      /* write flash bytes: param1 offset, param2 size (ProtectedFlashRanges only) */
#define RSUSB_HWM_OP_FES 0x0Bu      /* erase flash sectors: param1 sector, param2 count (ProtectedFlashRanges only) */
#define RSUSB_HWM_OP_FEF 0x0Cu      /* erase full flash: always denied */
#define RSUSB_HWM_OP_FSRU 0x0Du     /* flash status register unlock, never sent by librealsense: always denied */
#define RSUSB_HWM_OP_FPLOCK 0x0Eu   /* permanent flash lock: always denied */
#define RSUSB_FLASH_SECTOR_SIZE 0x1000u

/* Descriptor sizes on the wire */
#define RSUSB_DEVICE_DESC_LEN 18u
#define RSUSB_INTERFACE_DESC_LEN 9u
#define RSUSB_ENDPOINT_WIRE_LEN 7u

/* Limits (daemon enforces, client validates replies against them) */
#define RSUSB_MAX_BUILD_LEN 128u
#define RSUSB_MAX_DEVICES 32u
#define RSUSB_MAX_PATH_LEN 64u
#define RSUSB_MAX_INTERFACES 32u               /* per configuration (libusb USB_MAXINTERFACES) */
#define RSUSB_MAX_ALTSETTINGS 128u             /* libusb USB_MAXALTSETTING */
#define RSUSB_MAX_EXTRA_LEN 65535u             /* per altsetting; a configuration's wire bytes also <= its wTotalLength */
#define RSUSB_MAX_ENDPOINTS 32u
#define RSUSB_MAX_CONTROL_LEN 4096u
#define RSUSB_MAX_BULK_LEN 65536u
#define RSUSB_MAX_REQUEST_LEN (16u * 1024u * 1024u)
#define RSUSB_MAX_TIMEOUT_MS 10000u            /* to == 0 or larger is clamped to this */

/* Admission and quotas (DESIGN 3.6). Per-client limits are the per-device value times max(1, devices leased by the
 * connection); global limits bound the daemon. */
#define RSUSB_MAX_CLIENTS 16u                  /* READY connections */
#define RSUSB_MAX_NEW_PER_UID 4u               /* connections without HELLO, per euid */
#define RSUSB_NEW_TIMEOUT_MS 1000u             /* connection without HELLO is cancelled after this */
#define RSUSB_QUOTA_HANDLES_PER_DEVICE 16u
#define RSUSB_QUOTA_REQUESTS_PER_DEVICE 32u
#define RSUSB_QUOTA_SYNC_PER_DEVICE 16u        /* CONTROL + BULK + RESET_EP in progress */
#define RSUSB_QUOTA_SHM_PER_DEVICE (64ull * 1024u * 1024u)
#define RSUSB_QUOTA_INFLIGHT_PER_DEVICE (32ull * 1024u * 1024u)   /* request bytes submitted, not completed */
#define RSUSB_MAX_SHM_TOTAL (2048ull * 1024u * 1024u)             /* also <= physical memory / 8 */
#define RSUSB_MAX_INFLIGHT_TOTAL (512ull * 1024u * 1024u)         /* wired by IOKit while in flight */
#define RSUSB_BARRIER_EVERY 256u               /* daemon sends: xpc_connection_send_barrier every N messages */
#define RSUSB_MAX_UNACKED_SENDS 4096u          /* more outstanding (barrier not run) -> connection cancelled */
#define RSUSB_BARRIER_STALL_MS 5000u           /* a barrier pending longer -> connection cancelled */

/* Timing */
#define RSUSB_HELLO_TIMEOUT_MS 5000u
#define RSUSB_CALL_TIMEOUT_MS 3000u            /* ENUMERATE, LEASE, REQ_CREATE, REQ_SUBMIT, REQ_CANCEL */
#define RSUSB_DEVICE_CALL_TIMEOUT_MS 20000u    /* OPEN, CLOSE, RESET_EP */
#define RSUSB_REPLY_MARGIN_MS 2000u            /* CONTROL / BULK: clamped `to` + margin */
#define RSUSB_WATCHDOG_MS 15000u               /* daemon: a device call or CLOSE wait longer than this -> device lost */
#define RSUSB_LOSS_HIDE_MS 3000u               /* client: POLLING_DEVICES_INTERVAL_MS + 1000 (DESIGN 4.7.5) */

#endif
