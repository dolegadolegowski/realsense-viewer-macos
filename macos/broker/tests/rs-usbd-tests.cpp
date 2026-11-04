// License: Apache 2.0. See LICENSE file in root directory.
// Host tests of realsense-usbd's core against the mock backend and the fake policy (DESIGN 9.1 H2, H4 daemon side).
// Every case runs in its own child process.
//
//   rs-usbd-tests [--fixtures <dir>] [--jobs N]          run every case
//   rs-usbd-tests [--fixtures <dir>] --case <name>       run one case in this process
//   rs-usbd-tests [--fixtures <dir>] --stress <seconds>  H4 daemon side: concurrent load against the mock
//   rs-usbd-tests --list
// Built with librealsense2 (-DRSUSBD_H3) it also runs the h3_* cases: librealsense's broker client against the core.

#include "daemon/core/config.h"
#include "daemon/core/hwm.h"
#include "daemon/core/server.h"
#include "daemon/core/xpc-util.h"
#include "backend-mock.h"
#include "policy-fake.h"
#include "rs-usb-broker-protocol.h"

#ifdef RSUSBD_H3
#include "libusb/broker/broker-connection.h"
#include "libusb/interface-libusb.h"
#include "usb/usb-enumerator.h"
#include <librealsense2/rs.h>
#endif

#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <thread>

using namespace usbd;
extern char ** environ;

#define CHECK( c )                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if( ! ( c ) )                                                                                                  \
        {                                                                                                              \
            fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c );                                  \
            _exit( 1 );                                                                                                \
        }                                                                                                              \
    } while( 0 )
#define CHECK_EQ( a, b )                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        long long _a = (long long)( a ), _b = (long long)( b );                                                        \
        if( _a != _b )                                                                                                 \
        {                                                                                                              \
            fprintf( stderr, "%s:%d: CHECK_EQ failed: %s = %lld, %s = %lld\n", __FILE__, __LINE__, #a, _a, #b, _b );   \
            _exit( 1 );                                                                                                \
        }                                                                                                              \
    } while( 0 )

namespace
{
    std::string g_fixtures = "macos/broker/tests/fixtures";

    void sleep_ms( unsigned ms ) { usleep( ms * 1000 ); }

    template< class F >
    bool wait_for( F && f, unsigned ms = 3000 )
    {
        for( unsigned t = 0; t < ms; t += 5 )
        {
            if( f() )
                return true;
            sleep_ms( 5 );
        }
        return f();
    }

    std::vector< fixture_device > load( const std::vector< std::string > & files )
    {
        std::vector< fixture_device > rv;
        for( auto & f : files )
        {
            std::string err;
            if( ! load_fixture_file( g_fixtures + "/" + f, rv, err ) )
            {
                fprintf( stderr, "fixture %s: %s\n", f.c_str(), err.c_str() );
                _exit( 2 );
            }
        }
        return rv;
    }

    config test_config()
    {
        config c;
        c.build_id = std::string( 64, 'a' );
        c.build_number = 202611041200;
        c.cdhashes = { std::string( 40, '0' ) };
        c.daemon_cdhash = c.cdhashes[0];
        return c;
    }

    timing fast()
    {
        timing t;
        t.grace_ms = 300;
        t.new_timeout_ms = 300;
        t.watchdog_ms = 600;
        t.handback_poll_ms = 10;
        t.handback_wait_ms = 50;
        return t;
    }

    std::string temp_base()   // $TMPDIR (per-user on macOS), else /private/tmp
    {
        const char * t = getenv( "TMPDIR" );
        std::string b = t && *t ? t : "/private/tmp";
        while( b.size() > 1 && b.back() == '/' )
            b.pop_back();
        return b;
    }

    std::string temp_dir()
    {
        std::string tmpl = temp_base() + "/rs-usbd-tests.XXXXXX";
        CHECK( mkdtemp( &tmpl[0] ) );
        return tmpl;
    }

    struct env
    {
        mock_backend * mock;
        fake_policy * pol;
        usbd_core * core;
        xpc_connection_t listener;
        xpc_endpoint_t endpoint;
        std::string state;

        explicit env( const std::vector< std::string > & files, timing t = fast(), config c = test_config(),
                      std::function< void( env & ) > before_start = nullptr )
            : env( load( files ), t, c, before_start )
        {
        }

        env( std::vector< fixture_device > devices, timing t, config c, std::function< void( env & ) > before_start )
        {
            mock = new mock_backend( std::move( devices ) );
            pol = new fake_policy;
            paths p;
            state = temp_dir() + "/state";
            p.state_dir = state;
            p.require_root = false;
            core = new usbd_core( std::unique_ptr< usb_backend >( mock ), std::unique_ptr< peer_policy >( pol ), c, t, p );
            if( before_start )
                before_start( *this );
            core->start( false );
            core->sweep();
            listener = xpc_connection_create( nullptr, dispatch_queue_create( "test.listener", DISPATCH_QUEUE_SERIAL ) );
            endpoint = xpc_endpoint_create( listener );
            core->attach_listener( listener );
        }
    };

    xpc_object_t dict( std::initializer_list< std::pair< const char *, uint64_t > > kv )
    {
        xpc_object_t d = xpc_dictionary_create( nullptr, nullptr, 0 );
        for( auto & p : kv )
            xpc_dictionary_set_uint64( d, p.first, p.second );
        return d;
    }

    struct pending
    {
        std::mutex m;
        std::condition_variable cv;
        xpc_object_t reply = nullptr;
        bool wait( unsigned ms )
        {
            std::unique_lock< std::mutex > l( m );
            return cv.wait_for( l, std::chrono::milliseconds( ms ), [&] { return reply != nullptr; } );
        }
    };

    int64_t status( xpc_object_t r )
    {
        int64_t st;
        if( xpc_get_type( r ) != XPC_TYPE_DICTIONARY || ! get_i64( r, RSUSB_KEY_STATUS, st ) )
            return 1000;   // not a reply (connection error)
        return st;
    }

    uint64_t u64( xpc_object_t r, const char * k )
    {
        uint64_t v = 0;
        CHECK( get_u64( r, k, v ) );
        return v;
    }

    struct client_state
    {
        std::mutex m;
        std::condition_variable cv;
        std::deque< xpc_object_t > events;
        std::atomic< bool > invalid{ false };
    };

    struct client
    {
        xpc_connection_t c;
        std::shared_ptr< client_state > s = std::make_shared< client_state >();
        std::mutex & m = s->m;
        std::condition_variable & cv = s->cv;
        std::deque< xpc_object_t > & events = s->events;
        std::atomic< bool > & invalid = s->invalid;
        dispatch_queue_t q;

        explicit client( env & e, bool do_hello = true )
        {
            q = dispatch_queue_create( "test.client", DISPATCH_QUEUE_SERIAL );
            c = xpc_connection_create_from_endpoint( e.endpoint );
            xpc_connection_set_target_queue( c, q );
            std::shared_ptr< client_state > st = s;
            xpc_connection_set_event_handler( c, ^( xpc_object_t ev ) {
                if( xpc_get_type( ev ) == XPC_TYPE_DICTIONARY )
                {
                    std::lock_guard< std::mutex > l( st->m );
                    st->events.push_back( xpc_retain( ev ) );
                    st->cv.notify_all();
                }
                else   // the daemon cancelling an anonymous peer shows up as INTERRUPTED here, INVALID after our cancel
                {
                    std::lock_guard< std::mutex > l( st->m );
                    st->invalid = true;
                    st->cv.notify_all();
                }
            } );
            xpc_connection_resume( c );
            if( do_hello )
                CHECK_EQ( hello(), RSUSB_AUTH_OK );
        }
        ~client() { xpc_connection_cancel( c ); }

        xpc_object_t call( xpc_object_t msg )
        {
            xpc_object_t r = xpc_connection_send_message_with_reply_sync( c, msg );
            xpc_release( msg );
            return r;
        }

        int64_t st( xpc_object_t msg )
        {
            xpc_object_t r = call( msg );
            int64_t s = status( r );
            xpc_release( r );
            return s;
        }

        std::shared_ptr< pending > send( xpc_object_t msg, dispatch_queue_t reply_q = nullptr )
        {
            auto p = std::make_shared< pending >();
            xpc_connection_send_message_with_reply( c, msg, reply_q ? reply_q : dispatch_get_global_queue( 0, 0 ), ^( xpc_object_t r ) {
                std::lock_guard< std::mutex > l( p->m );
                p->reply = xpc_retain( r );
                p->cv.notify_all();
            } );
            xpc_release( msg );
            return p;
        }

        uint64_t hello( uint64_t pv = RSUSB_PROTOCOL_VERSION )
        {
            xpc_object_t r = call( dict( { { RSUSB_KEY_OP, RSUSB_OP_HELLO }, { RSUSB_KEY_VERSION, pv } } ) );
            if( status( r ) != 0 )
                return 1000;
            uint64_t a = u64( r, RSUSB_KEY_AUTH );
            CHECK_EQ( u64( r, RSUSB_KEY_VERSION ), RSUSB_PROTOCOL_VERSION );
            CHECK( get_typed( r, RSUSB_KEY_BUILD, XPC_TYPE_STRING ) );
            xpc_release( r );
            return a;
        }

        int64_t lease( uint64_t tok ) { return st( dict( { { RSUSB_KEY_OP, RSUSB_OP_LEASE }, { RSUSB_KEY_TOKEN, tok } } ) ); }

        int64_t open( uint64_t tok, uint64_t iface, uint64_t * h = nullptr )
        {
            xpc_object_t r = call( dict( { { RSUSB_KEY_OP, RSUSB_OP_OPEN }, { RSUSB_KEY_TOKEN, tok }, { RSUSB_KEY_INTERFACE, iface } } ) );
            int64_t s = status( r );
            if( s == 0 && h )
                *h = u64( r, RSUSB_KEY_HANDLE );
            if( s != 0 && xpc_get_type( r ) == XPC_TYPE_DICTIONARY )
                CHECK( ! xpc_dictionary_get_value( r, RSUSB_KEY_HANDLE ) );
            xpc_release( r );
            return s;
        }

        int64_t close( uint64_t h ) { return st( dict( { { RSUSB_KEY_OP, RSUSB_OP_CLOSE }, { RSUSB_KEY_HANDLE, h } } ) ); }

        static xpc_object_t control_msg( uint64_t h, uint64_t rt, uint64_t rq, uint64_t val, uint64_t idx, uint64_t len, uint64_t to,
                                         const std::vector< uint8_t > * data = nullptr )
        {
            xpc_object_t m = dict( { { RSUSB_KEY_OP, RSUSB_OP_CONTROL }, { RSUSB_KEY_HANDLE, h }, { RSUSB_KEY_REQUEST_TYPE, rt },
                                     { RSUSB_KEY_REQUEST, rq }, { RSUSB_KEY_VALUE, val }, { RSUSB_KEY_INDEX, idx },
                                     { RSUSB_KEY_LENGTH, len }, { RSUSB_KEY_TIMEOUT, to } } );
            if( data )
                xpc_dictionary_set_data( m, RSUSB_KEY_DATA, data->data(), data->size() );
            return m;
        }

        int64_t control( uint64_t h, uint64_t rt, uint64_t rq, uint64_t val, uint64_t idx, uint64_t len, uint64_t to = 100,
                         const std::vector< uint8_t > * data = nullptr, std::vector< uint8_t > * in = nullptr )
        {
            xpc_object_t r = call( control_msg( h, rt, rq, val, idx, len, to, data ) );
            int64_t s = status( r );
            if( s == 0 )
            {
                uint64_t n = u64( r, RSUSB_KEY_TRANSFERRED );
                const uint8_t * p;
                size_t dn;
                int has = get_data( r, RSUSB_KEY_DATA, p, dn );
                CHECK_EQ( has, ( rt & 0x80 ) ? 1 : 0 );
                if( has == 1 )
                {
                    CHECK_EQ( dn, n );
                    if( in )
                        in->assign( p, p + dn );
                }
            }
            else if( s != 1000 )
                CHECK( ! xpc_dictionary_get_value( r, RSUSB_KEY_TRANSFERRED ) && ! xpc_dictionary_get_value( r, RSUSB_KEY_DATA ) );
            xpc_release( r );
            return s;
        }

        static xpc_object_t bulk_msg( uint64_t h, uint64_t ep, uint64_t len, uint64_t to, const std::vector< uint8_t > * data = nullptr )
        {
            xpc_object_t m = dict( { { RSUSB_KEY_OP, RSUSB_OP_BULK }, { RSUSB_KEY_HANDLE, h }, { RSUSB_KEY_ENDPOINT, ep },
                                     { RSUSB_KEY_LENGTH, len }, { RSUSB_KEY_TIMEOUT, to } } );
            if( data )
                xpc_dictionary_set_data( m, RSUSB_KEY_DATA, data->data(), data->size() );
            return m;
        }

        int64_t bulk( uint64_t h, uint64_t ep, uint64_t len, uint64_t to = 100, const std::vector< uint8_t > * data = nullptr,
                      std::vector< uint8_t > * in = nullptr )
        {
            xpc_object_t r = call( bulk_msg( h, ep, len, to, data ) );
            int64_t s = status( r );
            if( s == 0 && in )
            {
                const uint8_t * p;
                size_t dn;
                CHECK_EQ( get_data( r, RSUSB_KEY_DATA, p, dn ), 1 );
                in->assign( p, p + dn );
            }
            xpc_release( r );
            return s;
        }

        int64_t reset_ep( uint64_t h, uint64_t ep ) { return st( dict( { { RSUSB_KEY_OP, RSUSB_OP_RESET_EP }, { RSUSB_KEY_HANDLE, h }, { RSUSB_KEY_ENDPOINT, ep } } ) ); }

        int64_t req_create( uint64_t h, uint64_t ep, uint64_t len, uint64_t * r = nullptr, uint8_t ** map = nullptr )
        {
            xpc_object_t rep = call( dict( { { RSUSB_KEY_OP, RSUSB_OP_REQ_CREATE }, { RSUSB_KEY_HANDLE, h }, { RSUSB_KEY_ENDPOINT, ep }, { RSUSB_KEY_LENGTH, len } } ) );
            int64_t s = status( rep );
            if( s == 0 )
            {
                if( r )
                    *r = u64( rep, RSUSB_KEY_REQUEST_ID );
                xpc_object_t shm = get_typed( rep, RSUSB_KEY_SHMEM, XPC_TYPE_SHMEM );
                CHECK( shm );
                void * addr = nullptr;
                size_t sz = xpc_shmem_map( shm, &addr );
                CHECK( sz >= len );
                if( map )
                    *map = static_cast< uint8_t * >( addr );
                else
                    munmap( addr, sz );
            }
            xpc_release( rep );
            return s;
        }

        int64_t req( uint64_t op, uint64_t r ) { return st( dict( { { RSUSB_KEY_OP, op }, { RSUSB_KEY_REQUEST_ID, r } } ) ); }
        void destroy( uint64_t r )
        {
            xpc_object_t m = dict( { { RSUSB_KEY_OP, RSUSB_OP_REQ_DESTROY }, { RSUSB_KEY_REQUEST_ID, r } } );
            xpc_connection_send_message( c, m );
            xpc_release( m );
        }

        std::vector< std::pair< uint64_t, snapshot > > enumerate()
        {
            xpc_object_t r = call( dict( { { RSUSB_KEY_OP, RSUSB_OP_ENUMERATE } } ) );
            CHECK_EQ( status( r ), 0 );
            xpc_object_t devs = get_typed( r, RSUSB_KEY_DEVICES, XPC_TYPE_ARRAY );
            CHECK( devs );
            std::vector< std::pair< uint64_t, snapshot > > rv;
            for( size_t i = 0; i < xpc_array_get_count( devs ); ++i )
            {
                snapshot s;
                uint64_t tok;
                CHECK( snapshot_from_xpc( xpc_array_get_value( devs, i ), s, tok ) );
                rv.emplace_back( tok, s );
            }
            xpc_release( r );
            return rv;
        }

