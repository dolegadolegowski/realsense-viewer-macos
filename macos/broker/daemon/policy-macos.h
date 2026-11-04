// License: Apache 2.0. See LICENSE file in root directory.
// Real peer policy (DESIGN 3.2): cdhash requirement, console user + GUI session, MDM / Screen Time allowCamera.
#pragma once

#include "policy.h"

#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>

#include <mutex>
#include <string>
#include <vector>

namespace usbd
{
    struct macos_policy_options
    {
        std::vector< std::string > cdhashes;
        bool lightweight = false;   // xpc_connection_set_peer_lightweight_code_requirement instead (3.2.1 fallback)
        std::string managed_root = "/Library/Managed Preferences";
        uid_t force_console_uid = uid_t( -1 );   // rs-usbd-testd only
        dispatch_queue_t queue = nullptr;        // main queue of the daemon
    };

    std::string cdhash_requirement( const std::vector< std::string > & cdhashes );
    xpc_object_t cdhash_lightweight_requirement( const std::vector< std::string > & cdhashes );
    // 3.2.5 for one console user name (empty: only the global file); false = deny
    bool camera_allowed_in( const std::string & managed_root, const std::string & user );

    class macos_policy : public peer_policy
    {
    public:
        explicit macos_policy( const macos_policy_options & o );
        ~macos_policy() override;
        bool prepare( xpc_connection_t peer ) override;
        uid_t peer_uid( xpc_connection_t peer ) override;
        pid_t peer_pid( xpc_connection_t peer ) override;
        rsusb_auth hello( xpc_connection_t peer ) override;
        bool camera_allowed() override;
        console_state console() override;
        void set_change_handler( std::function< void( change ) > handler ) override;

    private:
        void read_console();
        void arm_watches();
        static void store_changed( SCDynamicStoreRef, CFArrayRef, void * info );

        macos_policy_options _o;
        std::string _requirement;
        xpc_object_t _lwcr = nullptr;
        std::mutex _mutex;
        console_state _console;
        std::string _user;
        std::function< void( change ) > _handler;
        SCDynamicStoreRef _store = nullptr;
        std::vector< dispatch_source_t > _watches;
    };
}
