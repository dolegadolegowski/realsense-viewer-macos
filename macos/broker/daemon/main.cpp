// License: Apache 2.0. See LICENSE file in root directory.
// realsense-usbd: RealSense USB broker for macOS (DESIGN 2.1).
//
//   realsense-usbd --daemon               launchd only (root, ppid 1, installed path, valid config)
//   realsense-usbd --probe                any user: HELLO as a client; exit 0 OK, 2 unreachable/rejected, 3 AUTH_USER,
//                                         4 AUTH_CAMERA_DISABLED, 5 AUTH_VERSION, 6 AUTH_LIMIT
//   realsense-usbd --dump                 any user: the ENUMERATE snapshot as a fixture plist (no device is opened)
//   realsense-usbd --handback-recorded    root, daemon stopped: hands recorded captured devices back to macOS

#include "backend-libusb.h"
#include "core/log.h"
#include "core/server.h"
#include "core/xpc-util.h"
#include "handback.h"
#include "policy-macos.h"
#include "rs-usb-broker-protocol.h"

#include <libproc.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace usbd;

extern char ** environ;

namespace
{
    const char * const INSTALLED_PATH = "/Library/PrivilegedHelperTools/io.github.dolegadolegowski.realsense-usbd";

    int refuse( const char * why )
    {
        log_fault( "conn", "realsense-usbd --daemon refused to start: %{public}s", why );
        return 0;   // KeepAlive SuccessfulExit=false: a clean exit is not restarted
    }

    int run_daemon()
    {
        while( environ && *environ )   // ignore the environment (libusb reads LIBUSB_DEBUG)
        {
            std::string kv = *environ;
            unsetenv( kv.substr( 0, kv.find( '=' ) ).c_str() );
        }
        char path[PROC_PIDPATHINFO_MAXSIZE] = {};
        if( geteuid() != 0 )
            return refuse( "not root" );
        if( getppid() != 1 )
            return refuse( "not started by launchd" );
        if( proc_pidpath( getpid(), path, sizeof( path ) ) <= 0 || strcmp( path, INSTALLED_PATH ) != 0 )
            return refuse( "not the installed binary" );
        umask( 077 );
        chdir( "/" );
        config cfg;
        std::string error;
        if( ! load_config( CONFIG_PATH, true, cfg, error ) )
            return refuse( ( "configuration: " + error ).c_str() );
        // Lists of other projects (5.5): an invalid one is skipped, never stops the daemon
        std::vector< std::string > problems;
        apply_extra_lists( cfg, load_extra_lists( EXTRA_DIR, true, problems ), problems );
        for( auto & pr : problems )
            log_fault( "policy", "allowlist %{public}s skipped", pr.c_str() );
        log_info( "policy", "allowlist usbd.plist: build %{public}lld, %{public}zu code hashes", (long long)cfg.build_number,
                  cfg.cdhashes.size() );
        for( auto & l : cfg.extra_lists )
        {
            log_info( "policy", "allowlist usbd.d/%{public}s: build %{public}lld, %{public}zu code hashes", l.name.c_str(),
                      (long long)l.build_number, l.cdhashes.size() );
            if( l.protocol != RSUSB_PROTOCOL_VERSION )
                log_info( "policy", "allowlist usbd.d/%{public}s: made for protocol %{public}lld, its programs get AUTH_VERSION",
                          l.name.c_str(), (long long)l.protocol );
        }
        log_info( "policy", "admitting %{public}zu code hashes", cfg.admitted_cdhashes.size() );

        paths p;
        state_store lock_store( p );
        if( ! lock_store.prepare_dir() || lock_store.lock( false ) < 0 )
            log_fault( "device", "state directory lock unavailable" );   // keep going: the state file is best effort

        auto backend = std::make_unique< libusb_backend >();
        if( ! backend->ok() )
            return refuse( "libusb_init failed" );
        macos_policy_options po;
        po.cdhashes = cfg.admitted_cdhashes;
        po.queue = dispatch_get_main_queue();
        auto * core = new usbd_core( std::move( backend ), std::make_unique< macos_policy >( po ), cfg, timing::from( cfg ), p );
        core->start( true );
        xpc_connection_t listener = xpc_connection_create_mach_service( RSUSB_SERVICE_NAME, dispatch_get_main_queue(),
                                                                        XPC_CONNECTION_MACH_SERVICE_LISTENER );
        core->attach_listener( listener );
        log_info( "conn", "realsense-usbd started (build %{public}lld)", (long long)cfg.build_number );

        signal( SIGTERM, SIG_IGN );
        dispatch_source_t term = dispatch_source_create( DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue() );
        dispatch_set_context( term, core );
        dispatch_source_set_event_handler_f( term, []( void * c ) {
            log_info( "conn", "SIGTERM: handing devices back and exiting" );
            static_cast< usbd_core * >( c )->shutdown();
            exit( 0 );
        } );
        dispatch_resume( term );

        static uint64_t idle_since = 0;
        static unsigned idle_exit_ms = cfg.idle_exit_s * 1000;
        dispatch_source_t idle = dispatch_source_create( DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue() );
        dispatch_source_set_timer( idle, dispatch_time( DISPATCH_TIME_NOW, NSEC_PER_SEC ), NSEC_PER_SEC, NSEC_PER_SEC / 10 );
        dispatch_set_context( idle, core );
        dispatch_source_set_event_handler_f( idle, []( void * c ) {
            if( ! static_cast< usbd_core * >( c )->idle() )
                idle_since = 0;
            else if( ! idle_since )
                idle_since = now_ms();
            else if( now_ms() - idle_since >= idle_exit_ms )
            {
                log_info( "conn", "idle: exiting" );
                exit( 0 );
            }
        } );
        dispatch_resume( idle );
        dispatch_main();
    }