        // next event matching ev (others are kept); nullptr on timeout
        xpc_object_t event( uint64_t ev, unsigned ms = 2000, uint64_t r = 0 )
        {
            std::unique_lock< std::mutex > l( m );
            xpc_object_t found = nullptr;
            cv.wait_for( l, std::chrono::milliseconds( ms ), [&] {
                for( auto it = events.begin(); it != events.end(); ++it )
                {
                    uint64_t e = 0, rr = 0;
                    get_u64( *it, RSUSB_KEY_EVENT, e );
                    get_u64( *it, RSUSB_KEY_REQUEST_ID, rr );
                    if( e == ev && ( ! r || r == rr ) )
                    {
                        found = *it;
                        events.erase( it );
                        return true;
                    }
                }
                return false;
            } );
            return found;
        }

        bool wait_invalid( unsigned ms )   // the connection ended (cancelled by the daemon)
        {
            std::unique_lock< std::mutex > l( m );
            return cv.wait_for( l, std::chrono::milliseconds( ms ), [&] { return invalid.load(); } );
        }

        void cancel() { xpc_connection_cancel( c ); }
    };

    snapshot fixture_snap( const std::string & file, size_t i ) { return load( { file } )[i].snap; }

    bool same( const snapshot & a, const snapshot & b )
    {
        if( a.path != b.path || a.dd != b.dd || a.ifs.size() != b.ifs.size() )
            return false;
        for( size_t i = 0; i < a.ifs.size(); ++i )
        {
            if( a.ifs[i].cfg != b.ifs[i].cfg || a.ifs[i].eps != b.ifs[i].eps || a.ifs[i].alts.size() != b.ifs[i].alts.size() )
                return false;
            for( size_t k = 0; k < a.ifs[i].alts.size(); ++k )
                if( a.ifs[i].alts[k].d != b.ifs[i].alts[k].d || a.ifs[i].alts[k].x != b.ifs[i].alts[k].x )
                    return false;
        }
        return true;
    }

