// License: Apache 2.0. See LICENSE file in root directory.
// Fake peer policy for host tests (DESIGN 9.0.4). Test binaries only.
#pragma once

#include "daemon/policy.h"

#include <dispatch/dispatch.h>
#include <map>
#include <mutex>

namespace usbd
{
    class fake_policy : public peer_policy
    {
    public:
        fake_policy();
        // The next peer that connects; afterwards peers default to the console uid with a GUI session.
        void set_next_peer( uid_t euid, bool asid_ok );
        void set_console( uid_t uid, bool notify = true );   // bumps the generation and reports a console change
        void set_camera_allowed( bool allowed, bool notify = true );   // reports a camera change
        void set_reject_signature( bool reject );  // the next peer behaves as a code-signing reject (messages dropped)
        void set_reject_all_signatures( bool reject );   // every later peer does (a non-allowlisted client build)

        bool prepare( xpc_connection_t peer ) override;
        bool deliver( xpc_connection_t peer ) override;
        uid_t peer_uid( xpc_connection_t peer ) override;
        pid_t peer_pid( xpc_connection_t peer ) override;
        rsusb_auth hello( xpc_connection_t peer ) override;
        bool camera_allowed() override;
        console_state console() override;
        void forget( xpc_connection_t peer ) override;
        void set_change_handler( std::function< void( change ) > handler ) override;

    private:
        struct peer_info { uid_t euid; bool asid_ok, rejected; };
        peer_info info( xpc_connection_t peer );
        std::mutex _mutex;
        std::map< xpc_connection_t, peer_info > _peers;
        bool _next_set = false;
        peer_info _next{ 501, true, false };
        console_state _console{ 501, 1 };
        bool _camera = true;
        bool _reject_all = false;
        std::function< void( change ) > _handler;
        dispatch_queue_t _q;
    };
}