    int run_probe()
    {
        dispatch_queue_t q = dispatch_queue_create( "probe", DISPATCH_QUEUE_SERIAL );
        uint64_t deadline = now_ms() + RSUSB_HELLO_TIMEOUT_MS;
        for( int attempt = 0; attempt < 4; ++attempt )
        {
            xpc_connection_t c = xpc_connection_create_mach_service( RSUSB_SERVICE_NAME, q, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED );
            xpc_connection_set_event_handler( c, ^( xpc_object_t ) {} );
            xpc_connection_resume( c );
            xpc_object_t hello = xpc_dictionary_create( nullptr, nullptr, 0 );
            xpc_dictionary_set_uint64( hello, RSUSB_KEY_OP, RSUSB_OP_HELLO );
            xpc_dictionary_set_uint64( hello, RSUSB_KEY_VERSION, RSUSB_PROTOCOL_VERSION );
            __block xpc_object_t reply = nullptr;
            dispatch_semaphore_t sem = dispatch_semaphore_create( 0 );
            xpc_connection_send_message_with_reply( c, hello, q, ^( xpc_object_t r ) {
                reply = xpc_retain( r );
                dispatch_semaphore_signal( sem );
            } );
            xpc_release( hello );
            uint64_t now = now_ms();
            bool got = now < deadline
                    && dispatch_semaphore_wait( sem, dispatch_time( DISPATCH_TIME_NOW, int64_t( deadline - now ) * NSEC_PER_MSEC ) ) == 0;
            xpc_connection_cancel( c );
            if( ! got )
                return printf( "unreachable: no answer\n" ), 2;
            if( reply == XPC_ERROR_CONNECTION_INTERRUPTED && now_ms() < deadline )
                continue;
            if( xpc_get_type( reply ) != XPC_TYPE_DICTIONARY )
                return printf( "unreachable: not installed, turned off, or this build is not allowlisted\n" ), 2;
            int64_t st;
            uint64_t pv, auth;
            xpc_object_t b = get_typed( reply, RSUSB_KEY_BUILD, XPC_TYPE_STRING );
            if( ! get_i64( reply, RSUSB_KEY_STATUS, st ) || ! get_u64( reply, RSUSB_KEY_VERSION, pv ) || ! get_u64( reply, RSUSB_KEY_AUTH, auth ) || st != 0 )
                return printf( "rejected: invalid reply\n" ), 2;
            switch( auth )
            {
            case RSUSB_AUTH_OK:
                if( pv != RSUSB_PROTOCOL_VERSION )
                    return printf( "protocol version %llu\n", (unsigned long long)pv ), 5;
                return printf( "ok %s\n", b ? xpc_string_get_string_ptr( b ) : "" ), 0;
            case RSUSB_AUTH_USER: return printf( "not the console user\n" ), 3;
            case RSUSB_AUTH_CAMERA_DISABLED: return printf( "camera disabled by policy\n" ), 4;
            case RSUSB_AUTH_VERSION: return printf( "protocol version %llu\n", (unsigned long long)pv ), 5;
            case RSUSB_AUTH_LIMIT: return printf( "too many clients\n" ), 6;
            default: return printf( "rejected: auth %llu\n", (unsigned long long)auth ), 2;
            }
        }
        return printf( "unreachable: interrupted\n" ), 2;
    }

