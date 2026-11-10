// License: Apache 2.0. See LICENSE file in root directory.
// Real peer policy (DESIGN 3.2, 13): console user + GUI session, MDM / Screen Time allowCamera, and the optional
// cdhash requirement of an administrator who restricted the service to a list of programs.
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
        // AllowedCDHashes. Empty (the default): no code-signing requirement, every program goes on to the user and
        // camera checks. Not empty: only programs with one of these cdhashes get an answer.
        std::vector< std::string > cdhashes;
        // The daemon's own cdhash (running_cdhash()), admitted on top of a non-empty list: the installer verifies an
        // installation with the installed binary's --probe, which must be answered whatever the administrator listed.
        // Empty: nothing is added (tests, where the peer is the test binary itself).
        std::string daemon_cdhash;
        bool lightweight = false;   // xpc_connection_set_peer_lightweight_code_requirement instead (3.2.1 fallback)
        std::string managed_root = "/Library/Managed Preferences";
        uid_t force_console_uid = uid_t( -1 );   // rs-usbd-testd only
        dispatch_queue_t queue = nullptr;        // main queue of the daemon
    };

    // cdhash of the code this process runs (40 lowercase hex), read from its signature; empty when it cannot be read
    std::string running_cdhash();
    // The cdhashes of the peer requirement: none for an empty administrator list (no requirement at all), otherwise
    // the list (at most 256 entries) followed by `daemon_cdhash`, unless that is empty, malformed or already listed
    std::vector< std::string > required_cdhashes( const std::vector< std::string > & allowed,
                                                  const std::string & daemon_cdhash );
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
        std::vector< std::string > _required;   // required_cdhashes(); empty: no requirement
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
