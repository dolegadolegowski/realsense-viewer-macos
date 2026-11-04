// License: Apache 2.0. See LICENSE file in root directory.
// libusb backend (DESIGN 2.3.2): one context for the daemon's lifetime, hotplug, one event thread.
#pragma once

#include "backend.h"

#include <libusb.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace usbd
{
    class libusb_backend : public usb_backend
    {
    public:
        libusb_backend();
        ~libusb_backend() override;
        bool ok() const { return _ctx != nullptr; }

        void start( const events & ev ) override;
        void stop() override;
        std::vector< uint64_t > list() override;
        bool describe( uint64_t key, device_description & out ) override;
        void forget( uint64_t key ) override;
        // --dump: one-shot enumeration without hotplug or the event thread
        std::vector< device_description > enumerate_once();

        int open_anchor( uint64_t key, uint64_t & handle ) override { return open_handle( key, handle ); }
        void close_anchor( uint64_t handle ) override { close_handle( handle ); }
        int kernel_driver_active( uint64_t handle, uint8_t iface ) override;
        int capture( uint64_t handle, uint8_t iface ) override;
        int active_config( uint64_t handle, int & index ) override;
        int open_handle( uint64_t key, uint64_t & handle ) override;
        void close_handle( uint64_t handle ) override;
        int claim( uint64_t handle, uint8_t iface ) override;
        int release( uint64_t handle, uint8_t iface ) override;
        int clear_halt( uint64_t handle, uint8_t ep ) override;

        usb_transfer * alloc_transfer() override;
        void free_transfer( usb_transfer * t ) override;
        int submit( usb_transfer * t ) override;
        int cancel( usb_transfer * t ) override;

        uint64_t registry_id_of( uint64_t key ) override;
        bool interfaces_held_elsewhere( uint64_t registry_id ) override;
        int hand_back( const handback_target & t, unsigned poll_ms, unsigned wait_ms ) override;
        void force_reenumerate( uint64_t registry_id ) override;
        std::vector< live_device > live_devices() override;

    private:
        static int LIBUSB_CALL hotplug_cb( libusb_context *, libusb_device * dev, libusb_hotplug_event event, void * self );
        bool describe_device( libusb_device * dev, uint64_t key, device_description & out );
        libusb_device * device( uint64_t key );
        libusb_device_handle * handle( uint64_t h );

        libusb_context * _ctx = nullptr;
        events _ev;
        std::mutex _mutex;
        std::map< libusb_device *, uint64_t > _keys;
        std::map< uint64_t, libusb_device * > _devices;   // referenced
        std::map< uint64_t, libusb_device_handle * > _handles;
        uint64_t _next_key = 1, _next_handle = 1;
        libusb_hotplug_callback_handle _hotplug = 0;
        std::thread _events;
        int _stop = 0;
        std::atomic< bool > _started{ false };
    };
}
