// License: Apache 2.0. See LICENSE file in root directory.
// H6: the core with the mock backend behind the REAL peer policy (policy-macos), console uid = the tester.
//
//   rs-usbd-testd --agent <service> --config <usbd.plist> [--fixtures <dir>] [--lightweight]
//       per-user LaunchAgent mode (MachServices <service>); a config without AllowedCDHashes admits every program,
//       one with the list only the programs on it (this tool does not add its own cdhash as realsense-usbd does:
//       its client is the same binary)
//   rs-usbd-testd --client <service>
//       client: HELLO over the Mach service, prints what it observes (reply or error, elapsed ms)
//   rs-usbd-testd --inprocess [--fixtures <dir>]
//       no launchd: anonymous listener + client in this process (the peer is this binary, so the requirement is
//       built from this binary's own cdhash or from a foreign one) and records what client and daemon observe for:
//       no list (the default: no requirement), own cdhash, foreign cdhash, a 256-hash requirement, the
//       lightweight-requirement fallback, and a foreign list with the daemon's own entry on top (what realsense-usbd
//       does so that its own --probe is always admitted; 256 + 1 hashes at most)
// --fixtures defaults to the tests/fixtures folder next to the sources this binary was built from.

#include "daemon/core/server.h"
#include "daemon/core/xpc-util.h"
#include "daemon/policy-macos.h"
#include "tests/backend-mock.h"
#include "rs-usb-broker-protocol.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <string>

using namespace usbd;

#ifndef RSUSBD_FIXTURES
#define RSUSBD_FIXTURES "tests/fixtures"   // build-daemon.sh passes the absolute path
#endif

namespace
{
    std::string g_fixtures = RSUSBD_FIXTURES;

    // Wraps the real policy to record what the daemon side observes
    class recording_policy : public peer_policy
    {
    public:
        explicit recording_policy( std::unique_ptr< macos_policy > p ) : _p( std::move( p ) ) {}
        bool prepare( xpc_connection_t peer ) override { return prepared = _p->prepare( peer ); }
        uid_t peer_uid( xpc_connection_t peer ) override { return _p->peer_uid( peer ); }
        pid_t peer_pid( xpc_connection_t peer ) override { return _p->peer_pid( peer ); }
        rsusb_auth hello( xpc_connection_t peer ) override
        {
            ++hellos;
            rsusb_auth a = _p->hello( peer );
            last_auth = int( a );
            return a;
        }
        bool camera_allowed() override { return _p->camera_allowed(); }
        console_state console() override { return _p->console(); }
        void set_change_handler( std::function< void( change ) > h ) override { _p->set_change_handler( std::move( h ) ); }
        std::atomic< bool > prepared{ false };
        std::atomic< int > hellos{ 0 };
        std::atomic< int > last_auth{ -1 };

    private:
        std::unique_ptr< macos_policy > _p;
    };

    std::vector< fixture_device > fixtures()
    {
        std::vector< fixture_device > f;
        std::string err;
        if( ! load_fixture_file( g_fixtures + "/d455.plist", f, err ) )
        {
            fprintf( stderr, "fixtures: %s\n", err.c_str() );
            exit( 2 );
        }
        return f;
    }

    paths temp_paths()
    {
        char tmpl[] = "/private/tmp/rs-usbd-testd.XXXXXX";
        paths p;
        p.state_dir = std::string( mkdtemp( tmpl ) ) + "/state";
        p.require_root = false;
        return p;
    }

