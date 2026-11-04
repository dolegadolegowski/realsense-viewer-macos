// License: Apache 2.0. See LICENSE file in root directory.
// usbd_core: the backend-independent broker (sessions, registry, leases, transfers, policy hooks).
#pragma once

#include "../backend.h"
#include "../policy.h"
#include "config.h"
#include "state.h"

#include <xpc/xpc.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace usbd
{
    struct timing
    {
        unsigned grace_ms = 5000;
        unsigned idle_exit_ms = 60000;
        unsigned new_timeout_ms = RSUSB_NEW_TIMEOUT_MS;
        unsigned watchdog_ms = RSUSB_WATCHDOG_MS;
        unsigned handback_poll_ms = 100;
        unsigned handback_wait_ms = 5000;
        unsigned barrier_every = RSUSB_BARRIER_EVERY;
        unsigned max_unacked = RSUSB_MAX_UNACKED_SENDS;
        unsigned barrier_stall_ms = RSUSB_BARRIER_STALL_MS;
        static timing from( const config & c )
        {
            timing t;
            t.grace_ms = c.grace_s * 1000;
            t.idle_exit_ms = c.idle_exit_s * 1000;
            return t;
        }
    };

    struct core_stats
    {
        unsigned connections = 0, ready = 0, devices = 0, anchors = 0, messengers = 0, claimed = 0, leases = 0,
                 grace_timers = 0, captured = 0, requests = 0;
        uint64_t shm_bytes = 0, inflight_bytes = 0;
    };

    class session;   // one client connection (opaque outside the core)

    class usbd_core
    {
    public:
        usbd_core( std::unique_ptr< usb_backend > backend, std::unique_ptr< peer_policy > policy, const config & cfg,
                   const timing & t, const paths & p );
        ~usbd_core();
        usbd_core( const usbd_core & ) = delete;
        usbd_core & operator=( const usbd_core & ) = delete;

        void start( bool sweep_in_background = true );
        void attach_listener( xpc_connection_t listener );   // Mach service listener or an anonymous one
        void shutdown();   // SIGTERM path (2.7.8); blocks up to a few seconds
        bool idle();       // idle-exit predicate (2.7.9)
        core_stats stats();
        void sweep();      // startup sweep (2.7.7), synchronous

        usb_backend & backend();
        peer_policy & policy();

        // Fuzz / unit seam: a session without an XPC peer whose sends go to `sink` (replies are plain dictionaries).
        // `barrier`, if set, stands in for xpc_connection_send_barrier: it receives the acknowledgement to run (or hold).
        std::shared_ptr< session > open_session( std::function< void( xpc_object_t ) > sink,
                                                 std::function< void( std::function< void() > ack ) > barrier = nullptr );
        void session_message( const std::shared_ptr< session > & s, xpc_object_t msg );   // waits until handled
        void close_session( const std::shared_ptr< session > & s );

        class impl;

    private:
        std::shared_ptr< impl > _impl;
    };
}
