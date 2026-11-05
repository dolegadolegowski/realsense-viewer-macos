// License: Apache 2.0. See LICENSE file in root directory.
// H6: the core with the mock backend behind the REAL peer policy (policy-macos), console uid = the tester.
//
//   rs-usbd-testd --agent <service> --config <usbd.plist> [--extra-dir <usbd.d>] [--fixtures <dir>] [--lightweight]
//       per-user LaunchAgent mode (MachServices <service>); the config's AllowedCDHashes and the lists in the extra
//       directory (DESIGN 5.5, owner checks off) admit clients
//   rs-usbd-testd --client <service>
//       client: HELLO over the Mach service, prints what it observes (reply or error, elapsed ms)
//   rs-usbd-testd --inprocess [--fixtures <dir>]
//       no launchd: anonymous listener + client in this process (the peer is this binary, so the requirement is
//       built from this binary's own cdhash or from a foreign one) and records what client and daemon observe for:
//       own cdhash, foreign cdhash, a 256-hash requirement, the lightweight-requirement fallback, the own cdhash only
//       in a usbd.d list (and that list removed), and which list the real peer_cdhash attributes the peer to

#include "daemon/core/server.h"
#include "daemon/core/xpc-util.h"
#include "daemon/policy-macos.h"
#include "tests/backend-mock.h"
#include "rs-usb-broker-protocol.h"

#include <Security/Security.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

using namespace usbd;

namespace
{
    std::string g_fixtures = "macos/broker/tests/fixtures";

    std::string own_cdhash()
    {
        SecCodeRef code = nullptr;
        SecStaticCodeRef st = nullptr;
        CFDictionaryRef info = nullptr;
        std::string rv;
        if( SecCodeCopySelf( kSecCSDefaultFlags, &code ) == errSecSuccess
            && SecCodeCopyStaticCode( code, kSecCSDefaultFlags, &st ) == errSecSuccess
            && SecCodeCopySigningInformation( st, kSecCSDefaultFlags, &info ) == errSecSuccess )
        {
            auto h = static_cast< CFDataRef >( CFDictionaryGetValue( info, kSecCodeInfoUnique ) );
            for( CFIndex i = 0; h && i < CFDataGetLength( h ); ++i )
            {
                char b[3];
                snprintf( b, sizeof( b ), "%02x", CFDataGetBytePtr( h )[i] );
                rv += b;
            }
        }
        if( info )
            CFRelease( info );
        if( st )
            CFRelease( st );
        if( code )
            CFRelease( code );
        return rv;
    }

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
        std::string peer_cdhash( xpc_object_t message ) override
        {
            std::string h = _p->peer_cdhash( message );
            std::lock_guard< std::mutex > lock( _m );
            last_cdhash = h;
            return h;
        }
        std::string recorded_cdhash()
        {
            std::lock_guard< std::mutex > lock( _m );
            return last_cdhash;
        }
        bool camera_allowed() override { return _p->camera_allowed(); }
        console_state console() override { return _p->console(); }
        void set_change_handler( std::function< void( change ) > h ) override { _p->set_change_handler( std::move( h ) ); }
        std::atomic< bool > prepared{ false };
        std::atomic< int > hellos{ 0 };
        std::atomic< int > last_auth{ -1 };