    int run_dump()
    {
        libusb_backend b;
        if( ! b.ok() )
            return fprintf( stderr, "libusb_init failed\n" ), 1;
        std::vector< fixture_device > out;
        for( auto & d : b.enumerate_once() )
        {
            if( ! d.complete || ! device_allowed( d.snap ) )
                continue;
            fixture_device f;
            f.snap = d.snap;
            f.registry_id = d.registry_id;
            f.drivers_attached = d.drivers_attached;
            f.active_config = d.active_config;
            out.push_back( f );
        }
        std::stable_sort( out.begin(), out.end(), []( const fixture_device & a, const fixture_device & b ) { return a.registry_id < b.registry_id; } );
        fputs( fixture_xml( out ).c_str(), stdout );
        return 0;
    }

    int run_handback_recorded()
    {
        if( geteuid() != 0 )
            return fprintf( stderr, "--handback-recorded must run as root\n" ), 1;
        umask( 077 );
        paths p;
        state_store store( p );
        struct stat st;
        if( lstat( p.state_dir.c_str(), &st ) != 0 )
            return printf( "nothing recorded\n" ), 0;
        if( ! store.prepare_dir() )
            return printf( "state directory unusable\n" ), 0;
        int fd = store.lock( true );
        if( fd < 0 )
            return fprintf( stderr, "realsense-usbd is running (state lock held)\n" ), 1;
        std::vector< captured_entry > recorded;
        store.read( recorded );
        auto live = iokit_live_devices();
        auto selected = sweep_select( recorded, live );
        std::vector< captured_entry > keep;
        for( auto & e : recorded )
        {
            if( std::find( selected.begin(), selected.end(), e ) == selected.end() )
            {
                printf( "%04x:%04x registry 0x%llx: not captured or not present\n", e.vid, e.pid, (unsigned long long)e.registry_id );
                continue;
            }
            if( iokit_interfaces_held_elsewhere( e.registry_id ) )
            {
                printf( "%04x:%04x registry 0x%llx: in use by another process, skipped\n", e.vid, e.pid, (unsigned long long)e.registry_id );
                keep.push_back( e );
                continue;
            }
            handback_target t;
            t.registry_id = e.registry_id;
            t.vid = e.vid;
            t.pid = e.pid;
            int r = iokit_hand_back( t, 100, 10000 );
            int drivers = -1;
            for( auto & l : iokit_live_devices() )
                if( l.vid == e.vid && l.pid == e.pid && l.registry_id != e.registry_id )
                    drivers = l.drivers_attached;
            printf( "%04x:%04x registry 0x%llx: %s%s\n", e.vid, e.pid, (unsigned long long)e.registry_id,
                    r == 0 ? "handed back" : "hand-back failed", r == 0 ? ( drivers == 1 ? ", drivers attached" : ", drivers not attached yet" ) : "" );
            if( r )
                keep.push_back( e );
        }
        store.set_entries( keep );
        close( fd );
        return 0;
    }
}

int main( int argc, char ** argv )
{
    std::string mode = argc == 2 ? argv[1] : "";
    if( mode == "--daemon" )
        return run_daemon();
    if( mode == "--probe" )
        return run_probe();
    if( mode == "--dump" )
        return run_dump();
    if( mode == "--handback-recorded" )
        return run_handback_recorded();
    fprintf( stderr, "usage: realsense-usbd --daemon | --probe | --dump | --handback-recorded\n" );
    return 64;
}