    // HELLO on `c`; prints and returns a short description of what the client saw
    std::string client_hello( xpc_connection_t c, uint64_t & ms )
    {
        xpc_connection_set_event_handler( c, ^( xpc_object_t ) {} );
        xpc_connection_resume( c );
        xpc_object_t m = xpc_dictionary_create( nullptr, nullptr, 0 );
        xpc_dictionary_set_uint64( m, RSUSB_KEY_OP, RSUSB_OP_HELLO );
        xpc_dictionary_set_uint64( m, RSUSB_KEY_VERSION, RSUSB_PROTOCOL_VERSION );
        uint64_t t0 = now_ms();
        xpc_object_t r = xpc_connection_send_message_with_reply_sync( c, m );
        ms = now_ms() - t0;
        xpc_release( m );
        std::string rv;
        if( r == XPC_ERROR_CONNECTION_INVALID )
            rv = "XPC_ERROR_CONNECTION_INVALID";
        else if( r == XPC_ERROR_CONNECTION_INTERRUPTED )
            rv = "XPC_ERROR_CONNECTION_INTERRUPTED";
        else if( r == XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT )
            rv = "XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT";
        else if( xpc_get_type( r ) == XPC_TYPE_DICTIONARY )
        {
            uint64_t auth = 99, sv = 0;
            get_u64( r, RSUSB_KEY_AUTH, auth );
            get_u64( r, RSUSB_KEY_SERVICE_VERSION, sv );
            rv = "reply auth=" + std::to_string( auth ) + " sv=" + std::to_string( sv );
        }
        else
            rv = "other";
        xpc_release( r );
        return rv;
    }

    struct experiment
    {
        const char * name;
        std::vector< std::string > hashes;
        bool lightweight;
        bool daemon_entry = false;   // the daemon's own cdhash on top of the list (macos_policy_options::daemon_cdhash)
    };

    int inprocess()
    {
        std::string self = running_cdhash();   // the function realsense-usbd reads its own cdhash with
        printf( "own cdhash: %s\n", self.empty() ? "(unsigned)" : self.c_str() );
        if( self.size() != 40 )
            return 1;
        std::string foreign( 40, '7' );
        std::vector< std::string > big, foreign256;
        for( int i = 0; i < 256; ++i )
        {
            char b[41];
            snprintf( b, sizeof( b ), "%040x", i + 1 );
            foreign256.push_back( b );
        }
        big.assign( foreign256.begin(), foreign256.end() - 1 );
        big.push_back( self );
        printf( "256-hash requirement string: %zu bytes; with the daemon's own entry on top of 256: %zu bytes\n",
                cdhash_requirement( big ).size(), cdhash_requirement( required_cdhashes( foreign256, self ) ).size() );
        experiment ex[] = { { "no list", {}, false },
                            { "no list, lightweight", {}, true },
                            { "own cdhash", { self }, false },
                            { "foreign cdhash", { foreign }, false },
                            { "256 hashes incl. own", big, false },
                            { "256 hashes, own missing", std::vector< std::string >( big.begin(), big.end() - 1 ), false },
                            { "lightweight, own", { self }, true },
                            { "lightweight, foreign", { foreign }, true },
                            { "lightweight, 256 incl. own", big, true },
                            { "foreign cdhash + the daemon's own entry", { foreign }, false, true },
                            { "own cdhash listed + the daemon's own entry", { self }, false, true },
                            { "256 foreign + the daemon's own entry", foreign256, false, true },
                            { "lightweight, foreign + the daemon's own entry", { foreign }, true, true },
                            { "lightweight, 256 foreign + the daemon's own entry", foreign256, true, true } };
        int failures = 0;
        for( auto & e : ex )
        {
            macos_policy_options o;
            o.cdhashes = e.hashes;
            if( e.daemon_entry )
                o.daemon_cdhash = self;
            o.lightweight = e.lightweight;
            o.force_console_uid = getuid();
            o.queue = dispatch_queue_create( "testd.policy", DISPATCH_QUEUE_SERIAL );
            auto rec = new recording_policy( std::make_unique< macos_policy >( o ) );
            config c;
            c.cdhashes = e.hashes;
            usbd_core core( std::make_unique< mock_backend >( fixtures() ), std::unique_ptr< peer_policy >( rec ), c, timing(), temp_paths() );
            core.start( false );
            xpc_connection_t listener = xpc_connection_create( nullptr, dispatch_queue_create( "testd.listener", DISPATCH_QUEUE_SERIAL ) );
            xpc_endpoint_t ep = xpc_endpoint_create( listener );
            core.attach_listener( listener );
            xpc_connection_t c1 = xpc_connection_create_from_endpoint( ep );
            uint64_t ms;
            std::string seen = client_hello( c1, ms );
            // no list: no requirement, every program gets its HELLO answered; with the daemon's own entry this
            // binary (the peer) is admitted whatever the list holds
            bool admitted = e.hashes.empty() || e.daemon_entry;
            for( auto & h : e.hashes )
                admitted |= h == self;
            printf( "[%s] requirement set: %s; client: %s after %llu ms; daemon: HELLO handled %d time(s)%s\n", e.name,
                    ! rec->prepared ? "NO" : e.hashes.empty() ? "none needed" : "yes", seen.c_str(), (unsigned long long)ms,
                    rec->hellos.load(),
                    rec->hellos ? ( " (auth " + std::to_string( rec->last_auth.load() ) + ")" ).c_str() : " (message dropped)" );
            bool ok = admitted ? seen.rfind( "reply", 0 ) == 0 : ( seen == "XPC_ERROR_CONNECTION_INTERRUPTED" && rec->hellos == 0 );
            if( ! admitted )
            {
                // does the daemon get an error for the rejected peer, or does the NEW timeout remove it?
                usleep( 100000 );
                unsigned at100 = core.stats().connections;
                uint64_t t0 = now_ms();
                while( core.stats().connections && now_ms() - t0 < 3000 )
                    usleep( 10000 );
                printf( "    daemon: connection still present after 100 ms: %s (gone at once = the daemon got "
                        "XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT and cancelled it); removed after %llu ms more (NEW timeout %u ms)\n",
                        at100 ? "yes" : "no", (unsigned long long)( now_ms() - t0 ), RSUSB_NEW_TIMEOUT_MS );
                ok = ok && core.stats().connections == 0;   // removed: at once (peer error event) or by the NEW timeout
            }
            if( ! ok )
                ++failures;
            xpc_connection_cancel( c1 );
            core.shutdown();
        }
        printf( "%s\n", failures ? "H6 in-process: UNEXPECTED results above" : "H6 in-process: all as expected" );
        _exit( failures ? 1 : 0 );
    }