    private:
        std::unique_ptr< macos_policy > _p;
        std::mutex _m;
        std::string last_cdhash;
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
            uint64_t auth = 99;
            get_u64( r, RSUSB_KEY_AUTH, auth );
            rv = "reply auth=" + std::to_string( auth );
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
        std::string extra_dir;   // usbd.d to merge (5.5), or none
    };

    // a usbd.d directory holding one list `name` with `hashes`, or none if `hashes` is empty
    std::string make_extra_dir( const std::string & name, const std::vector< std::string > & hashes )
    {
        char tmpl[] = "/private/tmp/rs-usbd-testd.XXXXXX";
        std::string dir = std::string( mkdtemp( tmpl ) ) + "/usbd.d";
        mkdir( dir.c_str(), 0755 );
        chmod( dir.c_str(), 0755 );
        if( hashes.empty() )
            return dir;
        std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>Version</key><integer>1</integer>"
                        "<key>Name</key><string>" + name + "</string><key>BuildID</key><string>" + std::string( 64, 'e' )
                      + "</string><key>BuildNumber</key><integer>1</integer><key>ProtocolVersion</key><integer>1</integer>"
                        "<key>AllowedCDHashes</key><array>";
        for( auto & h : hashes )
            s += "<string>" + h + "</string>";
        std::ofstream( dir + "/" + name + ".plist" ) << s << "</array></dict></plist>";
        chmod( ( dir + "/" + name + ".plist" ).c_str(), 0644 );
        return dir;
    }

    int inprocess()
    {
        std::string self = own_cdhash();
        printf( "own cdhash: %s\n", self.empty() ? "(unsigned)" : self.c_str() );
        if( self.size() != 40 )
            return 1;
        std::string foreign( 40, '7' );
        std::vector< std::string > big;
        for( int i = 0; i < 255; ++i )
        {
            char b[41];
            snprintf( b, sizeof( b ), "%040x", i + 1 );
            big.push_back( b );
        }
        big.push_back( self );
        printf( "256-hash requirement string: %zu bytes\n", cdhash_requirement( big ).size() );
        const std::string list_name = "io.github.example.testd";
        std::string with_list = make_extra_dir( list_name, { self } ), list_removed = make_extra_dir( list_name, {} );
        experiment ex[] = { { "own cdhash", { self }, false, "" },
                            { "foreign cdhash", { foreign }, false, "" },
                            { "256 hashes incl. own", big, false, "" },
                            { "256 hashes, own missing", std::vector< std::string >( big.begin(), big.end() - 1 ), false, "" },
                            { "lightweight, own", { self }, true, "" },
                            { "lightweight, foreign", { foreign }, true, "" },
                            { "lightweight, 256 incl. own", big, true, "" },
                            { "own cdhash only in usbd.d", { foreign }, false, with_list },
                            { "usbd.d list removed", { foreign }, false, list_removed } };
        int failures = 0;
        for( auto & e : ex )
        {
            config c;
            c.build_id = std::string( 64, 'a' );
            c.cdhashes = e.hashes;
            std::vector< std::string > problems;
            apply_extra_lists( c, e.extra_dir.empty() ? std::vector< extra_list >() : load_extra_lists( e.extra_dir, false, problems ), problems );
            for( auto & p : problems )
                printf( "    allowlist %s skipped\n", p.c_str() );
            macos_policy_options o;
            o.cdhashes = c.admitted_cdhashes;
            o.lightweight = e.lightweight;
            o.force_console_uid = getuid();
            o.queue = dispatch_queue_create( "testd.policy", DISPATCH_QUEUE_SERIAL );
            auto rec = new recording_policy( std::make_unique< macos_policy >( o ) );
            usbd_core core( std::make_unique< mock_backend >( fixtures() ), std::unique_ptr< peer_policy >( rec ), c, timing(), temp_paths() );
            core.start( false );
            xpc_connection_t listener = xpc_connection_create( nullptr, dispatch_queue_create( "testd.listener", DISPATCH_QUEUE_SERIAL ) );
            xpc_endpoint_t ep = xpc_endpoint_create( listener );
            core.attach_listener( listener );
            xpc_connection_t c1 = xpc_connection_create_from_endpoint( ep );
            uint64_t ms;
            std::string seen = client_hello( c1, ms );
            bool own = false;
            for( auto & h : c.admitted_cdhashes )
                own |= h == self;
            printf( "[%s] requirement set: %s; client: %s after %llu ms; daemon: HELLO handled %d time(s)%s\n", e.name,
                    rec->prepared ? "yes" : "NO", seen.c_str(), (unsigned long long)ms, rec->hellos.load(),
                    rec->hellos ? ( " (auth " + std::to_string( rec->last_auth.load() ) + ")" ).c_str() : " (message dropped)" );
            bool ok = own ? seen.rfind( "reply", 0 ) == 0 : ( seen == "XPC_ERROR_CONNECTION_INTERRUPTED" && rec->hellos == 0 );
            if( own )
            {
                // the admitted line is logged after the reply: wait for the policy's peer_cdhash call
                for( int i = 0; i < 100 && rec->recorded_cdhash().empty(); ++i )
                    usleep( 10000 );
                std::string h = rec->recorded_cdhash(), by = lists_admitting( c, h );
                printf( "    daemon: peer cdhash from the HELLO message %s, admitted by %s\n", h.empty() ? "(none)" : h.c_str(),
                        by.empty() ? "an unknown list" : by.c_str() );
                ok = ok && h == self && by == ( e.extra_dir.empty() ? "usbd.plist" : list_name );
            }
            if( ! own )
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

    int agent( const std::string & service, const std::string & cfg_path, const std::string & extra_dir, bool lightweight )
    {
        config c;
        std::string err;
        if( ! load_config( cfg_path, false, c, err ) )
            return fprintf( stderr, "config: %s\n", err.c_str() ), 1;
        std::vector< std::string > problems;
        apply_extra_lists( c, extra_dir.empty() ? std::vector< extra_list >() : load_extra_lists( extra_dir, false, problems ), problems );
        for( auto & p : problems )
            fprintf( stderr, "allowlist %s skipped\n", p.c_str() );
        macos_policy_options o;
        o.cdhashes = c.admitted_cdhashes;
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
    std::string mode, service, cfg, extra_dir;
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
        else if( a == "--extra-dir" && i + 1 < argc )
            extra_dir = argv[++i];
        else if( a == "--fixtures" && i + 1 < argc )
            g_fixtures = argv[++i];
        else if( a == "--lightweight" )
            lightweight = true;
    }
    if( mode == "--inprocess" )
        return inprocess();
    if( mode == "--agent" && ! cfg.empty() )
        return agent( service, cfg, extra_dir, lightweight );
    if( mode == "--client" )
        return client( service );
    fprintf( stderr, "usage: rs-usbd-testd --inprocess | --agent <service> --config <plist> [--extra-dir <dir>] [--lightweight] | --client <service>\n" );
    return 64;
}
