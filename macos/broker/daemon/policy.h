// License: Apache 2.0. See LICENSE file in root directory.
// Peer policy used by the core (DESIGN 3.2.6). Real implementation: policy-macos; fake: tests/policy-fake.
#pragma once

#include "rs-usb-broker-protocol.h"

#include <sys/types.h>
#include <xpc/xpc.h>

#include <cstdint>
#include <functional>
#include <string>

namespace usbd
{
    struct console_state
    {
        uid_t uid = uid_t( -1 );
        uint64_t gen = 0;
    };

    class peer_policy
    {
    public:
        enum class change { console, camera };
        virtual ~peer_policy() = default;
        virtual bool prepare( xpc_connection_t peer ) = 0;   // sets the code-signing requirement, before resume
        virtual bool deliver( xpc_connection_t ) { return true; }   // fake only: simulates a signature reject
        virtual uid_t peer_uid( xpc_connection_t peer ) = 0;
        virtual pid_t peer_pid( xpc_connection_t peer ) = 0;
        virtual rsusb_auth hello( xpc_connection_t peer ) = 0;   // user and camera checks (3.2.2, 3.2.5)
        // cdhash (40 lowercase hex) of the process that sent `message`, "" if unknown. Diagnostics only (which list
        // admitted a peer, 2.10): admission is decided by the code-signing requirement (3.2.1).
        virtual std::string peer_cdhash( xpc_object_t /*message*/ ) { return {}; }
        virtual bool camera_allowed() = 0;
        virtual console_state console() = 0;
        virtual void forget( xpc_connection_t ) {}
        // Called on the main queue after the console user or the camera restriction changed.
        virtual void set_change_handler( std::function< void( change ) > handler ) = 0;
    };
}
