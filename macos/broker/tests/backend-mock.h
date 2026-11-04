// License: Apache 2.0. See LICENSE file in root directory.
// Deterministic mock USB backend driven by fixtures (DESIGN 9.0.3, 9.0.4). Test binaries only.
#pragma once

#include "daemon/backend.h"

#include <condition_variable>
#include <dispatch/dispatch.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace usbd
{
    class mock_backend : public usb_backend
    {
    public:
        enum class op { open, detach, claim, release, control, bulk, submit, cancel, clear_halt, hand_back };
        struct counters_t
        {
            unsigned anchors = 0, handles = 0, claimed = 0, live_transfers = 0, captures = 0, force_reenumerations = 0;
            std::vector< uint64_t > hand_backs;   // registry IDs
        };

        explicit mock_backend( std::vector< fixture_device > fixtures );
        ~mock_backend() override;

        // Test controls; `tok` is the core's token (= the mock's key, 1, 2, ... in fixture order).
        void unplug( uint64_t tok );
        uint64_t plug( size_t fixture_index );
        // control/bulk: `status` is the transfer status (xs) of the next completions; other ops: the libusb error returned
        void fail_next( uint64_t tok, op o, int status, int count = 1 );
        void hold_completions( uint64_t tok, uint8_t ep, bool hold );   // ep 0: control transfers
        void wedge( uint64_t tok, op o );
        void unwedge();
        void set_held_elsewhere( uint64_t tok, uint8_t iface, bool held );
        void set_drivers_attached( uint64_t tok, bool attached );
        void set_incomplete( uint64_t tok, bool incomplete );   // describe() reports an unreadable configuration
        void set_stream_period( uint64_t tok, uint8_t ep, unsigned ms );
        counters_t counters();
        std::vector< std::string > out_log();
        uint32_t last_timeout();
        uint64_t registry_id( uint64_t tok );
        uint64_t last_plugged();

        // usb_backend
        void start( const events & ev ) override;
        void stop() override;
        std::vector< uint64_t > list() override;
        bool describe( uint64_t key, device_description & out ) override;
        void forget( uint64_t key ) override;
        int open_anchor( uint64_t key, uint64_t & handle ) override;
        void close_anchor( uint64_t handle ) override;
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
        struct xfer;
        struct mdev;
        struct mhandle
        {
            uint64_t key = 0;
            bool anchor = false;
            std::set< uint8_t > claimed;
        };
        int open( uint64_t key, bool anchor, uint64_t & handle );
        int injected( uint64_t key, op o );   // 0 or the injected status (lock held)
        int wait_wedge( std::unique_lock< std::mutex > & lock, uint64_t key, op o );
        void schedule( xfer * x, unsigned delay_ms, int xs, uint32_t n );
        void complete( xfer * x, uint64_t gen, int xs, uint32_t n );
        void abort_all( uint64_t key, int xs, bool control, int ep );   // lock held
        uint64_t add_device( size_t fixture_index, bool drivers );     // lock held
        void fill_in( mdev & d, xfer & x, uint32_t & n );               // lock held

        std::vector< fixture_device > _fixtures;
        std::mutex _mutex;
        std::condition_variable _cv;
        std::map< uint64_t, std::unique_ptr< mdev > > _devs;
        std::map< uint64_t, mhandle > _handles;
        std::set< xfer * > _inflight;
        std::map< std::pair< uint64_t, op >, std::pair< int, int > > _fail;
        std::set< std::pair< uint64_t, op > > _wedged;
        std::vector< std::string > _log;
        counters_t _counters;
        uint32_t _last_timeout = 0;
        uint64_t _next_key = 1, _next_handle = 1, _last_plugged = 0;
        events _ev;
        dispatch_queue_t _q;
        bool _started = false;
    };
}