    int agent( const std::string & service, const std::string & cfg_path, bool lightweight )
    {
        config c;
        std::string err;
        if( ! load_config( cfg_path, false, c, err ) )
            return fprintf( stderr, "config: %s\n", err.c_str() ), 1;
        macos_policy_options o;
        o.cdhashes = c.cdhashes;
        o.lightweight = lightweight;
        o.force_console_uid = getuid();
        o.queue = dispatch_get_main_queue();
        auto * core = new usbd_core( std::make_unique< mock_backend >( fixtures() ), std::make_unique< macos_policy >( o ), c,
                                     timing::from( c ), temp_paths() );
        core->start( true );
        core->attach_listener( xpc_connection_create_mach_service( service.c_str(), dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER ) );
        dispatch_main();
    }

    int client( const std::string & service )
    {
        uint64_t ms;
        std::string seen = client_hello( xpc_connection_create_mach_service( service.c_str(), nullptr, 0 ), ms );
        printf( "%s after %llu ms\n", seen.c_str(), (unsigned long long)ms );
        return seen.rfind( "reply auth=0", 0 ) == 0 ? 0 : 2;
    }
}

int main( int argc, char ** argv )
{
    setvbuf( stdout, nullptr, _IOLBF, 0 );
    std::string mode, service, cfg;
    bool lightweight = false;
    for( int i = 1; i < argc; ++i )
    {
        std::string a = argv[i];
        if( ( a == "--agent" || a == "--client" ) && i + 1 < argc )
            mode = a, service = argv[++i];
        else if( a == "--inprocess" )
            mode = a;
        else if( a == "--config" && i + 1 < argc )
            cfg = argv[++i];
        else if( a == "--fixtures" && i + 1 < argc )
            g_fixtures = argv[++i];
        else if( a == "--lightweight" )
            lightweight = true;
    }
    if( mode == "--inprocess" )
        return inprocess();
    if( mode == "--agent" && ! cfg.empty() )
        return agent( service, cfg, lightweight );
    if( mode == "--client" )
        return client( service );
    fprintf( stderr, "usage: rs-usbd-testd --inprocess | --agent <service> --config <plist> [--lightweight] | --client <service>\n" );
    return 64;
}
