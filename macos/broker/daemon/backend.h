// License: Apache 2.0. See LICENSE file in root directory.
// Abstract USB backend of realsense-usbd (DESIGN 2.3). Semantics are libusb's; the core never sees libusb types.
#pragma once

#include "core/snapshot.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace usbd
{
    // One asynchronous transfer. For control transfers `buffer` starts with the 8-byte setup packet and `length`
    // includes it; `done` receives the data bytes transferred (setup excluded). `done` runs on the backend's event
    // thread and must not block.
    struct usb_transfer
    {
        enum kind_t { control, bulk, interrupt };
        kind_t kind = control;
        uint64_t handle = 0;
        uint8_t endpoint = 0;
        uint8_t * buffer = nullptr;
        uint32_t length = 0;
        uint32_t timeout_ms = 0;
        std::function< void( int xs, uint32_t actual ) > done;
        virtual ~usb_transfer() = default;
    };

    // A device that is (or was) present. `key` doubles as the core's token: nonzero, never reused in a run.
    struct device_description
    {
        uint64_t key = 0;
        uint64_t registry_id = 0;
        bool drivers_attached = true;
        bool complete = false;   // every configuration read; incomplete snapshots are retried at the next refresh
        int active_config = 0;   // index, as cached by the OS (--dump only)
        snapshot snap;
    };

    // Live IOUSBHostDevice as seen by the IORegistry (sweep, hand-back).
    struct live_device
    {
        uint64_t registry_id = 0;
        uint16_t vid = 0, pid = 0;
        uint32_t location = 0;
        uint16_t address = 0;
        bool drivers_attached = false;
    };

    struct handback_target
    {
        uint64_t registry_id = 0;
        uint8_t bus = 0;
        uint16_t address = 0, vid = 0, pid = 0;
    };

    class usb_backend
    {
    public:
        struct events
        {
            std::function< bool( uint16_t vid, uint16_t pid ) > wanted;   // cheap pre-filter (2.4.1, VID/PID part)
            std::function< void( uint64_t key ) > arrived;
            std::function< void( uint64_t key ) > left;
        };
        virtual ~usb_backend() = default;

        // Registers hotplug (arrivals of present devices are reported from here) and starts the event thread.
        virtual void start( const events & ev ) = 0;
        virtual void stop() = 0;
        virtual std::vector< uint64_t > list() = 0;   // keys currently present and wanted
        virtual bool describe( uint64_t key, device_description & out ) = 0;
        virtual void forget( uint64_t key ) = 0;   // drops the backend's reference (device gone and cleaned up)

        virtual int open_anchor( uint64_t key, uint64_t & handle ) = 0;
        virtual void close_anchor( uint64_t handle ) = 0;
        virtual int kernel_driver_active( uint64_t handle, uint8_t iface ) = 0;
        virtual int capture( uint64_t handle, uint8_t iface ) = 0;   // libusb_detach_kernel_driver
        virtual int active_config( uint64_t handle, int & index ) = 0;
        virtual int open_handle( uint64_t key, uint64_t & handle ) = 0;
        virtual void close_handle( uint64_t handle ) = 0;
        virtual int claim( uint64_t handle, uint8_t iface ) = 0;
        virtual int release( uint64_t handle, uint8_t iface ) = 0;
        virtual int clear_halt( uint64_t handle, uint8_t ep ) = 0;

        virtual usb_transfer * alloc_transfer() = 0;
        virtual void free_transfer( usb_transfer * t ) = 0;
        virtual int submit( usb_transfer * t ) = 0;
        virtual int cancel( usb_transfer * t ) = 0;

        virtual uint64_t registry_id_of( uint64_t key ) = 0;
        virtual bool interfaces_held_elsewhere( uint64_t registry_id ) = 0;
        // Re-enumerates without capture and waits for drivers; 0, ACCESS (open elsewhere), NOT_FOUND or another error
        virtual int hand_back( const handback_target & t, unsigned poll_ms, unsigned wait_ms ) = 0;
        virtual void force_reenumerate( uint64_t registry_id ) = 0;   // watchdog recovery, may block
        virtual std::vector< live_device > live_devices() = 0;
    };
}