    std::vector< uint8_t > hwm_packet( uint8_t opcode, uint32_t p1 = 0, uint32_t p2 = 0, size_t len = 1024, uint8_t m0 = 0xAB, uint8_t m1 = 0xCD )
    {
        std::vector< uint8_t > p( len, 0 );
        const uint8_t head[5] = { 20, 0, m0, m1, opcode };
        for( size_t i = 0; i < len && i < 5; ++i )
            p[i] = head[i];
        for( size_t i = 0; i < 4 && len >= 16; ++i )
        {
            p[8 + i] = uint8_t( p1 >> ( 8 * i ) );
            p[12 + i] = uint8_t( p2 >> ( 8 * i ) );
        }
        return p;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Cases

    std::map< std::string, std::function< void() > > & cases()
    {
        static std::map< std::string, std::function< void() > > c;
        return c;
    }
    struct reg
    {
        reg( const char * n, std::function< void() > f ) { cases()[n] = std::move( f ); }
    };
#define CASE( name )                                                                                                   \
    static void case_##name();                                                                                         \
    static reg reg_##name( #name, case_##name );                                                                       \
    static void case_##name()

    CASE( hello_ok )
    {
        env e( { "d455.plist" } );
        client a( e );
        CHECK_EQ( e.core->stats().ready, 1 );
    }

    CASE( hello_wrong_version )
    {
        env e( { "d455.plist" } );
        client a( e, false );
        CHECK_EQ( a.hello( 99 ), RSUSB_AUTH_VERSION );
        CHECK( a.wait_invalid( 1000 ) );
    }

    CASE( hello_rules )
    {
        env e( { "d455.plist" } );
        {   // op before HELLO
            client a( e, false );
            CHECK_EQ( a.st( dict( { { RSUSB_KEY_OP, RSUSB_OP_ENUMERATE } } ) ), 1000 );
            CHECK( a.wait_invalid( 1000 ) );
        }
        {   // second HELLO
            client a( e );
            CHECK_EQ( a.hello(), 1000 );
            CHECK( a.wait_invalid( 1000 ) );
        }
        {   // HELLO without pv
            client a( e, false );
            CHECK_EQ( a.st( dict( { { RSUSB_KEY_OP, RSUSB_OP_HELLO } } ) ), 1000 );
        }
        {   // missing op
            client a( e, false );
            CHECK_EQ( a.st( dict( { { RSUSB_KEY_VERSION, 1 } } ) ), 1000 );
        }
        {   // unknown op
            client a( e );
            CHECK_EQ( a.st( dict( { { RSUSB_KEY_OP, 99 } } ) ), 1000 );
            CHECK( a.wait_invalid( 1000 ) );
        }
        {   // wrong type for `if`
            client a( e );
            xpc_object_t m = dict( { { RSUSB_KEY_OP, RSUSB_OP_OPEN }, { RSUSB_KEY_TOKEN, 1 } } );
            xpc_dictionary_set_string( m, RSUSB_KEY_INTERFACE, "0" );
            CHECK_EQ( a.st( m ), 1000 );
        }
        {   // wrong type for `val` (int64 instead of uint64)
            client a( e );
            uint64_t h;
            CHECK_EQ( a.open( 1, 0, &h ), 0 );
            xpc_object_t m = client::control_msg( h, 0xA1, 0x81, 0, 0, 4, 100 );
            xpc_dictionary_set_int64( m, RSUSB_KEY_VALUE, 0x100 );
            CHECK_EQ( a.st( m ), 1000 );
        }
        {   // wrong type for data
            client a( e );
            uint64_t h;
            CHECK( wait_for( [&] { return a.open( 1, 0, &h ) == 0; } ) );
            xpc_object_t m = client::control_msg( h, 0x21, 0x01, 0x0200, 0, 4, 100 );
            xpc_dictionary_set_string( m, RSUSB_KEY_DATA, "abcd" );
            CHECK_EQ( a.st( m ), 1000 );
        }
        CHECK( wait_for( [&] { return e.core->stats().connections == 0; } ) );
    }

    CASE( new_timeout_and_signature_reject )
    {
        // As libxpc does for a peer failing the code-signing requirement (H6): its messages are dropped, so the
        // reply right dies (client: INTERRUPTED at once) and the NEW timeout removes the connection.
        env e( { "d455.plist" } );
        e.pol->set_reject_signature( true );
        client a( e, false );
        uint64_t t0 = now_ms();
        auto p = a.send( dict( { { RSUSB_KEY_OP, RSUSB_OP_HELLO }, { RSUSB_KEY_VERSION, 1 } } ) );
        CHECK( p->wait( 3000 ) );
        CHECK( p->reply == XPC_ERROR_CONNECTION_INTERRUPTED );
        CHECK_EQ( e.core->stats().connections, 1 );
        CHECK( wait_for( [&] { return e.core->stats().connections == 0; } ) );
        uint64_t dt = now_ms() - t0;
        CHECK( dt >= 250 && dt < 1000 );   // new_timeout_ms = 300
        CHECK( a.wait_invalid( 1000 ) );
        // a peer that never sends anything is not visible to the daemon at all (XPC connects lazily)
    }

    CASE( new_cap_per_uid )
    {
        timing t = fast();
        t.new_timeout_ms = 3000;
        env e( { "d455.plist" }, t );
        std::vector< std::unique_ptr< client > > cs;
        for( int i = 0; i < 5; ++i )
        {
            e.pol->set_reject_signature( true );   // stays NEW
            cs.emplace_back( new client( e, false ) );
            cs.back()->send( dict( { { RSUSB_KEY_OP, RSUSB_OP_HELLO }, { RSUSB_KEY_VERSION, 1 } } ) );
            CHECK( wait_for( [&] { return e.core->stats().connections == unsigned( std::min( i + 1, 5 ) ) || cs.back()->invalid; } ) );
        }
        CHECK( cs[4]->wait_invalid( 1000 ) );
        for( int i = 0; i < 4; ++i )
            CHECK( ! cs[i]->invalid );
        e.pol->set_next_peer( 502, true );   // another uid is not affected by uid 501's NEW connections
        client other( e, false );
        CHECK_EQ( other.hello(), RSUSB_AUTH_USER );
    }

    CASE( ready_limit )
    {
        env e( { "d455.plist" } );
        std::vector< std::unique_ptr< client > > cs;
        for( unsigned i = 0; i < RSUSB_MAX_CLIENTS; ++i )
            cs.emplace_back( new client( e ) );
        client extra( e, false );
        CHECK_EQ( extra.hello(), RSUSB_AUTH_LIMIT );
        CHECK( extra.wait_invalid( 1000 ) );
        cs[0]->cancel();
        CHECK( wait_for( [&] { return e.core->stats().ready == RSUSB_MAX_CLIENTS - 1; } ) );
        client again( e );
    }

    CASE( hello_policy )
    {
        env e( { "d455.plist" } );
        e.pol->set_next_peer( 502, true );
        client other_user( e, false );
        CHECK_EQ( other_user.hello(), RSUSB_AUTH_USER );
        e.pol->set_next_peer( 501, false );
        client no_gui( e, false );
        CHECK_EQ( no_gui.hello(), RSUSB_AUTH_USER );
        e.pol->set_next_peer( 0, false );
        client root( e, false );
        CHECK_EQ( root.hello(), RSUSB_AUTH_OK );
        e.pol->set_console( 0 );   // loginwindow-like console uid: no user peer
        client user( e, false );
        CHECK_EQ( user.hello(), RSUSB_AUTH_USER );
        e.pol->set_console( 501 );
        e.pol->set_camera_allowed( false );
        client cam( e, false );
        CHECK_EQ( cam.hello(), RSUSB_AUTH_CAMERA_DISABLED );
        CHECK( cam.wait_invalid( 1000 ) );
    }

    CASE( enumerate_fixtures )
    {
        env e( { "two-d455.plist", "filtered.plist", "recovery.plist" } );
        client a( e );
        auto list = a.enumerate();
        CHECK_EQ( list.size(), 3 );   // the webcam, the hub and the device with a boot keyboard are hidden
        CHECK_EQ( list[0].first, 2 );   // RegistryID 0x...A00
        CHECK_EQ( list[1].first, 1 );   // 0x...B00
        CHECK_EQ( list[2].first, 6 );   // recovery 0x...F00
        CHECK( same( list[0].second, fixture_snap( "two-d455.plist", 1 ) ) );
        CHECK( same( list[1].second, fixture_snap( "two-d455.plist", 0 ) ) );
        CHECK( same( list[2].second, fixture_snap( "recovery.plist", 0 ) ) );
        CHECK_EQ( a.open( 3, 0 ), RSUSB_ST_NO_DEVICE );   // filtered devices have no token
        CHECK_EQ( a.lease( 5 ), RSUSB_ST_NO_DEVICE );
    }

    CASE( incomplete_snapshot )
    {
        env e( { "d455.plist" }, fast(), test_config(), []( env & x ) { x.mock->set_incomplete( 1, true ); } );
        client a( e );
        CHECK_EQ( a.enumerate().size(), 0 );
        e.mock->set_incomplete( 1, false );
        sleep_ms( 1100 );   // refresh at most once per second from ENUMERATE
        CHECK_EQ( a.enumerate().size(), 1 );
    }

    CASE( lease )
    {
        env e( { "d455.plist" } );
        client a( e );
        CHECK_EQ( a.lease( 1 ), 0 );
        CHECK_EQ( a.lease( 1 ), 0 );
        CHECK_EQ( e.core->stats().leases, 1 );
        CHECK_EQ( a.lease( 99 ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( a.lease( 0 ), RSUSB_ST_NO_DEVICE );
        client b( e );
        e.pol->set_camera_allowed( false, false );   // changed, not yet reported
        CHECK_EQ( b.lease( 1 ), RSUSB_ST_ACCESS );
        CHECK_EQ( b.open( 1, 0 ), RSUSB_ST_ACCESS );
        e.pol->set_camera_allowed( true, false );
        // NO_DEVICE while the hand-back runs
        e.mock->wedge( 1, mock_backend::op::hand_back );
        uint64_t h;
        CHECK_EQ( b.open( 1, 0, &h ), 0 );
        CHECK_EQ( b.close( h ), 0 );
        a.cancel();
        b.cancel();
        client c( e );
        CHECK( wait_for( [&] { return c.enumerate().empty(); } ) );
        CHECK_EQ( c.lease( 1 ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( c.open( 1, 0 ), RSUSB_ST_NO_DEVICE );
        e.mock->unwedge();
        CHECK( wait_for( [&] { auto l = c.enumerate(); return l.size() == 1 && l[0].first == 2; } ) );
        CHECK_EQ( e.mock->counters().hand_backs.size(), 1 );
    }

    CASE( open_claim_set )
    {
        env e( { "d455.plist" } );
        client a( e ), b( e );
        uint64_t h0, h3, h5;
        CHECK_EQ( a.open( 1, 0, &h0 ), 0 );
        auto c = e.mock->counters();
        CHECK_EQ( c.anchors, 1 );
        CHECK_EQ( c.handles, 1 );
        CHECK_EQ( c.claimed, 3 );   // VC 0 + VS 1, 2
        CHECK_EQ( c.captures, 1 );
        CHECK_EQ( a.open( 1, 3, &h3 ), 0 );
        CHECK_EQ( e.mock->counters().claimed, 5 );
        CHECK_EQ( a.open( 1, 5, &h5 ), 0 );
        CHECK_EQ( e.mock->counters().claimed, 6 );
        CHECK( h0 < h3 && h3 < h5 );
        CHECK_EQ( a.open( 1, 0 ), RSUSB_ST_ACCESS );   // same connection
        CHECK_EQ( a.open( 1, 1 ), RSUSB_ST_ACCESS );   // VS held by the VC messenger
        CHECK_EQ( b.open( 1, 4 ), RSUSB_ST_ACCESS );   // other connection
        CHECK_EQ( a.open( 1, 7 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.open( 1, 300 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.open( 99, 0 ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( a.close( h3 ), 0 );
        CHECK_EQ( b.open( 1, 3 ), 0 );
        c = e.mock->counters();
        CHECK_EQ( c.captures, 1 );   // capture once per device instance
        CHECK_EQ( c.anchors, 1 );
        CHECK_EQ( a.close( h3 ), RSUSB_ST_NOT_FOUND );
    }

    CASE( open_busy_and_all_or_nothing )
    {
        env e( { "d455.plist" } );
        client a( e );
        uint64_t h;
        e.mock->fail_next( 1, mock_backend::op::claim, RSUSB_ST_BUSY, 2 );
        CHECK_EQ( a.open( 1, 5, &h ), 0 );
        CHECK_EQ( a.close( h ), 0 );
        e.mock->fail_next( 1, mock_backend::op::claim, RSUSB_ST_BUSY, 3 );
        CHECK_EQ( a.open( 1, 5 ), RSUSB_ST_BUSY );
        e.mock->set_held_elsewhere( 1, 2, true );   // the third interface of the claim set fails
        CHECK_EQ( a.open( 1, 0 ), RSUSB_ST_ACCESS );
        auto c = e.mock->counters();
        CHECK_EQ( c.claimed, 0 );
        CHECK_EQ( c.handles, 0 );
        CHECK_EQ( e.core->stats().claimed, 0 );
        e.mock->set_held_elsewhere( 1, 2, false );
        e.mock->fail_next( 1, mock_backend::op::open, RSUSB_ST_IO, 1 );
        CHECK_EQ( a.open( 1, 0 ), RSUSB_ST_IO );
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
    }

    CASE( capture_refused )
    {
        env e( { "d455.plist" } );
        client a( e );
        e.mock->fail_next( 1, mock_backend::op::detach, RSUSB_ST_ACCESS, 1 );
        CHECK_EQ( a.open( 1, 0 ), RSUSB_ST_ACCESS );
        CHECK_EQ( e.mock->counters().anchors, 0 );
        e.mock->fail_next( 1, mock_backend::op::detach, RSUSB_ST_IO, 1 );   // other errors: the claim is attempted
        uint64_t h;
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
        CHECK_EQ( e.mock->counters().captures, 0 );
    }

    // While the first OPEN captures the device it stays listed and leasable; an OPEN queued behind the capture runs after it
    CASE( capture_keeps_device_listed )
    {
        timing t = fast();
        t.watchdog_ms = 30000;
        env e( { "d455.plist" }, t );
        client a( e ), b( e );
        e.mock->wedge( 1, mock_backend::op::detach );
        auto first = a.send( dict( { { RSUSB_KEY_OP, RSUSB_OP_OPEN }, { RSUSB_KEY_TOKEN, 1 }, { RSUSB_KEY_INTERFACE, 0 } } ) );
        sleep_ms( 200 );
        CHECK( ! first->wait( 0 ) );
        auto list = b.enumerate();
        CHECK( list.size() == 1 && list[0].first == 1 );
        CHECK_EQ( b.lease( 1 ), 0 );
        auto second = b.send( dict( { { RSUSB_KEY_OP, RSUSB_OP_OPEN }, { RSUSB_KEY_TOKEN, 1 }, { RSUSB_KEY_INTERFACE, 3 } } ) );
        e.mock->unwedge();
        CHECK( first->wait( 3000 ) && status( first->reply ) == 0 );
        CHECK( second->wait( 3000 ) && status( second->reply ) == 0 );
        CHECK_EQ( e.mock->counters().captures, 1 );
        CHECK_EQ( e.core->stats().messengers, 2 );
    }

    CASE( handback_after_grace_uses_reread_registry_id )
    {
        env e( { "d455.plist" } );
        uint64_t before = e.mock->registry_id( 1 );
        {
            client a( e );
            uint64_t h;
            CHECK_EQ( a.open( 1, 0, &h ), 0 );
            std::ifstream f( e.state + "/captured.plist" );   // recorded with the registry ID re-read after the capture
            std::string text( ( std::istreambuf_iterator< char >( f ) ), std::istreambuf_iterator< char >() );
            CHECK( text.find( "<integer>" + std::to_string( before + 0x100 ) + "</integer>" ) != std::string::npos );
            CHECK( text.find( "<integer>" + std::to_string( before ) + "</integer>" ) == std::string::npos );
            CHECK_EQ( a.close( h ), 0 );
            sleep_ms( 600 );
            CHECK_EQ( e.mock->counters().hand_backs.size(), 0 );   // CLOSE never hands back while the lease lives
            a.cancel();
        }
        CHECK( wait_for( [&] { return e.mock->counters().hand_backs.size() == 1; } ) );
        CHECK_EQ( e.mock->counters().hand_backs[0], before + 0x100 );
        CHECK_EQ( e.core->stats().captured, 0 );
        CHECK( wait_for( [&] {   // entry removed after the hand-back
            std::ifstream f( e.state + "/captured.plist" );
            std::string text( ( std::istreambuf_iterator< char >( f ) ), std::istreambuf_iterator< char >() );
            return ! text.empty() && text.find( "<dict>" ) == std::string::npos;
        } ) );
        client b( e );
        CHECK( wait_for( [&] { auto l = b.enumerate(); return l.size() == 1 && l[0].first == 2; } ) );
        b.cancel();
        CHECK( wait_for( [&] { return e.core->idle(); } ) );
    }

    CASE( grace_cancelled_by_lease_and_open )
    {
        env e( { "d455.plist" } );
        for( int variant = 0; variant < 2; ++variant )
        {
            auto a = std::make_unique< client >( e );
            uint64_t h;
            CHECK( wait_for( [&] { return a->open( 1, 0, &h ) == 0; } ) );
            a->cancel();
            CHECK( wait_for( [&] { return e.core->stats().grace_timers == 1; } ) );
            client b( e );
            if( variant == 0 )
                CHECK_EQ( b.lease( 1 ), 0 );
            else
                CHECK_EQ( b.open( 1, 3 ), 0 );
            sleep_ms( 800 );
            CHECK_EQ( e.mock->counters().hand_backs.size(), 0 );
            CHECK_EQ( e.mock->counters().captures, 1 );   // anchor and capture reused
            b.cancel();
            CHECK( wait_for( [&] { return e.core->stats().connections == 0 && e.core->stats().grace_timers == 1; } ) );
            CHECK( wait_for( [&] { return e.mock->counters().hand_backs.size() == 0 && e.core->stats().messengers == 0; } ) );
            client c( e );
            CHECK_EQ( c.lease( 1 ), 0 );   // keep it captured for the next variant
            c.cancel();
            CHECK( wait_for( [&] { return e.core->stats().connections == 0; } ) );
        }
        CHECK( wait_for( [&] { return e.mock->counters().hand_backs.size() == 1; } ) );
    }

    CASE( held_elsewhere_skips_and_retries )
    {
        env e( { "d455.plist" } );
        {
            client a( e );
            uint64_t h;
            CHECK_EQ( a.open( 1, 0, &h ), 0 );
            CHECK_EQ( a.close( h ), 0 );
            e.mock->set_held_elsewhere( 1, 0, true );
            a.cancel();
        }
        sleep_ms( 1000 );
        CHECK_EQ( e.mock->counters().hand_backs.size(), 0 );
        CHECK( e.core->stats().grace_timers == 1 || ! e.core->idle() );
        e.mock->set_held_elsewhere( 1, 0, false );
        CHECK( wait_for( [&] { return e.mock->counters().hand_backs.size() == 1; } ) );
    }

    CASE( close_waits_and_parks_open )
    {
        env e( { "d455.plist" } );
        client a( e ), b( e );
        uint64_t h0, hb;
        CHECK_EQ( a.open( 1, 0, &h0 ), 0 );
        CHECK_EQ( b.open( 1, 3, &hb ), 0 );
        e.mock->set_stream_period( 1, 0x82, 100000 );
        uint64_t r;
        CHECK_EQ( a.req_create( h0, 0x82, 4096, &r ), 0 );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r ), 0 );
        e.mock->hold_completions( 1, 0, true );
        auto ctl_a = a.send( client::control_msg( h0, 0xA1, 0x81, 0x0200, 0x0000, 4, 5000 ) );
        auto ctl_b = b.send( client::control_msg( hb, 0xA1, 0x81, 0x0200, 0x0003, 4, 5000 ) );
        sleep_ms( 50 );
        auto close = a.send( dict( { { RSUSB_KEY_OP, RSUSB_OP_CLOSE }, { RSUSB_KEY_HANDLE, h0 } } ) );
        auto done = a.event( RSUSB_EV_REQ_DONE, 2000, r );   // CLOSE cancels requests
        CHECK( done );
        CHECK_EQ( u64( done, RSUSB_KEY_TRANSFER_STATUS ), RSUSB_XS_CANCELLED );
        auto reopen = a.send( dict( { { RSUSB_KEY_OP, RSUSB_OP_OPEN }, { RSUSB_KEY_TOKEN, 1 }, { RSUSB_KEY_INTERFACE, 0 } } ) );
        CHECK( ! close->wait( 300 ) );    // waits for the held control transfer
        CHECK( ! reopen->wait( 10 ) );    // parked on the releasing messenger, not ACCESS
        CHECK( ! ctl_b->wait( 10 ) );     // other messenger's control transfer untouched
        e.mock->hold_completions( 1, 0, false );
        CHECK( ctl_a->wait( 2000 ) && status( ctl_a->reply ) == 0 );   // control ran to completion, not cancelled
        CHECK( ctl_b->wait( 2000 ) && status( ctl_b->reply ) == 0 );
        CHECK( close->wait( 2000 ) && status( close->reply ) == 0 );
        CHECK( reopen->wait( 2000 ) && status( reopen->reply ) == 0 );
        CHECK( u64( reopen->reply, RSUSB_KEY_HANDLE ) > h0 );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r ), RSUSB_ST_NOT_FOUND );   // requests freed with their messenger
    }

    CASE( input_validation )
    {
        env e( { "d455.plist", "recovery.plist" } );
        client a( e );
        uint64_t h, hr;
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
        std::vector< uint8_t > d4( 4, 1 ), d3( 3, 1 );
        CHECK_EQ( a.control( h, 0x40, 0x01, 0, 0, 4, 100, &d4 ), RSUSB_ST_ACCESS );   // vendor request
        CHECK_EQ( a.control( h, 0x80, 0x06, 0x0100, 0, 18 ), RSUSB_ST_ACCESS );      // standard GET_DESCRIPTOR
        CHECK_EQ( a.control( h, 0xA1, 0x100, 0, 0, 4 ), RSUSB_ST_INVALID_PARAM );
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x10000, 0, 4 ), RSUSB_ST_INVALID_PARAM );
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0, 0x10000, 4 ), RSUSB_ST_INVALID_PARAM );
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0, 0x0003, 4 ), RSUSB_ST_ACCESS );      // interface 3 not held
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0, 0x0005, 4 ), RSUSB_ST_ACCESS );
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0, 0, RSUSB_MAX_CONTROL_LEN + 1 ), RSUSB_ST_INVALID_PARAM );
        CHECK_EQ( a.control( h, 0x21, 0x01, 0, 0, 4 ), RSUSB_ST_INVALID_PARAM );           // OUT without data
        CHECK_EQ( a.control( h, 0x21, 0x01, 0, 0, 4, 100, &d3 ), RSUSB_ST_INVALID_PARAM ); // length mismatch
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0, 0, 4, 100, &d4 ), RSUSB_ST_INVALID_PARAM ); // data on IN
        CHECK_EQ( a.control( h, 0x21, 0x01, 0, 0, 0, 100, &d4 ), RSUSB_ST_INVALID_PARAM ); // data with len 0
        CHECK_EQ( a.control( 999, 0xA1, 0x81, 0, 0, 4 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.control( h, 0x21, 0x01, 0x0200, 0x0000, 0 ), 0 );                       // OUT len 0 without data
        std::vector< uint8_t > in;
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0x0000, 10, 100, nullptr, &in ), 0 );
        CHECK_EQ( in.size(), 10 );
        for( size_t i = 0; i < in.size(); ++i )
            CHECK_EQ( in[i], ( 0x81 + 0x200 + i ) & 0xFF );
        CHECK_EQ( a.control( h, 0xA1, 0x87, 0x0100, 0x0001, 48, 100, nullptr, &in ), 0 );   // scripted, VS interface 1
        for( size_t i = 0; i < 48; ++i )
            CHECK_EQ( in[i], i );
        CHECK_EQ( a.bulk( h, 0x84, 16 ), RSUSB_ST_NOT_FOUND );   // interface 4 not held
        CHECK_EQ( a.bulk( h, 0x182, 16 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.bulk( h, 0x82, RSUSB_MAX_BULK_LEN + 1 ), RSUSB_ST_INVALID_PARAM );
        CHECK_EQ( a.bulk( h, 0x82, 16, 100, &d4 ), RSUSB_ST_INVALID_PARAM );   // data on IN
        CHECK_EQ( a.bulk( h, 0x87, 16, 1000 ), 0 );                            // interrupt endpoint of the VC
        CHECK_EQ( a.reset_ep( h, 0x84 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.reset_ep( h, 0x300 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.reset_ep( h, 0x82 ), 0 );
        CHECK_EQ( a.reset_ep( 77, 0x82 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.req_create( h, 0x82, 0 ), RSUSB_ST_INVALID_PARAM );
        CHECK_EQ( a.req_create( h, 0x82, RSUSB_MAX_REQUEST_LEN + 1 ), RSUSB_ST_INVALID_PARAM );
        CHECK_EQ( a.req_create( h, 0x84, 64 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.req_create( 77, 0x82, 64 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, 77 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( a.req( RSUSB_OP_REQ_CANCEL, 77 ), RSUSB_ST_NOT_FOUND );
        a.destroy( 77 );   // ignored
        CHECK_EQ( a.open( 2, 1, &hr ), 0 );   // recovery: vendor interface with a bulk OUT endpoint
        CHECK_EQ( a.req_create( hr, 0x01, 64 ), RSUSB_ST_INVALID_PARAM );   // OUT endpoint
        CHECK_EQ( a.bulk( hr, 0x01, 4 ), RSUSB_ST_INVALID_PARAM );          // OUT without data
        CHECK_EQ( a.bulk( hr, 0x01, 4, 100, &d4 ), 0 );
        CHECK_EQ( a.lease( 0 ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( e.core->stats().connections, 1 );   // none of the above cancelled the connection
    }

    CASE( status_mapping_and_timeouts )
    {
        env e( { "d455.plist" } );
        client a( e );
        uint64_t h;
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
        std::pair< int, int > map[] = { { RSUSB_XS_TIMED_OUT, RSUSB_ST_TIMEOUT }, { RSUSB_XS_STALL, RSUSB_ST_PIPE },
                                        { RSUSB_XS_OVERFLOW, RSUSB_ST_OVERFLOW }, { RSUSB_XS_ERROR, RSUSB_ST_IO },
                                        { RSUSB_XS_CANCELLED, RSUSB_ST_IO } };
        for( auto & m : map )
        {
            e.mock->fail_next( 1, mock_backend::op::control, m.first );
            CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0, 4 ), m.second );
            e.mock->fail_next( 1, mock_backend::op::bulk, m.first );
            CHECK_EQ( a.bulk( h, 0x82, 64 ), m.second );
        }
        e.mock->fail_next( 1, mock_backend::op::submit, RSUSB_ST_BUSY );
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0, 4 ), RSUSB_ST_BUSY );   // submit errors are passed through
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0, 4, 0 ), 0 );
        CHECK_EQ( e.mock->last_timeout(), RSUSB_MAX_TIMEOUT_MS );
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0, 4, 20000 ), 0 );
        CHECK_EQ( e.mock->last_timeout(), RSUSB_MAX_TIMEOUT_MS );
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0, 4, 100 ), 0 );
        CHECK_EQ( e.mock->last_timeout(), 100 );
        e.mock->set_stream_period( 1, 0x82, 500 );
        CHECK_EQ( a.bulk( h, 0x82, 64, 50 ), RSUSB_ST_TIMEOUT );   // honours the timeout
        e.mock->fail_next( 1, mock_backend::op::control, RSUSB_XS_NO_DEVICE );
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0, 4 ), RSUSB_ST_NO_DEVICE );
        CHECK( a.event( RSUSB_EV_DEVICE_GONE ) );
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0, 4 ), RSUSB_ST_NO_DEVICE );
    }

    CASE( hwm_filter_unit )
    {
        config c = test_config();
        hwm_policy p( c );
        auto deny = [&]( const std::vector< uint8_t > & pk, bool control = true, uint16_t val = 0x0100, uint16_t idx = 0x0300 ) {
            return hwm_denied( p, pk.data(), pk.size(), control, val, idx );
        };
        for( uint8_t op : { 0x0C, 0x0D, 0x0E } )
        {
            CHECK( deny( hwm_packet( op ) ) );
            CHECK( deny( hwm_packet( op ), true, 0x1100, 0x0700 ) );   // any unit / selector with the magic
            CHECK( deny( hwm_packet( op ), false, 0, 0 ) );            // bulk
            CHECK( deny( hwm_packet( op, 0, 0, 1024, 0x12, 0x34 ) ) ); // altered magic on unit 3 selector 1
            auto high = hwm_packet( op );
            high[5] = 0x12, high[6] = 0x34, high[7] = 0x56;
            CHECK( deny( high ) );
        }
        CHECK( ! deny( hwm_packet( 0x0C, 0, 0, 1024, 0x12, 0x34 ), true, 0x0200, 0x0300 ) );   // not the HWM XU, no magic
        CHECK( ! deny( hwm_packet( 0x0C, 0, 0, 1024, 0x12, 0x34 ), false, 0, 0 ) );
        for( uint8_t op : { 0x0A, 0x0B, 0x1E, 0x20, 0x10 } )
            CHECK( ! deny( hwm_packet( op ) ) );
        CHECK( ! deny( hwm_packet( 0x0C, 0, 0, 4 ) ) );   // shorter than RSUSB_HWM_MIN_LEN
        CHECK( deny( hwm_packet( 0x0C, 0, 0, 5 ) ) );
        c.denied_opcodes.set( 0x10 );
        c.protected_ranges = { { 0x10000, 0x1000 } };
        hwm_policy q( c );
        auto deny2 = [&]( const std::vector< uint8_t > & pk ) { return hwm_denied( q, pk.data(), pk.size(), true, 0x0100, 0x0300 ); };
        CHECK( deny2( hwm_packet( 0x10 ) ) );
        CHECK( deny2( hwm_packet( 0x0B, 0x10, 1 ) ) );     // FES sector 0x10 = 0x10000
        CHECK( deny2( hwm_packet( 0x0B, 0x0F, 2 ) ) );     // [0xF000, 0x11000) overlaps
        CHECK( ! deny2( hwm_packet( 0x0B, 0x0F, 1 ) ) );   // [0xF000, 0x10000) touches only
        CHECK( ! deny2( hwm_packet( 0x0B, 0x10, 0 ) ) );   // param2 0 never overlaps
        CHECK( deny2( hwm_packet( 0x0A, 0x10FFF, 1 ) ) );  // FWB byte range
        CHECK( ! deny2( hwm_packet( 0x0A, 0x11000, 16 ) ) );
        CHECK( ! deny2( hwm_packet( 0x0B, 0xFFFFFFFF, 0xFFFFFFFF ) ) );   // 64-bit arithmetic, far above
        CHECK( ! deny2( hwm_packet( 0x0B, 0x10, 1, 15 ) ) );   // ranges need len >= 16
    }

    CASE( hwm_filter_wire )
    {
        config c = test_config();
        c.denied_opcodes.set( 0x10 );
        env e( { "d455.plist", "recovery.plist" }, fast(), c );
        client a( e );
        uint64_t h, hr, hd;
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
        auto fef = hwm_packet( 0x0C ), gvd = hwm_packet( 0x10 ), fwb = hwm_packet( 0x0A, 0, 16 );
        size_t logged = e.mock->out_log().size();
        CHECK_EQ( a.control( h, 0x21, 0x01, 0x0100, 0x0300, fef.size(), 100, &fef ), RSUSB_ST_ACCESS );
        CHECK_EQ( a.control( h, 0x21, 0x11, 0x0100, 0x0300, fef.size(), 100, &fef ), RSUSB_ST_ACCESS );
        CHECK_EQ( a.control( h, 0x21, 0x01, 0x0100, 0x0300, gvd.size(), 100, &gvd ), RSUSB_ST_ACCESS );   // DeniedHWMOpcodes
        CHECK_EQ( e.mock->out_log().size(), logged );   // nothing reached the device
        CHECK_EQ( a.control( h, 0x21, 0x01, 0x0100, 0x0300, fwb.size(), 100, &fwb ), 0 );
        std::vector< uint8_t > in;
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0100, 0x0300, 1024, 100, nullptr, &in ), 0 );
        CHECK_EQ( in[0], 0x0A );   // mock HWM echo
        CHECK_EQ( a.open( 2, 1, &hr ), 0 );
        auto fplock = hwm_packet( 0x0E, 0, 0, 24 );
        CHECK_EQ( a.bulk( hr, 0x01, fplock.size(), 100, &fplock ), RSUSB_ST_ACCESS );
        auto ok = hwm_packet( 0x20, 0, 0, 24 );
        CHECK_EQ( a.bulk( hr, 0x01, ok.size(), 100, &ok ), 0 );
        CHECK_EQ( a.bulk( hr, 0x81, 1024, 100, nullptr, &in ), 0 );
        CHECK_EQ( in[0], 0x20 );
        CHECK_EQ( a.open( 2, 0, &hd ), 0 );   // DFU interface: DNLOAD blocks are never filtered
        std::vector< uint8_t > block = hwm_packet( 0x0C, 0, 0, 1024 );
        CHECK_EQ( a.control( hd, 0x21, 0x01, 0x0001, 0x0000, block.size(), 5000, &block ), 0 );
    }

    CASE( requests )
    {
        env e( { "d455.plist" } );
        client a( e );
        uint64_t h, r1, r2, r3;
        uint8_t * map = nullptr;
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
        CHECK_EQ( a.req_create( h, 0x82, 1000, &r1, &map ), 0 );
        for( uint64_t seq = 0; seq < 3; ++seq )
        {
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r1 ), 0 );
            auto ev = a.event( RSUSB_EV_REQ_DONE, 2000, r1 );
            CHECK( ev );
            CHECK_EQ( u64( ev, RSUSB_KEY_TRANSFER_STATUS ), RSUSB_XS_COMPLETED );
            CHECK_EQ( u64( ev, RSUSB_KEY_TRANSFERRED ), 1000 );
            uint64_t got = 0;
            memcpy( &got, map, 8 );
            CHECK_EQ( got, seq );
            CHECK_EQ( map[8], 0x82 );
            for( int i = 9; i < 1000; ++i )
                CHECK_EQ( map[i], ( seq + i ) & 0xFF );
        }
        e.mock->set_stream_period( 1, 0x82, 100000 );
        CHECK_EQ( a.req( RSUSB_OP_REQ_CANCEL, r1 ), RSUSB_ST_NOT_FOUND );   // idle
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r1 ), 0 );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r1 ), RSUSB_ST_BUSY );
        CHECK_EQ( a.req_create( h, 0x82, 1000, &r2 ), 0 );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r2 ), 0 );
        CHECK_EQ( a.req( RSUSB_OP_REQ_CANCEL, r2 ), 0 );   // aborts the whole pipe
        auto e1 = a.event( RSUSB_EV_REQ_DONE, 2000, r1 ), e2 = a.event( RSUSB_EV_REQ_DONE, 2000, r2 );
        CHECK( e1 && e2 );
        CHECK_EQ( u64( e1, RSUSB_KEY_TRANSFER_STATUS ), RSUSB_XS_CANCELLED );
        CHECK_EQ( u64( e2, RSUSB_KEY_TRANSFER_STATUS ), RSUSB_XS_CANCELLED );
        CHECK_EQ( a.req_create( h, 0x82, 5000, &r3 ), 0 );
        CHECK_EQ( e.core->stats().requests, 3 );
        CHECK_EQ( e.core->stats().shm_bytes, 3 * uint64_t( getpagesize() ) );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r3 ), 0 );
        CHECK_EQ( e.core->stats().inflight_bytes, 5000 );
        a.destroy( r3 );   // in flight: cancelled, freed after its completion, no REQ_DONE
        CHECK( wait_for( [&] { return e.core->stats().requests == 2 && e.core->stats().inflight_bytes == 0; } ) );
        CHECK( ! a.event( RSUSB_EV_REQ_DONE, 300, r3 ) );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r3 ), RSUSB_ST_NOT_FOUND );
        CHECK_EQ( e.mock->counters().live_transfers, 0 );
        // interrupt request on the VC endpoint
        uint64_t ri;
        CHECK_EQ( a.req_create( h, 0x87, 64, &ri ), 0 );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, ri ), 0 );
        CHECK( a.event( RSUSB_EV_REQ_DONE, 2000, ri ) );
    }

    CASE( quotas )
    {
        env e( { "two-d455.plist" } );
        {
            client a( e );
            uint64_t h, r;
            CHECK_EQ( a.open( 1, 0, &h ), 0 );
            for( unsigned i = 0; i < RSUSB_QUOTA_REQUESTS_PER_DEVICE; ++i )
                CHECK_EQ( a.req_create( h, 0x82, 64 ), 0 );
            CHECK_EQ( a.req_create( h, 0x82, 64 ), RSUSB_ST_NO_MEM );
            CHECK_EQ( a.lease( 2 ), 0 );   // quotas scale with the leased devices
            CHECK_EQ( a.req_create( h, 0x82, 64, &r ), 0 );
        }
        {
            client a( e );
            uint64_t h, r[4];
            CHECK( wait_for( [&] { return a.open( 1, 3, &h ) == 0; } ) );
            for( auto & x : r )
                CHECK_EQ( a.req_create( h, 0x84, 16u << 20, &x ), 0 );
            CHECK_EQ( a.req_create( h, 0x84, 4096 ), RSUSB_ST_NO_MEM );   // 64 MiB shm per device
            e.mock->set_stream_period( 1, 0x84, 100000 );
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r[0] ), 0 );
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r[1] ), 0 );
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r[2] ), RSUSB_ST_NO_MEM );   // 32 MiB in flight per device
            CHECK_EQ( a.lease( 2 ), 0 );
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r[2] ), 0 );
            CHECK_EQ( a.req_create( h, 0x84, 4096 ), 0 );
            e.mock->hold_completions( 1, 0, true );
            std::vector< std::shared_ptr< pending > > ps;
            for( unsigned i = 0; i < 2 * RSUSB_QUOTA_SYNC_PER_DEVICE; ++i )
                ps.push_back( a.send( client::control_msg( h, 0xA1, 0x81, 0x0200, 0x0003, 4, 5000 ) ) );
            CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0x0003, 4 ), RSUSB_ST_NO_MEM );   // 16 sync ops per device
            CHECK_EQ( a.reset_ep( h, 0x84 ), RSUSB_ST_NO_MEM );
            e.mock->hold_completions( 1, 0, false );
            for( auto & p : ps )
                CHECK( p->wait( 2000 ) && status( p->reply ) == 0 );
            CHECK_EQ( a.reset_ep( h, 0x84 ), 0 );
        }
        CHECK( wait_for( [&] { auto s = e.core->stats(); return s.requests == 0 && s.shm_bytes == 0 && s.inflight_bytes == 0; } ) );
    }

    CASE( lease_limit_and_global_inflight )
    {
        auto one = load( { "d455.plist" } )[0];
        std::vector< fixture_device > many;
        for( unsigned i = 0; i < RSUSB_MAX_DEVICES + 1; ++i )
        {
            fixture_device f = one;
            f.snap.path = std::to_string( i + 1 ) + "-1-1";
            f.registry_id = 0x200000000ull - i;   // reverse order: ENUMERATE sorts by registry ID
            many.push_back( f );
        }
        env e( many, fast(), test_config(), nullptr );
        client a( e );
        auto list = a.enumerate();
        CHECK_EQ( list.size(), RSUSB_MAX_DEVICES );
        CHECK_EQ( list[0].first, RSUSB_MAX_DEVICES + 1 );   // smallest registry ID first
        for( unsigned t = 1; t <= RSUSB_MAX_DEVICES; ++t )
            CHECK_EQ( a.lease( t ), 0 );
        CHECK_EQ( a.lease( RSUSB_MAX_DEVICES + 1 ), RSUSB_ST_NO_MEM );
        CHECK_EQ( a.lease( 5 ), 0 );   // idempotent
        // global in-flight bytes (RSUSB_MAX_INFLIGHT_TOTAL) bind before this connection's 32 x 32 MiB
        uint64_t h;
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
        e.mock->set_stream_period( 1, 0x82, 100000 );
        const uint64_t len = 16u << 20;
        unsigned fit = unsigned( RSUSB_MAX_INFLIGHT_TOTAL / len );
        for( unsigned i = 0; i <= fit; ++i )
        {
            uint64_t r;
            CHECK_EQ( a.req_create( h, 0x82, len, &r ), 0 );
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r ), i < fit ? 0 : RSUSB_ST_NO_MEM );
        }
        CHECK_EQ( e.core->stats().inflight_bytes, RSUSB_MAX_INFLIGHT_TOTAL );
    }

    CASE( device_loss )
    {
        env e( { "d455.plist" } );
        client a( e ), b( e );
        uint64_t h, r;
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
        CHECK_EQ( b.lease( 1 ), 0 );
        e.mock->set_stream_period( 1, 0x82, 100000 );
        CHECK_EQ( a.req_create( h, 0x82, 4096, &r ), 0 );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r ), 0 );
        e.mock->unplug( 1 );
        // DEVICE_GONE arrives before the REQ_DONE(NO_DEVICE) it causes
        xpc_object_t first = nullptr;
        CHECK( wait_for( [&] {
            std::lock_guard< std::mutex > l( a.m );
            if( a.events.size() >= 2 )
                first = a.events.front();
            return first != nullptr;
        } ) );
        CHECK_EQ( u64( first, RSUSB_KEY_EVENT ), RSUSB_EV_DEVICE_GONE );
        CHECK_EQ( u64( first, RSUSB_KEY_TOKEN ), 1 );
        auto done = a.event( RSUSB_EV_REQ_DONE, 1000, r );
        CHECK( done );
        CHECK_EQ( u64( done, RSUSB_KEY_TRANSFER_STATUS ), RSUSB_XS_NO_DEVICE );
        CHECK( b.event( RSUSB_EV_DEVICE_GONE ) );   // lease holder too
        CHECK_EQ( a.control( h, 0xA1, 0x81, 0x0200, 0, 4 ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( a.bulk( h, 0x82, 4 ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( a.req( RSUSB_OP_REQ_CANCEL, r ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( a.reset_ep( h, 0x82 ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( a.lease( 1 ), RSUSB_ST_NO_DEVICE );
        CHECK( wait_for( [&] { return e.mock->counters().handles == 0 && e.mock->counters().anchors == 0; } ) );
        CHECK_EQ( a.close( h ), RSUSB_ST_NO_DEVICE );
        CHECK_EQ( e.mock->counters().hand_backs.size(), 0 );   // leases on a gone device end without hand-back
        CHECK( wait_for( [&] { auto s = e.core->stats(); return s.leases == 0 && s.messengers == 0 && s.devices == 0; } ) );
        uint64_t fresh = e.mock->plug( 0 );
        CHECK( wait_for( [&] { auto l = a.enumerate(); return l.size() == 1 && l[0].first == fresh; } ) );
        CHECK( fresh != 1 );
    }

    CASE( watchdog )
    {
        env e( { "d455.plist" } );
        client a( e );
        e.mock->wedge( 1, mock_backend::op::claim );
        uint64_t t0 = now_ms();
        CHECK_EQ( a.open( 1, 0 ), RSUSB_ST_NO_DEVICE );
        uint64_t dt = now_ms() - t0;
        CHECK( dt >= 550 && dt < 2000 );   // watchdog_ms = 600, well before RSUSB_DEVICE_CALL_TIMEOUT_MS
        CHECK( wait_for( [&] { return e.mock->counters().force_reenumerations == 1; } ) );
        CHECK( wait_for( [&] { auto l = a.enumerate(); return l.size() == 1 && l[0].first == 2; } ) );
        uint64_t h;
        CHECK_EQ( a.open( 2, 0, &h ), 0 );
        CHECK( wait_for( [&] { return e.mock->counters().handles == 1; } ) );   // the abandoned claim cleaned up
    }

    CASE( watchdog_close_wait )
    {
        env e( { "d455.plist" } );
        client a( e );
        uint64_t h;
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
        e.mock->hold_completions( 1, 0, true );
        auto ctl = a.send( client::control_msg( h, 0xA1, 0x81, 0x0200, 0, 4, 0 ) );   // 10 s
        sleep_ms( 20 );
        auto close = a.send( dict( { { RSUSB_KEY_OP, RSUSB_OP_CLOSE }, { RSUSB_KEY_HANDLE, h } } ) );
        CHECK( close->wait( 3000 ) );   // watchdog -> forced re-enumeration completes the transfer
        CHECK( ctl->wait( 100 ) && status( ctl->reply ) == RSUSB_ST_NO_DEVICE );
        CHECK_EQ( e.mock->counters().force_reenumerations, 1 );
    }

    CASE( client_crash_cleanup )
    {
        env e( { "two-d455.plist" } );
        {
            client a( e );
            uint64_t h0, h3, r1, r2;
            CHECK_EQ( a.open( 1, 0, &h0 ), 0 );
            CHECK_EQ( a.open( 1, 3, &h3 ), 0 );
            CHECK_EQ( a.lease( 2 ), 0 );
            e.mock->set_stream_period( 1, 0x84, 100000 );
            CHECK_EQ( a.req_create( h0, 0x82, 1 << 20, &r1 ), 0 );
            CHECK_EQ( a.req_create( h3, 0x84, 1 << 20, &r2 ), 0 );
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r1 ), 0 );
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r2 ), 0 );
            e.mock->hold_completions( 1, 0, true );
            a.send( client::control_msg( h0, 0xA1, 0x81, 0x0200, 0, 4, 200 ) );
            sleep_ms( 50 );
            a.cancel();
        }
        CHECK( wait_for( [&] {
            auto s = e.core->stats();
            auto m = e.mock->counters();
            return s.connections == 0 && s.messengers == 0 && s.claimed == 0 && s.requests == 0 && s.shm_bytes == 0
                && s.inflight_bytes == 0 && s.leases == 0 && m.handles == 0 && m.claimed == 0 && m.live_transfers == 0;
        } ) );
        CHECK( wait_for( [&] { return e.mock->counters().anchors == 0 && e.mock->counters().hand_backs.size() == 1; } ) );
    }

    CASE( console_change_revokes )
    {
        env e( { "d455.plist" } );
        client user( e );
        e.pol->set_next_peer( 0, false );
        client root( e );
        e.pol->set_console( 502 );
        CHECK( user.wait_invalid( 1000 ) );
        CHECK_EQ( root.enumerate().size(), 1 );
        CHECK( ! root.invalid );
        e.pol->set_console( 501 );
        client again( e );
        e.pol->set_console( 503, false );   // the change is not reported yet: a stale generation cancels on an op
        CHECK_EQ( again.enumerate().size(), 1 );
        CHECK_EQ( again.lease( 1 ), 1000 );
        CHECK( again.wait_invalid( 1000 ) );
    }

    CASE( camera_disallowed_cancels_everyone )
    {
        env e( { "d455.plist" } );
        client user( e );
        e.pol->set_next_peer( 0, false );
        client root( e );
        e.pol->set_camera_allowed( false );
        CHECK( user.wait_invalid( 1000 ) );
        CHECK( root.wait_invalid( 1000 ) );
    }

    CASE( idle_predicate )
    {
        env e( { "d455.plist" } );
        CHECK( e.core->idle() );
        {
            client a( e );
            CHECK( ! e.core->idle() );
            uint64_t h;
            CHECK_EQ( a.open( 1, 0, &h ), 0 );
            a.cancel();
            CHECK( wait_for( [&] { return e.core->stats().connections == 0; } ) );
            CHECK( ! e.core->idle() );   // anchor / grace timer
        }
        CHECK( wait_for( [&] { return e.core->idle(); } ) );
        CHECK_EQ( e.mock->counters().hand_backs.size(), 1 );
    }

    CASE( back_pressure )
    {
        // A peer whose barriers never run (it does not take our messages) is cancelled once more than max_unacked
        // sends are outstanding, or when a barrier stays pending longer than barrier_stall_ms.
        timing t = fast();
        t.barrier_every = 4;
        t.max_unacked = 64;
        t.barrier_stall_ms = 300;
        env e( { "d455.plist" }, t );
        auto msg = []( uint64_t op ) { return dict( { { RSUSB_KEY_OP, op }, { RSUSB_KEY_VERSION, RSUSB_PROTOCOL_VERSION } } ); };
        auto held = std::make_shared< std::vector< std::function< void() > > >();
        std::mutex hm;
        auto stuck = e.core->open_session( []( xpc_object_t ) {}, [&]( std::function< void() > ack ) {
            std::lock_guard< std::mutex > l( hm );
            held->push_back( ack );
        } );
        auto good = e.core->open_session( []( xpc_object_t ) {}, []( std::function< void() > ack ) { ack(); } );
        e.core->session_message( stuck, msg( RSUSB_OP_HELLO ) );
        e.core->session_message( good, msg( RSUSB_OP_HELLO ) );
        for( int i = 0; i < 63; ++i )
        {
            e.core->session_message( stuck, msg( RSUSB_OP_ENUMERATE ) );
            e.core->session_message( good, msg( RSUSB_OP_ENUMERATE ) );
        }
        CHECK_EQ( e.core->stats().connections, 2 );   // 64 sends unacknowledged: at the limit, not over it
        sleep_ms( 100 );
        e.core->session_message( stuck, msg( RSUSB_OP_ENUMERATE ) );
        CHECK( wait_for( [&] { return e.core->stats().connections == 1; } ) );
        for( int i = 0; i < 1000; ++i )
            e.core->session_message( good, msg( RSUSB_OP_ENUMERATE ) );
        sleep_ms( 500 );
        CHECK_EQ( e.core->stats().connections, 1 );   // acknowledged barriers: never cancelled
        // stall: a few sends, the barrier never runs, nothing else is sent -> the 1 Hz check cancels it
        auto quiet = e.core->open_session( []( xpc_object_t ) {}, []( std::function< void() > ) {} );
        e.core->session_message( quiet, msg( RSUSB_OP_HELLO ) );
        for( int i = 0; i < 4; ++i )
            e.core->session_message( quiet, msg( RSUSB_OP_ENUMERATE ) );
        uint64_t t0 = now_ms();
        CHECK_EQ( e.core->stats().connections, 2 );
        CHECK( wait_for( [&] { return e.core->stats().connections == 1; } ) );
        CHECK( now_ms() - t0 >= 250 );
    }

    CASE( back_pressure_xpc_reading_client )
    {
        // real XPC barriers: a client that reads is never cancelled, even when flooded
        timing t = fast();
        t.barrier_every = 4;
        t.max_unacked = 64;
        t.barrier_stall_ms = 300;
        env e( { "d455.plist" }, t );
        client a( e );
        std::vector< std::shared_ptr< pending > > ps;
        for( int i = 0; i < 3000; ++i )
            ps.push_back( a.send( dict( { { RSUSB_KEY_OP, RSUSB_OP_ENUMERATE } } ) ) );
        for( auto & p : ps )
            CHECK( p->wait( 5000 ) && status( p->reply ) == 0 );
        sleep_ms( 400 );
        CHECK_EQ( e.core->stats().connections, 1 );
    }

    CASE( shutdown_hands_back )
    {
        env e( { "d455.plist" } );
        client a( e );
        uint64_t h;
        CHECK_EQ( a.open( 1, 0, &h ), 0 );
        e.mock->hold_completions( 1, 0, true );
        auto ctl = a.send( client::control_msg( h, 0xA1, 0x81, 0x0200, 0, 4, 0 ) );
        sleep_ms( 20 );
        uint64_t t0 = now_ms();
        e.core->shutdown();
        CHECK( now_ms() - t0 < 3500 );
        CHECK_EQ( e.mock->counters().hand_backs.size(), 1 );
        CHECK( a.wait_invalid( 1000 ) );
        CHECK_EQ( e.mock->counters().handles, 0 );
        CHECK_EQ( e.mock->counters().live_transfers, 0 );   // control transfers cancelled at SIGTERM
    }

    CASE( startup_sweep )
    {
        // a recorded capture whose device has no drivers is handed back at start; unrecorded ones are not
        env e( { "two-d455.plist" }, fast(), test_config(), []( env & x ) {
            x.mock->set_drivers_attached( 1, false );
            x.mock->set_drivers_attached( 2, false );
            paths p;
            p.state_dir = x.state;
            p.require_root = false;
            state_store s( p );
            CHECK( s.prepare_dir() );
            CHECK( s.write( { { x.mock->registry_id( 1 ), 0x8086, 0x0B5C }, { 0x1234, 0x8086, 0x0B5C } } ) );
        } );
        CHECK_EQ( e.mock->counters().hand_backs.size(), 1 );
        CHECK_EQ( e.mock->counters().hand_backs[0], 0x100000B00 );
        CHECK( wait_for( [&] { return e.core->idle(); } ) );
    }

    CASE( config_parser )
    {
        auto plist = []( const std::string & body ) {
            return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>" + body + "</dict></plist>";
        };
        std::string h1( 40, 'a' ), h2( 40, 'b' );
        std::string base = "<key>Version</key><integer>1</integer><key>BuildID</key><string>" + std::string( 64, 'f' )
            + "</string><key>BuildNumber</key><integer>202611041200</integer><key>ProtocolVersion</key><integer>1</integer>"
              "<key>AllowedCDHashes</key><array><string>" + h1 + "</string><string>" + h2 + "</string></array>";
        std::string daemon = "<key>DaemonCDHash</key><string>" + h1 + "</string>";
        auto parse = [&]( const std::string & s, config & c ) {
            std::string err;
            return parse_config( reinterpret_cast< const uint8_t * >( s.data() ), s.size(), c, err );
        };
        config c;
        CHECK( parse( plist( base + daemon ), c ) );
        CHECK_EQ( c.cdhashes.size(), 2 );
        CHECK_EQ( c.grace_s, 5 );
        CHECK_EQ( c.idle_exit_s, 60 );
        CHECK( parse( plist( base + daemon + "<key>HandBackGraceSeconds</key><integer>0</integer><key>IdleExitSeconds</key><integer>3600</integer>"
                                                "<key>DeniedHWMOpcodes</key><array><integer>149</integer><integer>168</integer></array>"
                                                "<key>ProtectedFlashRanges</key><array><dict><key>Start</key><integer>0</integer><key>Length</key><integer>4294967296</integer></dict></array>"
                                                "<key>Unknown</key><string>ignored</string>" ),
                      c ) );
        CHECK( c.denied_opcodes.test( 0x95 ) && c.denied_opcodes.test( 0xA8 ) && c.protected_ranges.size() == 1 && c.grace_s == 0 );
        const char * bad[] = {
            "<key>HandBackGraceSeconds</key><integer>601</integer>", "<key>IdleExitSeconds</key><integer>9</integer>",
            "<key>DeniedHWMOpcodes</key><array><integer>256</integer></array>", "<key>DeniedHWMOpcodes</key><integer>1</integer>",
            "<key>ProtectedFlashRanges</key><array><dict><key>Start</key><integer>1</integer><key>Length</key><integer>4294967296</integer></dict></array>",
            "<key>ProtectedFlashRanges</key><array><dict><key>Start</key><integer>1</integer><key>Length</key><integer>0</integer></dict></array>",
            "<key>HandBackGraceSeconds</key><real>5</real>", "<key>HandBackGraceSeconds</key><string>5</string>",
        };
        for( auto b : bad )
            CHECK( ! parse( plist( base + daemon + b ), c ) );
        CHECK( ! parse( plist( base ), c ) );   // no DaemonCDHash
        CHECK( ! parse( plist( base + "<key>DaemonCDHash</key><string>" + std::string( 40, 'c' ) + "</string>" ), c ) );
        std::string upper = base;
        upper.replace( upper.find( h2 ), 40, std::string( 40, 'B' ) );
        CHECK( ! parse( plist( upper + daemon ), c ) );
        std::string v2 = base;
        v2.replace( v2.find( "<integer>1</integer>" ), 20, "<integer>2</integer>" );
        CHECK( ! parse( plist( v2 + daemon ), c ) );
        CHECK( ! parse( "not a plist", c ) );
        std::string many = "<key>Version</key><integer>1</integer><key>BuildID</key><string>" + std::string( 64, 'f' )
            + "</string><key>BuildNumber</key><integer>1</integer><key>ProtocolVersion</key><integer>1</integer><key>AllowedCDHashes</key><array>";
        for( int i = 0; i < 257; ++i )
            many += "<string>" + h1 + "</string>";
        CHECK( ! parse( plist( many + "</array>" + daemon ), c ) );
        // file rules (owner check off: the tester owns the temp dir)
        std::string dir = temp_dir();
        chmod( dir.c_str(), 0755 );
        std::string file = dir + "/usbd.plist", text = plist( base + daemon ), err;
        std::ofstream( file ) << text;
        chmod( file.c_str(), 0644 );
        CHECK( load_config( file, false, c, err ) );
        CHECK( ! load_config( file, true, c, err ) );   // not root-owned
        chmod( file.c_str(), 0664 );
        CHECK( ! load_config( file, false, c, err ) );
        chmod( file.c_str(), 0644 );
        symlink( file.c_str(), ( dir + "/link.plist" ).c_str() );
        CHECK( ! load_config( dir + "/link.plist", false, c, err ) );
        chmod( dir.c_str(), 0777 );
        CHECK( ! load_config( file, false, c, err ) );
        chmod( dir.c_str(), 0755 );
        std::ofstream( dir + "/big.plist" ) << std::string( 70000, ' ' );
        chmod( ( dir + "/big.plist" ).c_str(), 0644 );
        CHECK( ! load_config( dir + "/big.plist", false, c, err ) );
    }

    CASE( state_file_and_sweep_selection )
    {
        std::string root = temp_dir();
        paths p;
        p.state_dir = root + "/state";
        p.require_root = false;
        state_store s( p );
        CHECK( s.prepare_dir() );
        struct stat st;
        CHECK( lstat( p.state_dir.c_str(), &st ) == 0 && ( st.st_mode & 07777 ) == 0700 );
        std::vector< captured_entry > in = { { 0x100000123, 0x8086, 0x0B5C }, { 7, 0x38E5, 0x1234 } }, out;
        CHECK( s.write( in ) );
        CHECK( s.read( out ) && out == in );
        CHECK( lstat( s.file().c_str(), &st ) == 0 && ( st.st_mode & 07777 ) == 0600 );
        chmod( s.file().c_str(), 0644 );
        CHECK( ! s.read( out ) );
        s.add( { 1, 0x8086, 0x0B5C } );
        s.update( 1, { 2, 0x8086, 0x0B5C } );
        CHECK( s.read( out ) && out.size() == 1 && out[0].registry_id == 2 );
        s.remove( 2 );
        CHECK( s.read( out ) && out.empty() );
        // a symlinked or loosely-permissioned state dir is replaced
        state_store s2( p );
        std::string target = root + "/elsewhere";
        mkdir( target.c_str(), 0777 );
        std::string cmd_dir = p.state_dir;
        rename( cmd_dir.c_str(), ( root + "/old" ).c_str() );
        symlink( target.c_str(), cmd_dir.c_str() );
        CHECK( s2.prepare_dir() );
        CHECK( lstat( cmd_dir.c_str(), &st ) == 0 && S_ISDIR( st.st_mode ) && ( st.st_mode & 07777 ) == 0700 );
        chmod( cmd_dir.c_str(), 0755 );
        CHECK( s2.prepare_dir() );
        CHECK( lstat( cmd_dir.c_str(), &st ) == 0 && ( st.st_mode & 07777 ) == 0700 );
        // lock: exclusive
        int fd = s2.lock( true );
        CHECK( fd >= 0 );
        pid_t pid = fork();
        if( pid == 0 )
            _exit( s2.lock( true ) >= 0 ? 1 : 0 );
        int status = 0;
        waitpid( pid, &status, 0 );
        CHECK( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 );
        close( fd );
        // sweep selection
        std::vector< live_device > live = { { 10, 0x8086, 0x0B5C, 0x01000000, 1, false },
                                            { 11, 0x8086, 0x0B5C, 0x02000000, 1, true },
                                            { 12, 0x046D, 0x085E, 0x03000000, 1, false },
                                            { 13, 0x38E5, 0x1111, 0x04000000, 1, false },
                                            { 14, 0x8086, 0x8000, 0x05000000, 1, false } };
        std::vector< captured_entry > rec = { { 10, 0x8086, 0x0B5C }, { 11, 0x8086, 0x0B5C }, { 12, 0x046D, 0x085E },
                                              { 13, 0x38E5, 0x1111 }, { 14, 0x8086, 0x8000 }, { 99, 0x8086, 0x0B5C },
                                              { 10, 0x8086, 0x0B07 } };
        auto sel = sweep_select( rec, live );
        CHECK_EQ( sel.size(), 2 );
        CHECK( sel[0].registry_id == 10 && sel[1].registry_id == 13 );
    }

    CASE( snapshot_rules )
    {
        auto f = load( { "d455.plist", "recovery.plist" } );
        const snapshot & s = f[0].snap;
        CHECK( device_allowed( s ) );
        CHECK( ( claim_set( s, 0, 0 ) == std::vector< uint8_t >{ 0, 1, 2 } ) );
        CHECK( ( claim_set( s, 0, 3 ) == std::vector< uint8_t >{ 3, 4 } ) );
        CHECK( ( claim_set( s, 0, 1 ) == std::vector< uint8_t >{ 1 } ) );
        CHECK( ( claim_set( s, 0, 5 ) == std::vector< uint8_t >{ 5 } ) );
        CHECK( claim_set( s, 0, 9 ).empty() );
        CHECK( claim_set( s, 1, 0 ).empty() );   // only the active configuration
        xpc_object_t x = snapshot_to_xpc( s, 42 );
        snapshot back;
        uint64_t tok;
        CHECK( snapshot_from_xpc( x, back, tok ) && tok == 42 && same( back, s ) );
        xpc_dictionary_set_uint64( x, RSUSB_KEY_TOKEN, 0 );
        CHECK( ! snapshot_from_xpc( x, back, tok ) );
        xpc_release( x );
        auto all = load( { "filtered.plist" } );
        for( auto & d : all )
            CHECK( ! device_allowed( d.snap ) );
        CHECK( device_allowed( f[1].snap ) );
        CHECK( vid_pid_allowed( 0x38E5, 0x9999 ) && vid_pid_allowed( 0x8086, 0x0ADB ) && ! vid_pid_allowed( 0x8086, 0x0001 ) );
        CHECK( refused_interface_class( 3, 1, 2 ) && ! refused_interface_class( 3, 1, 0 ) && ! refused_interface_class( 3, 0, 1 ) );
        // fixture round trip (the --dump format)
        std::string xml = fixture_xml( f );
        std::string dir = temp_dir();
        std::ofstream( dir + "/rt.plist" ) << xml;
        std::vector< fixture_device > rt;
        std::string err;
        CHECK( load_fixture_file( dir + "/rt.plist", rt, err ) );
        CHECK( rt.size() == 2 && same( rt[0].snap, f[0].snap ) && rt[0].registry_id == f[0].registry_id && same( rt[1].snap, f[1].snap ) );
    }

    // -----------------------------------------------------------------------------------------------------------------
    // H4 (daemon side): concurrent CONTROL/BULK on 3 messengers, 2 streaming requests at 1 kHz, open/close churn

    int stress( unsigned seconds )
    {
        env e( { "d455.plist", "recovery.plist" } );
        e.mock->set_stream_period( 1, 0x82, 1 );
        e.mock->set_stream_period( 1, 0x83, 1 );
        client a( e );
        uint64_t h[3];
        CHECK_EQ( a.open( 1, 0, &h[0] ), 0 );
        CHECK_EQ( a.open( 1, 3, &h[1] ), 0 );
        CHECK_EQ( a.open( 1, 5, &h[2] ), 0 );
        uint64_t r1, r2;
        CHECK_EQ( a.req_create( h[0], 0x82, 4096, &r1 ), 0 );
        CHECK_EQ( a.req_create( h[0], 0x83, 4096, &r2 ), 0 );
        std::atomic< bool > stop{ false };
        std::atomic< uint64_t > ops{ 0 }, frames{ 0 }, churn{ 0 }, failures{ 0 };
        std::vector< std::thread > ts;
        for( int t = 0; t < 8; ++t )
            ts.emplace_back( [&, t] {
                uint64_t mh = h[t % 3];
                uint64_t idx = t % 3 == 0 ? 0 : t % 3 == 1 ? 3 : 5;
                std::vector< uint8_t > out( 64, uint8_t( t ) );
                while( ! stop )
                {
                    int64_t s = t % 2 ? a.control( mh, 0xA1, 0x81, 0x0200, idx, 64 ) : a.control( mh, 0x21, 0x01, 0x0200, idx, 64, 100, &out );
                    if( t % 3 == 1 )
                        s |= a.bulk( mh, 0x88, 64, 100 );
                    failures += s != 0;
                    ++ops;
                }
            } );
        ts.emplace_back( [&] {   // streaming: resubmit on every REQ_DONE
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r1 ), 0 );
            CHECK_EQ( a.req( RSUSB_OP_REQ_SUBMIT, r2 ), 0 );
            while( ! stop )
            {
                xpc_object_t ev = a.event( RSUSB_EV_REQ_DONE, 1000 );
                if( ! ev )
                {
                    ++failures;
                    continue;
                }
                ++frames;
                failures += a.req( RSUSB_OP_REQ_SUBMIT, u64( ev, RSUSB_KEY_REQUEST_ID ) ) != 0;
                xpc_release( ev );
            }
        } );
        ts.emplace_back( [&] {   // open/close churn on another client and the recovery device
            client b( e );
            while( ! stop )
            {
                uint64_t hb, rb;
                if( b.open( 2, 1, &hb ) != 0 )
                {
                    ++failures;
                    continue;
                }
                if( b.req_create( hb, 0x81, 512, &rb ) == 0 && b.req( RSUSB_OP_REQ_SUBMIT, rb ) == 0 )
                    b.destroy( rb );
                std::vector< uint8_t > d( 24, 0 );
                b.bulk( hb, 0x01, 24, 100, &d );
                failures += b.close( hb ) != 0;
                ++churn;
            }
        } );
        uint64_t end = now_ms() + seconds * 1000ull;
        while( now_ms() < end )
        {
            sleep_ms( 1000 );
            if( ( end - now_ms() ) / 1000 % 60 == 0 )
                fprintf( stderr, "stress: %llu ops, %llu frames, %llu open/close, %llu failures\n", (unsigned long long)ops.load(),
                         (unsigned long long)frames.load(), (unsigned long long)churn.load(), (unsigned long long)failures.load() );
        }
        stop = true;
        for( auto & t : ts )
            t.join();
        a.cancel();
        CHECK( wait_for( [&] {
            auto s = e.core->stats();
            auto m = e.mock->counters();
            return s.connections == 0 && s.messengers == 0 && s.requests == 0 && s.inflight_bytes == 0 && m.live_transfers == 0;
        }, 5000 ) );
        printf( "stress %us: %llu sync ops, %llu frames (%.0f/s), %llu open/close cycles, %llu failures\n", seconds,
                (unsigned long long)ops.load(), (unsigned long long)frames.load(), frames.load() / double( seconds ),
                (unsigned long long)churn.load(), (unsigned long long)failures.load() );
        return failures ? 1 : 0;
    }

#ifdef RSUSBD_H3
    // -----------------------------------------------------------------------------------------------------------------
    // H3: librealsense's broker client (librealsense2.dylib, through usb_enumerator) against the core and the mock

    namespace lp = librealsense::platform;
    using clk = std::chrono::steady_clock;

    double ms_since( clk::time_point t ) { return std::chrono::duration< double, std::milli >( clk::now() - t ).count(); }

    struct h3_env
    {
        env e;
        explicit h3_env( const std::vector< std::string > & files, timing t = fast() )
            : e( files, t )
        {
            if( getenv( "RSUSBD_H3_LOG" ) )
                rs2_log_to_console( RS2_LOG_SEVERITY_DEBUG, nullptr );
            lp::usb_broker::use_endpoint_for_testing( e.endpoint );
            lp::usb_broker::hide_ms_for_testing( 300 );
        }
        void up() { CHECK( lp::usb_broker::instance() != nullptr ); }
    };

    // rs-macos-usbdump's text (DESIGN 9.0.6)
    std::string h3_dump()
    {
        std::string out;
        char line[512];
        auto infos = lp::usb_enumerator::query_devices_info();
        for( auto & i : infos )
            snprintf( line, sizeof line, "info id=%s vid=%04x pid=%04x mi=%u cls=%u uid=%s spec=%04x\n", i.id.c_str(),
                      i.vid, i.pid, i.mi, unsigned( i.cls ), i.unique_id.c_str(), unsigned( i.conn_spec ) ),
                out += line;
        std::set< std::string > seen;
        for( auto & i : infos )
        {
            if( ! seen.insert( i.id ).second )
                continue;
            auto dev = lp::usb_enumerator::create_usb_device( i );
            if( ! dev )
            {
                out += "device " + i.id + " unavailable\n";
                continue;
            }
            out += "device " + i.id + "\n";
            for( auto & intf : dev->get_interfaces() )
            {
                std::string assoc;
                if( auto li = std::dynamic_pointer_cast< lp::usb_interface_libusb >( intf ) )
                    for( auto & a : li->get_associated_interfaces() )
                        assoc += ( assoc.empty() ? "" : "," ) + std::to_string( a->get_number() );
                snprintf( line, sizeof line, "  intf %u cls=%u sub=%u assoc=[%s]\n", intf->get_number(), intf->get_class(),
                          intf->get_subclass(), assoc.c_str() );
                out += line;
                for( auto & ep : intf->get_endpoints() )
                    snprintf( line, sizeof line, "    ep %02x type=%u dir=%02x intf=%u\n", ep->get_address(),
                              unsigned( ep->get_type() ), unsigned( ep->get_direction() ), ep->get_interface_number() ),
                        out += line;
            }
            for( auto & d : dev->get_descriptors() )
            {
                snprintf( line, sizeof line, "  desc type=%02x len=%u ", d.type, d.length );
                out += line;
                for( auto b : d.data )
                    snprintf( line, sizeof line, "%02x", b ), out += line;
                out += "\n";
            }
        }
        return out;
    }

    void h3_golden( const std::string & name )
    {
        h3_env h( { name + ".plist" } );
        h.up();
        std::ifstream f( g_fixtures + "/" + name + ".expected.txt" );
        CHECK( f.good() );
        std::string expected( ( std::istreambuf_iterator< char >( f ) ), std::istreambuf_iterator< char >() );
        std::string got = h3_dump();
        if( got != expected )
        {
            fprintf( stderr, "--- got\n%s--- expected\n%s", got.c_str(), expected.c_str() );
            CHECK( got == expected );
        }
        printf( "%s: %zu bytes identical\n", name.c_str(), got.size() );
    }

    lp::rs_usb_device h3_device( const std::string & id )
    {
        for( auto & i : lp::usb_enumerator::query_devices_info() )
            if( i.id == id )
                return lp::usb_enumerator::create_usb_device( i );
        return nullptr;
    }

    bool h3_listed( const std::string & id )
    {
        for( auto & i : lp::usb_enumerator::query_devices_info() )
            if( i.id == id )
                return true;
        return false;
    }

    lp::rs_usb_endpoint h3_ep( const lp::rs_usb_device & dev, uint8_t addr )
    {
        for( auto & i : dev->get_interfaces() )
            for( auto & e : i->get_endpoints() )
                if( e->get_address() == addr )
                    return e;
        return nullptr;
    }

    struct h3_probe
    {
        std::mutex m;
        int calls = 0, bad = 0, wrong_queue = 0, resubmit_fail = 0, resubmits = 0;
        std::vector< int > lengths;
        int count()
        {
            std::lock_guard< std::mutex > l( m );
            return calls;
        }
    };

    // A request whose callback checks the mock's bulk pattern (DESIGN 9.0.3) and the queue, and resubmits
    lp::rs_usb_request h3_request( const lp::rs_usb_messenger & m, const lp::rs_usb_endpoint & ep, h3_probe & p, size_t len = 1024 )
    {
        auto req = m->create_request( ep );
        req->set_buffer( std::vector< uint8_t >( len ) );
        std::weak_ptr< lp::usb_messenger > wm = m;
        uint8_t addr = ep->get_address();
        req->set_callback( std::make_shared< lp::usb_request_callback >( [&p, wm, addr]( lp::rs_usb_request r ) {
            std::lock_guard< std::mutex > l( p.m );
            p.calls++;
            int n = r->get_actual_length();
            p.lengths.push_back( n );
            if( strcmp( dispatch_queue_get_label( DISPATCH_CURRENT_QUEUE_LABEL ), "librealsense.usb-broker.device" ) )
                p.wrong_queue++;
            auto & b = r->get_buffer();
            if( n >= 9 )
            {
                uint64_t seq;
                memcpy( &seq, b.data(), 8 );
                if( b[8] != addr )
                    p.bad++;
                for( int i = 9; i < n; ++i )
                    if( b[i] != uint8_t( seq + i ) )
                    {
                        p.bad++;
                        break;
                    }
            }
            if( p.resubmits > 0 )
            {
                p.resubmits--;
                if( auto mm = wm.lock() )
                    if( mm->submit_request( r ) != lp::RS2_USB_STATUS_SUCCESS )
                        p.resubmit_fail++;
            }
        } ) );
        return req;
    }

    bool h3_daemon_clean( env & e )
    {
        return wait_for( [&] {
            auto s = e.core->stats();
            auto m = e.mock->counters();
            return s.connections == 0 && s.messengers == 0 && s.claimed == 0 && s.requests == 0 && s.shm_bytes == 0
                && s.inflight_bytes == 0 && s.leases == 0 && m.handles == 0 && m.claimed == 0 && m.live_transfers == 0;
        } );
    }

    CASE( h3_golden_d455 ) { h3_golden( "d455" ); }
    CASE( h3_golden_two_d455 ) { h3_golden( "two-d455" ); }
    CASE( h3_golden_recovery ) { h3_golden( "recovery" ); }
    CASE( h3_golden_filtered ) { h3_golden( "filtered" ); }

    // No broker reachable (the real Mach service is not installed here): in-process libusb, decided at once
    CASE( h3_fallback_no_broker )
    {
        auto t = clk::now();
        auto b = lp::usb_broker::instance();
        double ms = ms_since( t );
        CHECK( ! b );
        CHECK( ms < 1000 );
        auto infos = lp::usb_enumerator::query_devices_info();
        printf( "no broker: instance() nullptr after %.1f ms; in-process libusb lists %zu interfaces\n", ms, infos.size() );
        CHECK( ! lp::usb_broker::instance() );
    }

    void h3_rejected( const char * what, std::function< void( env & ) > setup, double max_ms )
    {
        timing t = fast();
        t.new_timeout_ms = RSUSB_NEW_TIMEOUT_MS;
        h3_env h( { "d455.plist" }, t );
        setup( h.e );
        auto start = clk::now();
        auto b = lp::usb_broker::instance();
        double ms = ms_since( start );
        printf( "%s: instance() nullptr=%d after %.1f ms\n", what, ! b, ms );
        CHECK( ! b );
        CHECK( ms < max_ms );
        CHECK( wait_for( [&] { return h.e.core->stats().connections == 0; }, 3000 ) );
        CHECK_EQ( h.e.mock->counters().anchors, 0 );
        CHECK( ! lp::usb_broker::instance() );   // decided once
    }

    CASE( h3_reject_user ) { h3_rejected( "AUTH_USER", []( env & e ) { e.pol->set_next_peer( 502, true ); }, 500 ); }
    CASE( h3_reject_session ) { h3_rejected( "no GUI session", []( env & e ) { e.pol->set_next_peer( 501, false ); }, 500 ); }
    CASE( h3_reject_camera ) { h3_rejected( "AUTH_CAMERA_DISABLED", []( env & e ) { e.pol->set_camera_allowed( false, false ); }, 500 ); }
    CASE( h3_reject_signature )
    {
        h3_rejected( "code-signing reject (messages dropped, NEW timeout)",
                     []( env & e ) { e.pol->set_reject_all_signatures( true ); }, RSUSB_HELLO_TIMEOUT_MS + 500 );
    }
    CASE( h3_reject_limit )
    {
        timing t = fast();
        h3_env h( { "d455.plist" }, t );
        std::vector< std::unique_ptr< client > > others;
        for( unsigned i = 0; i < RSUSB_MAX_CLIENTS; ++i )
            others.emplace_back( new client( h.e ) );
        auto start = clk::now();
        CHECK( ! lp::usb_broker::instance() );
        printf( "AUTH_LIMIT: instance() nullptr after %.1f ms\n", ms_since( start ) );
    }

    CASE( h3_messenger )
    {
        h3_env h( { "d455.plist" } );
        h.up();
        auto infos = lp::usb_enumerator::query_devices_info();
        CHECK_EQ( infos.size(), 3 );
        auto dev = lp::usb_enumerator::create_usb_device( infos[0] );
        CHECK( dev && dev->get_info() == infos[0] );
        CHECK_EQ( h.e.core->stats().leases, 1 );
        CHECK_EQ( h.e.mock->counters().anchors, 0 );   // LEASE neither opens nor captures

        auto m0 = dev->open( 0 );   // VC 0 + VS 1, 2
        CHECK( m0 );
        CHECK_EQ( h.e.mock->counters().captures, 1 );
        CHECK_EQ( h.e.core->stats().claimed, 3 );
        CHECK( dev->open( 1 ) == nullptr );   // held by m0: ACCESS
        CHECK( dev->open( 7 ) == nullptr );   // no such interface
        auto m3 = dev->open( 3 );
        auto m5 = dev->open( 5 );
        CHECK( m3 && m5 );
        CHECK_EQ( h.e.core->stats().messengers, 3 );
        CHECK_EQ( h.e.mock->counters().captures, 1 );   // once per instance

        // control IN: scripted reply, then the mock's pattern
        uint8_t buf[64];
        uint32_t n = 77;
        CHECK_EQ( m0->control_transfer( 0xA1, 0x87, 0x0100, 0x0001, buf, 48, n, 100 ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK_EQ( n, 48 );
        for( int i = 0; i < 48; ++i )
            CHECK_EQ( buf[i], i );
        CHECK_EQ( m0->control_transfer( 0xA1, 0x81, 0x0200, 0x0000, buf, 40, n, 100 ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK_EQ( n, 40 );
        for( int i = 0; i < 40; ++i )
            CHECK_EQ( buf[i], uint8_t( 0x81 + 0x0200 + i ) );
        // ownership: interface 3 belongs to m3
        n = 77;
        CHECK_EQ( m0->control_transfer( 0xA1, 0x81, 0x0200, 0x0003, buf, 4, n, 100 ), lp::RS2_USB_STATUS_ACCESS );
        CHECK_EQ( n, 77 );
        CHECK_EQ( m3->control_transfer( 0xA1, 0x81, 0x0200, 0x0003, buf, 4, n, 100 ), lp::RS2_USB_STATUS_SUCCESS );
        // DFU-detach shape: OUT, no data
        CHECK_EQ( m0->control_transfer( 0x21, 0, 1000, 0, nullptr, 0, n, 1000 ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK_EQ( n, 0 );

        // HWM over the depth XU (unit 3, selector 1): FEF denied, GVD (0x10) passes and the mock echoes the opcode
        auto fef = hwm_packet( 0x0C, 0, 0, 24 ), gvd = hwm_packet( 0x10, 0, 0, 24 );
        n = 77;
        CHECK_EQ( m0->control_transfer( 0x21, 1, 0x0100, 0x0300, fef.data(), 24, n, 100 ), lp::RS2_USB_STATUS_ACCESS );
        CHECK_EQ( n, 77 );
        CHECK_EQ( m0->control_transfer( 0x21, 1, 0x0100, 0x0300, gvd.data(), 24, n, 100 ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK_EQ( n, 24 );
        CHECK_EQ( m0->control_transfer( 0xA1, 0x81, 0x0100, 0x0300, buf, 24, n, 100 ), lp::RS2_USB_STATUS_SUCCESS );
        uint32_t echoed;
        memcpy( &echoed, buf, 4 );
        CHECK_EQ( echoed, 0x10 );

        // transfer status mapping; buffer and `transferred` untouched on failure
        std::pair< int, lp::usb_status > map[] = { { RSUSB_XS_STALL, lp::RS2_USB_STATUS_PIPE },
                                                   { RSUSB_XS_TIMED_OUT, lp::RS2_USB_STATUS_TIMEOUT },
                                                   { RSUSB_XS_OVERFLOW, lp::RS2_USB_STATUS_OVERFLOW },
                                                   { RSUSB_XS_ERROR, lp::RS2_USB_STATUS_IO },
                                                   { RSUSB_XS_CANCELLED, lp::RS2_USB_STATUS_IO } };
        for( auto & p : map )
        {
            h.e.mock->fail_next( 1, mock_backend::op::control, p.first );
            memset( buf, 0xEE, sizeof buf );
            n = 77;
            CHECK_EQ( m0->control_transfer( 0xA1, 0x81, 0, 0, buf, 10, n, 100 ), p.second );
            CHECK( n == 77 && buf[0] == 0xEE );
        }
        // a libusb submit error comes back as is
        h.e.mock->fail_next( 1, mock_backend::op::submit, RSUSB_ST_NOT_SUPPORTED );
        CHECK_EQ( m0->control_transfer( 0xA1, 0x81, 0, 0, buf, 10, n, 100 ), lp::RS2_USB_STATUS_NOT_SUPPORTED );
        // clamped timeout reaches the backend
        CHECK_EQ( m0->control_transfer( 0xA1, 0x81, 0, 0, buf, 10, n, 0 ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK_EQ( h.e.mock->last_timeout(), RSUSB_MAX_TIMEOUT_MS );

        // bulk and interrupt IN by the endpoint's raw type; pattern bytes; NOT_FOUND on another messenger's endpoint
        std::vector< uint8_t > rx( 1024 );
        auto e82 = h3_ep( dev, 0x82 ), e87 = h3_ep( dev, 0x87 ), e88 = h3_ep( dev, 0x88 );
        CHECK_EQ( m0->bulk_transfer( e82, rx.data(), 1024, n, 1000 ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK( n == 1024 && rx[8] == 0x82 );
        uint64_t seq;
        memcpy( &seq, rx.data(), 8 );
        CHECK_EQ( rx[100], uint8_t( seq + 100 ) );
        CHECK_EQ( m0->bulk_transfer( e87, rx.data(), 64, n, 1000 ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK( n == 64 && rx[8] == 0x87 );
        n = 77;
        CHECK_EQ( m0->bulk_transfer( e88, rx.data(), 64, n, 1000 ), lp::RS2_USB_STATUS_NOT_FOUND );
        CHECK_EQ( m0->bulk_transfer( e82, rx.data(), 1024, n, 5 ), lp::RS2_USB_STATUS_TIMEOUT );   // period 33 ms
        CHECK_EQ( n, 77 );
        CHECK_EQ( m0->reset_endpoint( e82, 100 ), lp::RS2_USB_STATUS_SUCCESS );

        // concurrent sync calls on two messengers
        std::atomic< int > errors{ 0 };
        std::vector< std::thread > th;
        for( int k = 0; k < 8; ++k )
            th.emplace_back( [&, k] {
                auto mm = k % 2 ? m0 : m3;
                for( int j = 0; j < 200; ++j )
                {
                    uint8_t b[32];
                    uint32_t nn = 0;
                    if( mm->control_transfer( 0xA1, 0x81, j, k % 2 ? 0 : 3, b, 32, nn, 100 ) || nn != 32
                        || b[1] != uint8_t( 0x81 + j + 1 ) )
                        errors++;
                }
            } );
        for( auto & x : th )
            x.join();
        CHECK_EQ( errors.load(), 0 );

        // CLOSE releases; the interface can be opened again; the lease stays
        m0.reset();
        CHECK( wait_for( [&] { return h.e.core->stats().messengers == 2; } ) );
        auto m1 = dev->open( 1 );
        CHECK( m1 );
        m1.reset();
        m3.reset();
        m5.reset();
        CHECK( wait_for( [&] { return h.e.core->stats().messengers == 0 && h.e.mock->counters().handles == 0; } ) );
        CHECK_EQ( h.e.core->stats().leases, 1 );
        sleep_ms( 500 );   // > grace: the lease keeps the capture
        CHECK_EQ( h.e.mock->counters().hand_backs.size(), 0 );
    }

    CASE( h3_requests )
    {
        h3_env h( { "d455.plist" } );
        h.up();
        h.e.mock->set_stream_period( 1, 0x82, 2 );
        auto dev = h3_device( "1-1-1" );
        auto m = dev->open( 0 );
        CHECK( m );
        auto ep = h3_ep( dev, 0x82 );
        h3_probe p;
        p.resubmits = 49;
        auto req = h3_request( m, ep, p );
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK( wait_for( [&] { return p.count() == 50; } ) );
        sleep_ms( 50 );
        {
            std::lock_guard< std::mutex > l( p.m );
            printf( "requests: calls %d bad %d wrong_queue %d resubmit_fail %d\n", p.calls, p.bad, p.wrong_queue, p.resubmit_fail );
            CHECK( p.calls == 50 && p.bad == 0 && p.wrong_queue == 0 && p.resubmit_fail == 0 );
            for( int l : p.lengths )
                CHECK_EQ( l, 1024 );
        }
        CHECK_EQ( req->get_actual_length(), 1024 );
        CHECK_EQ( h.e.core->stats().requests, 1 );
        CHECK_EQ( m->cancel_request( req ), lp::RS2_USB_STATUS_SUCCESS );   // idle
        // a new buffer size re-creates the remote request
        req->set_buffer( std::vector< uint8_t >( 4096 ) );
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK( wait_for( [&] { return p.count() == 51; } ) );
        CHECK_EQ( req->get_actual_length(), 4096 );
        CHECK( wait_for( [&] { return h.e.core->stats().requests == 1; } ) );

        // in flight: BUSY, cancel -> one completion with length 0
        h.e.mock->hold_completions( 1, 0x82, true );
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_BUSY );
        CHECK_EQ( m->cancel_request( req ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK( wait_for( [&] { return p.count() == 52; } ) );
        CHECK_EQ( req->get_actual_length(), 0 );
        // cancel then destroy with the real core is quick and frees the daemon side
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_SUCCESS );
        auto t = clk::now();
        CHECK_EQ( m->cancel_request( req ), lp::RS2_USB_STATUS_SUCCESS );
        req.reset();
        double ms = ms_since( t );
        printf( "cancel + destroy of an in-flight request: %.1f ms\n", ms );
        CHECK( ms <= 110 );
        // destroy while in flight without cancel: the daemon cancels, frees after completion, sends no REQ_DONE
        h3_probe p2;
        auto req2 = h3_request( m, ep, p2 );
        CHECK_EQ( m->submit_request( req2 ), lp::RS2_USB_STATUS_SUCCESS );
        t = clk::now();
        req2.reset();
        printf( "destroy of an in-flight request: %.1f ms\n", ms_since( t ) );
        CHECK( ms_since( t ) <= 110 );
        CHECK( wait_for( [&] {
            auto s = h.e.core->stats();
            return s.requests == 0 && s.shm_bytes == 0 && s.inflight_bytes == 0 && h.e.mock->counters().live_transfers == 0;
        } ) );
        h.e.mock->hold_completions( 1, 0x82, false );
        m.reset();
        CHECK( wait_for( [&] { return h.e.core->stats().messengers == 0; } ) );
    }

    CASE( h3_quotas )
    {
        h3_env h( { "d455.plist" } );
        h.up();
        auto dev = h3_device( "1-1-1" );
        auto m = dev->open( 0 );
        CHECK( m );
        auto ep = h3_ep( dev, 0x82 );
        h3_probe p;
        h.e.mock->hold_completions( 1, 0x82, true );
        // request count: 32 per leased device
        std::vector< lp::rs_usb_request > reqs;
        for( unsigned i = 0; i < RSUSB_QUOTA_REQUESTS_PER_DEVICE; ++i )
        {
            reqs.push_back( h3_request( m, ep, p, 512 ) );
            CHECK_EQ( m->submit_request( reqs.back() ), lp::RS2_USB_STATUS_SUCCESS );
        }
        auto extra = h3_request( m, ep, p, 512 );
        CHECK_EQ( m->submit_request( extra ), lp::RS2_USB_STATUS_NO_MEM );
        for( auto & r : reqs )
            r->get_callback()->cancel(), m->cancel_request( r );
        reqs.clear();
        extra.reset();
        CHECK( wait_for( [&] { return h.e.core->stats().requests == 0 && h.e.core->stats().inflight_bytes == 0; } ) );
        // in-flight bytes: 32 MiB per leased device
        const size_t big = 4 << 20;
        for( unsigned i = 0; i < RSUSB_QUOTA_INFLIGHT_PER_DEVICE / big; ++i )
        {
            reqs.push_back( h3_request( m, ep, p, big ) );
            CHECK_EQ( m->submit_request( reqs.back() ), lp::RS2_USB_STATUS_SUCCESS );
        }
        extra = h3_request( m, ep, p, big );
        CHECK_EQ( m->submit_request( extra ), lp::RS2_USB_STATUS_NO_MEM );
        CHECK_EQ( h.e.core->stats().inflight_bytes, RSUSB_QUOTA_INFLIGHT_PER_DEVICE );
        // shm bytes: 64 MiB per leased device (completed requests keep their mapping)
        h.e.mock->hold_completions( 1, 0x82, false );
        h.e.mock->set_stream_period( 1, 0x82, 1 );
        extra.reset();   // created remotely (REQ_SUBMIT was refused): destroying it returns its shm
        CHECK( wait_for( [&] { return h.e.core->stats().inflight_bytes == 0 && h.e.core->stats().requests == reqs.size(); } ) );
        unsigned created = unsigned( reqs.size() );
        while( created < RSUSB_QUOTA_SHM_PER_DEVICE / big )
        {
            reqs.push_back( h3_request( m, ep, p, big ) );
            CHECK_EQ( m->submit_request( reqs.back() ), lp::RS2_USB_STATUS_SUCCESS );
            ++created;
        }
        CHECK( wait_for( [&] { return h.e.core->stats().inflight_bytes == 0; } ) );
        CHECK_EQ( h.e.core->stats().shm_bytes, RSUSB_QUOTA_SHM_PER_DEVICE );
        extra = h3_request( m, ep, p, big );
        CHECK_EQ( m->submit_request( extra ), lp::RS2_USB_STATUS_NO_MEM );
        for( auto & r : reqs )
            r->get_callback()->cancel();
        reqs.clear();
        extra.reset();
        CHECK( wait_for( [&] { return h.e.core->stats().requests == 0 && h.e.core->stats().shm_bytes == 0; } ) );

        // sync ops: 16 per leased device in progress
        h.e.mock->hold_completions( 1, 0, true );
        std::vector< std::thread > th;
        std::atomic< int > ok{ 0 };
        for( unsigned i = 0; i < RSUSB_QUOTA_SYNC_PER_DEVICE; ++i )
            th.emplace_back( [&] {
                uint8_t b[8];
                uint32_t n = 0;
                if( m->control_transfer( 0xA1, 0x81, 0, 0, b, 8, n, 3000 ) == lp::RS2_USB_STATUS_SUCCESS )
                    ok++;
            } );
        CHECK( wait_for( [&] { return h.e.mock->counters().live_transfers == RSUSB_QUOTA_SYNC_PER_DEVICE; } ) );
        uint8_t b[8];
        uint32_t n = 77;
        CHECK_EQ( m->control_transfer( 0xA1, 0x81, 0, 0, b, 8, n, 3000 ), lp::RS2_USB_STATUS_NO_MEM );
        h.e.mock->hold_completions( 1, 0, false );
        for( auto & x : th )
            x.join();
        printf( "sync quota: %d of %u held transfers completed after release\n", ok.load(), RSUSB_QUOTA_SYNC_PER_DEVICE );
        CHECK_EQ( m->control_transfer( 0xA1, 0x81, 0, 0, b, 8, n, 100 ), lp::RS2_USB_STATUS_SUCCESS );
        m.reset();
    }

    CASE( h3_device_gone )
    {
        h3_env h( { "two-d455.plist" } );
        h.up();
        auto dev = h3_device( "1-1-1" );   // token 1
        CHECK( dev );
        auto m = dev->open( 0 );
        CHECK( m );
        h3_probe p;
        h.e.mock->hold_completions( 1, 0x82, true );
        auto req = h3_request( m, h3_ep( dev, 0x82 ), p );
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_SUCCESS );
        h.e.mock->unplug( 1 );
        CHECK( wait_for( [&] { return p.count() == 1; } ) );
        CHECK_EQ( req->get_actual_length(), 0 );
        uint8_t b[4];
        uint32_t n = 77;
        CHECK_EQ( m->control_transfer( 0xA1, 0x81, 0, 0, b, 4, n, 100 ), lp::RS2_USB_STATUS_NO_DEVICE );
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_NO_DEVICE );
        CHECK( dev->open( 3 ) == nullptr );
        auto t = clk::now();
        uint64_t tok = h.e.mock->plug( 0 );   // same path, new token
        CHECK( tok > 2 );
        CHECK( ! h3_listed( "1-1-1" ) );
        CHECK( h3_listed( "2-1-1" ) );
        CHECK( h3_device( "1-1-1" ) == nullptr );
        CHECK( wait_for( [&] { return h3_listed( "1-1-1" ); }, 2000 ) );
        printf( "path hidden for %.0f ms (window 300)\n", ms_since( t ) );
        auto dev2 = h3_device( "1-1-1" );
        CHECK( dev2 );
        auto m2 = dev2->open( 0 );
        CHECK( m2 );
        CHECK_EQ( m2->control_transfer( 0xA1, 0x81, 0, 0, b, 4, n, 100 ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK( dev->open( 0 ) == nullptr );   // the old object stays dead
        req.reset();
        m.reset();
        m2.reset();
        CHECK( wait_for( [&] { return h.e.core->stats().messengers == 0 && h.e.mock->counters().handles == 0; } ) );
    }

    // The daemon drops every connection (as on a crash and relaunch): the client ends its session, hides the devices,
    // delivers one NO_DEVICE completion, then gets a new session; the core's state of the old connection is gone
    CASE( h3_daemon_restart )
    {
        h3_env h( { "d455.plist" } );
        h.up();
        h.e.mock->set_stream_period( 1, 0x82, 2 );
        auto dev = h3_device( "1-1-1" );
        auto m = dev->open( 0 );
        CHECK( m );
        h3_probe p;
        p.resubmits = 1000000;
        auto req = h3_request( m, h3_ep( dev, 0x82 ), p );
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_SUCCESS );
        sleep_ms( 100 );
        h.e.mock->hold_completions( 1, 0x82, true );
        sleep_ms( 20 );
        int before = p.count();
        h.e.pol->set_camera_allowed( false );   // cancels every connection
        auto t = clk::now();
        CHECK( wait_for( [&] { return p.count() == before + 1; } ) );
        sleep_ms( 50 );
        {
            std::lock_guard< std::mutex > l( p.m );
            printf( "connection dropped: completions before %d after %d, last length %d, resubmit failures %d\n", before,
                    p.calls, p.lengths.back(), p.resubmit_fail );
            CHECK( p.calls == before + 1 && p.lengths.back() == 0 && p.resubmit_fail == 1 && p.bad == 0 );
        }
        uint8_t b[4];
        uint32_t n = 0;
        CHECK_EQ( m->control_transfer( 0xA1, 0x81, 0, 0, b, 4, n, 100 ), lp::RS2_USB_STATUS_NO_DEVICE );
        CHECK( lp::usb_enumerator::query_devices_info().empty() );
        CHECK( h3_daemon_clean( h.e ) );
        CHECK( wait_for( [&] { return h.e.mock->counters().hand_backs.size() == 1; } ) );   // grace after the last lease
        sleep_ms( 1200 );
        CHECK( lp::usb_enumerator::query_devices_info().empty() );   // camera disallowed: HELLO refused, no libusb
        h.e.mock->hold_completions( 1, 0x82, false );
        h.e.pol->set_camera_allowed( true );
        CHECK( wait_for( [&] { return h3_listed( "1-1-1" ); }, 3000 ) );
        printf( "devices back %.0f ms after the drop\n", ms_since( t ) );
        CHECK( dev->open( 0 ) == nullptr );
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_NO_DEVICE );
        auto dev2 = h3_device( "1-1-1" );
        CHECK( dev2 );
        auto m2 = dev2->open( 0 );
        CHECK( m2 );
        h.e.mock->set_stream_period( h.e.mock->last_plugged(), 0x82, 2 );
        h3_probe p2;
        p2.resubmits = 9;
        auto req2 = h3_request( m2, h3_ep( dev2, 0x82 ), p2 );
        CHECK_EQ( m2->submit_request( req2 ), lp::RS2_USB_STATUS_SUCCESS );
        CHECK( wait_for( [&] { return p2.count() == 10; } ) );
        CHECK_EQ( p2.bad, 0 );
        CHECK_EQ( h.e.mock->counters().captures, 2 );
        req.reset();
        req2.reset();
        m.reset();
        m2.reset();
    }

    // The daemon goes away for good: empty lists, dead objects, never a libusb fallback after the first HELLO
    CASE( h3_daemon_gone )
    {
        h3_env h( { "d455.plist" } );
        h.up();
        auto dev = h3_device( "1-1-1" );
        auto m = dev->open( 0 );
        CHECK( m );
        h3_probe p;
        h.e.mock->hold_completions( 1, 0x82, true );
        auto req = h3_request( m, h3_ep( dev, 0x82 ), p );
        CHECK_EQ( m->submit_request( req ), lp::RS2_USB_STATUS_SUCCESS );
        h.e.core->shutdown();
        CHECK( wait_for( [&] { return p.count() == 1; } ) );
        CHECK_EQ( h.e.mock->counters().hand_backs.size(), 1 );
        uint8_t b[4];
        uint32_t n = 0;
        CHECK_EQ( m->control_transfer( 0xA1, 0x81, 0, 0, b, 4, n, 100 ), lp::RS2_USB_STATUS_NO_DEVICE );
        for( int i = 0; i < 4; ++i )
        {
            CHECK( lp::usb_enumerator::query_devices_info().empty() );
            sleep_ms( 600 );
        }
        CHECK( lp::usb_broker::instance() != nullptr );
        req.reset();
        m.reset();
    }

    // OPEN with no answer (wedged claim, watchdog later than the deadline): nullptr at the deadline, session end,
    // new session afterwards
    CASE( h3_open_deadline )
    {
        timing t = fast();
        t.watchdog_ms = RSUSB_DEVICE_CALL_TIMEOUT_MS + 10000;
        h3_env h( { "d455.plist" }, t );
        h.up();
        auto dev = h3_device( "1-1-1" );
        CHECK( dev );
        h.e.mock->wedge( 1, mock_backend::op::claim );
        auto start = clk::now();
        CHECK( dev->open( 0 ) == nullptr );
        double ms = ms_since( start );
        printf( "open with a wedged claim: nullptr after %.0f ms\n", ms );
        CHECK( ms > RSUSB_DEVICE_CALL_TIMEOUT_MS - 100 && ms < RSUSB_DEVICE_CALL_TIMEOUT_MS + 1000 );
        CHECK( lp::usb_enumerator::query_devices_info().empty() );
        h.e.mock->unwedge();
        CHECK( wait_for( [&] { return h3_listed( "1-1-1" ); }, 3000 ) );
        CHECK( dev->open( 0 ) == nullptr );
        auto dev2 = h3_device( "1-1-1" );
        CHECK( dev2 );
        auto m = dev2->open( 0 );
        CHECK( m );
        CHECK( wait_for( [&] { return h.e.core->stats().connections == 1 && h.e.core->stats().messengers == 1; } ) );
    }

    // Messengers still alive when their device is lost CLOSE their daemon handle without waiting; otherwise the
    // connection's handle table (and OPEN quota) fills up over a few reconnects
    CASE( h3_gone_handles_closed )
    {
        h3_env h( { "d455.plist" } );
        h.up();
        uint64_t tok = 1;
        for( int i = 0; i < 8; ++i )   // 8 x 3 handles > RSUSB_QUOTA_HANDLES_PER_DEVICE
        {
            CHECK( wait_for( [&] { return h3_listed( "1-1-1" ); }, 3000 ) );
            auto d0 = h3_device( "1-1-1" ), d3 = h3_device( "1-1-1" ), d5 = h3_device( "1-1-1" );
            CHECK( d0 && d3 && d5 );
            auto m0 = d0->open( 0 ), m3 = d3->open( 3 ), m5 = d5->open( 5 );
            CHECK( m0 && m3 && m5 );
            h.e.mock->unplug( tok );
            uint8_t b[4];
            uint32_t n = 0;
            CHECK( wait_for( [&] { return m0->control_transfer( 0xA1, 0x81, 0, 0, b, 4, n, 100 ) == lp::RS2_USB_STATUS_NO_DEVICE; } ) );
            auto t = clk::now();
            m0.reset(), m3.reset(), m5.reset();
            CHECK( ms_since( t ) < 50 );
            d0.reset(), d3.reset(), d5.reset();
            tok = h.e.mock->plug( 0 );
        }
        CHECK( wait_for( [&] { return h.e.core->stats().messengers == 0 && h.e.mock->counters().handles == 0; } ) );
        printf( "8 losses with open messengers: every OPEN succeeded\n" );
    }

    // A capture in progress does not hide the device from query_devices_info or create_usb_device
    CASE( h3_capture_window )
    {
        timing t = fast();
        t.watchdog_ms = 30000;
        h3_env h( { "d455.plist" }, t );
        h.up();
        auto dev = h3_device( "1-1-1" );
        CHECK( dev );
        h.e.mock->wedge( 1, mock_backend::op::detach );
        lp::rs_usb_messenger m;
        std::thread th( [&] { m = dev->open( 0 ); } );
        sleep_ms( 300 );
        bool listed = h3_listed( "1-1-1" );
        auto other = h3_device( "1-1-1" );
        h.e.mock->unwedge();
        th.join();
        printf( "during capture: listed=%d create_usb_device=%s; open=%s\n", int( listed ), other ? "ok" : "nullptr",
                m ? "ok" : "nullptr" );
        CHECK( listed && other && m );
        CHECK( other->open( 3 ) != nullptr );
    }

    // H4 client side: 8 threads of CONTROL/BULK, 2 streaming requests at about 1 kHz, open/stream/close churn on a
    // third messenger
    CASE( h3_stress )
    {
        unsigned seconds = 20;
        if( const char * s = getenv( "RSUSBD_H3_STRESS_SECONDS" ) )
            seconds = unsigned( atoi( s ) );
        h3_env h( { "d455.plist" } );
        h.up();
        for( uint8_t ep : { 0x82, 0x83, 0x85, 0x87, 0x88 } )
            h.e.mock->set_stream_period( 1, ep, 1 );
        auto dev = h3_device( "1-1-1" );
        lp::rs_usb_messenger m0 = dev->open( 0 ), m5 = dev->open( 5 );
        CHECK( m0 && m5 );
        h3_probe p1, p2;
        p1.resubmits = p2.resubmits = 1 << 30;
        auto r1 = h3_request( m0, h3_ep( dev, 0x87 ), p1, 64 );
        auto r2 = h3_request( m5, h3_ep( dev, 0x85 ), p2, 64 );
        CHECK( m0->submit_request( r1 ) == 0 && m5->submit_request( r2 ) == 0 );
        std::atomic< bool > stop{ false };
        std::atomic< long > ops{ 0 }, errors{ 0 }, cycles{ 0 }, churn_errors{ 0 };
        std::vector< std::thread > th;
        for( int k = 0; k < 8; ++k )
            th.emplace_back( [&, k] {
                std::vector< uint8_t > b( 1024 );
                auto e82 = h3_ep( dev, 0x82 ), e83 = h3_ep( dev, 0x83 );
                while( ! stop )
                {
                    uint32_t n = 0;
                    lp::usb_status s;
                    if( k < 4 )
                        s = ( k % 2 ? m0 : m5 )->control_transfer( 0xA1, 0x81, k, k % 2 ? 0 : 5, b.data(), 64, n, 100 );
                    else
                        s = m0->bulk_transfer( k % 2 ? e82 : e83, b.data(), 1024, n, 100 );
                    if( s || ( k >= 4 && b[8] != ( k % 2 ? 0x82 : 0x83 ) ) )
                        errors++;
                    ops++;
                }
            } );
        std::thread churn( [&] {
            auto e88 = h3_ep( dev, 0x88 );
            while( ! stop )
            {
                auto m3 = dev->open( 3 );
                if( ! m3 )
                {
                    churn_errors++;
                    continue;
                }
                h3_probe pc;
                auto r = h3_request( m3, e88, pc, 64 );
                uint8_t b[16];
                uint32_t n = 0;
                if( m3->submit_request( r ) || m3->control_transfer( 0xA1, 0x81, 1, 3, b, 16, n, 100 ) )
                    churn_errors++;
                sleep_ms( 2 );
                r->get_callback()->cancel();   // as librealsense: callback first, then the request
                m3->cancel_request( r );
                cycles++;
            }
        } );
        sleep_ms( seconds * 1000 );
        stop = true;
        for( auto & x : th )
            x.join();
        churn.join();
        r1->get_callback()->cancel();
        r2->get_callback()->cancel();
        m0->cancel_request( r1 );
        m5->cancel_request( r2 );
        std::lock_guard< std::mutex > l1( p1.m ), l2( p2.m );
        printf( "h3 stress %us: sync ops %ld errors %ld, stream completions %d + %d, bad %d, wrong queue %d, "
                "open/stream/close cycles %ld errors %ld\n",
                seconds, ops.load(), errors.load(), p1.calls, p2.calls, p1.bad + p2.bad, p1.wrong_queue + p2.wrong_queue,
                cycles.load(), churn_errors.load() );
        CHECK( errors == 0 && churn_errors == 0 && p1.bad + p2.bad == 0 && p1.wrong_queue + p2.wrong_queue == 0 );
        CHECK( p1.calls > int( 300 * seconds ) && p2.calls > int( 300 * seconds ) && cycles > 10 * long( seconds ) );
    }
#endif

    int run_all( const char * self, unsigned jobs )
    {
        std::vector< std::string > names;
        for( auto & c : cases() )
            names.push_back( c.first );
        std::map< pid_t, std::pair< std::string, uint64_t > > running;
        size_t next = 0;
        int failed = 0;
        while( next < names.size() || ! running.empty() )
        {
            while( next < names.size() && running.size() < jobs )
            {
                std::string out = temp_base() + "/rs-usbd-tests." + names[next] + ".log";
                const char * argv[] = { self, "--fixtures", g_fixtures.c_str(), "--case", names[next].c_str(), nullptr };
                posix_spawn_file_actions_t fa;
                posix_spawn_file_actions_init( &fa );
                posix_spawn_file_actions_addopen( &fa, 2, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644 );
                pid_t pid;
                if( posix_spawn( &pid, self, &fa, nullptr, const_cast< char ** >( argv ), environ ) == 0 )
                    running[pid] = { names[next], now_ms() };
                posix_spawn_file_actions_destroy( &fa );
                ++next;
            }
            int status;
            pid_t pid = waitpid( -1, &status, WNOHANG );
            if( pid <= 0 )
            {
                for( auto & r : running )
                    if( now_ms() - r.second.second > 60000 )
                        kill( r.first, SIGKILL );
                sleep_ms( 10 );
                continue;
            }
            auto it = running.find( pid );
            if( it == running.end() )
                continue;
            bool ok = WIFEXITED( status ) && WEXITSTATUS( status ) == 0;
            std::string log = temp_base() + "/rs-usbd-tests." + it->second.first + ".log";
            printf( "%-48s %s (%llu ms)\n", it->second.first.c_str(), ok ? "PASS" : "FAIL", (unsigned long long)( now_ms() - it->second.second ) );
            if( ! ok )
            {
                ++failed;
                std::ifstream f( log );
                std::string line;
                while( std::getline( f, line ) )
                    if( line.find( "CHECK" ) != std::string::npos || line.find( "ERROR" ) != std::string::npos
                        || line.find( "Sanitizer" ) != std::string::npos )
                        printf( "    %s\n", line.c_str() );
                if( WIFSIGNALED( status ) )
                    printf( "    signal %d\n", WTERMSIG( status ) );
            }
            else
                unlink( log.c_str() );
            running.erase( it );
        }
        printf( "%zu cases, %d failed\n", names.size(), failed );
        return failed ? 1 : 0;
    }
}

int main( int argc, char ** argv )
{
    std::string one;
    unsigned jobs = 6, seconds = 0;
    for( int i = 1; i < argc; ++i )
    {
        std::string a = argv[i];
        if( a == "--fixtures" && i + 1 < argc )
            g_fixtures = argv[++i];
        else if( a == "--case" && i + 1 < argc )
            one = argv[++i];
        else if( a == "--jobs" && i + 1 < argc )
            jobs = unsigned( atoi( argv[++i] ) );
        else if( a == "--stress" && i + 1 < argc )
            seconds = unsigned( atoi( argv[++i] ) );
        else if( a == "--list" )
        {
            for( auto & c : cases() )
                printf( "%s\n", c.first.c_str() );
            return 0;
        }
        else
            return fprintf( stderr, "usage: rs-usbd-tests [--fixtures <dir>] [--jobs N] [--case <name> | --stress <s> | --list]\n" ), 64;
    }
    signal( SIGPIPE, SIG_IGN );
    if( seconds )
        return stress( seconds );
    if( ! one.empty() )
    {
        auto it = cases().find( one );
        if( it == cases().end() )
            return fprintf( stderr, "no case %s\n", one.c_str() ), 64;
        it->second();
        fflush( stdout );
        _exit( 0 );   // skip global destructors while daemon queues may still run
    }
    char self[4096];
    uint32_t size = sizeof( self );
    _NSGetExecutablePath( self, &size );
    return run_all( self, jobs );
}
