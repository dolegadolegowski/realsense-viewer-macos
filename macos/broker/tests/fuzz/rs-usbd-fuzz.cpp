// License: Apache 2.0. See LICENSE file in root directory.
// H5 fuzz harness (DESIGN 9.1). Apple clang ships no libFuzzer runtime, so this binary carries a small
// coverage-guided driver (-fsanitize-coverage=trace-pc-guard) around three LLVMFuzzerTestOneInput-style targets:
//   session   bytes -> XPC dictionaries (typed protocol keys, random values and types) into the core's session
//             handler, mock backend, fake policy, reply sink; every input must leave no daemon state behind
//   snapshot  bytes -> libusb descriptor structs into the snapshot builder; limits and XPC round trip checked
//   enumerate bytes -> ENUMERATE reply dictionaries into the reply validators (daemon side, and the client's
//             parse_enumerate_reply when broker-wire.cpp is available)
//
//   rs-usbd-fuzz <target> [--seconds N] [--corpus <dir>] [--seed N] [--fixtures <dir>]
//   rs-usbd-fuzz <target> --run <file>...      replay inputs

#include "daemon/core/server.h"
#include "daemon/core/xpc-util.h"
#include "daemon/snapshot-libusb.h"
#include "tests/backend-mock.h"
#include "tests/policy-fake.h"
#include "rs-usb-broker-protocol.h"
#ifdef RSUSBD_CLIENT_VALIDATOR
#include "broker-wire.h"
#endif

#include <dirent.h>
#include <sanitizer/common_interface_defs.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace usbd;

// ---------------------------------------------------------------------------------------------------------------------
// Coverage

static uint8_t * g_cov = nullptr;
static size_t g_cov_n = 0;

extern "C" void __sanitizer_cov_trace_pc_guard_init( uint32_t * start, uint32_t * stop )
{
    static uint32_t n = 0;
    if( start == stop || *start )
        return;
    for( uint32_t * x = start; x < stop; ++x )
        *x = ++n;
    g_cov_n = n + 1;
    g_cov = static_cast< uint8_t * >( realloc( g_cov, g_cov_n ) );
    memset( g_cov, 0, g_cov_n );
}

extern "C" void __sanitizer_cov_trace_pc_guard( uint32_t * guard )
{
    if( *guard && *guard < g_cov_n )
        g_cov[*guard] = 1;
}

namespace
{
    struct reader
    {
        const uint8_t * p;
        size_t n, i = 0;
        uint8_t u8() { return i < n ? p[i++] : 0; }
        uint64_t u64()
        {
            uint64_t v = 0;
            for( int k = 0; k < 8; ++k )
                v = v << 8 | u8();
            return v;
        }
        bool done() const { return i >= n; }
        std::vector< uint8_t > bytes( size_t max )
        {
            size_t len = std::min< size_t >( u8() % ( max + 1 ), n - std::min( n, i ) );
            std::vector< uint8_t > v( p + i, p + i + len );
            i += len;
            return v;
        }
    };

    std::string g_fixtures = "macos/broker/tests/fixtures";

    // ---------------------------------------------------------------------------------------------------------------
    // session target

    const char * const KEYS[] = { RSUSB_KEY_OP, RSUSB_KEY_VERSION, RSUSB_KEY_TOKEN, RSUSB_KEY_INTERFACE, RSUSB_KEY_HANDLE,
                                  RSUSB_KEY_REQUEST_TYPE, RSUSB_KEY_REQUEST, RSUSB_KEY_VALUE, RSUSB_KEY_INDEX, RSUSB_KEY_LENGTH,
                                  RSUSB_KEY_TIMEOUT, RSUSB_KEY_DATA, RSUSB_KEY_ENDPOINT, RSUSB_KEY_REQUEST_ID, RSUSB_KEY_SHMEM };

    uint64_t interesting( reader & r, const char * key )
    {
        uint8_t mode = r.u8();
        if( mode < 16 )
            return r.u64();
        uint8_t b = r.u8();
        if( ! strcmp( key, RSUSB_KEY_OP ) )
            return 1 + b % 13;
        if( ! strcmp( key, RSUSB_KEY_TOKEN ) || ! strcmp( key, RSUSB_KEY_HANDLE ) || ! strcmp( key, RSUSB_KEY_REQUEST_ID ) )
            return b % 6;
        if( ! strcmp( key, RSUSB_KEY_INTERFACE ) )
            return b % 8;
        if( ! strcmp( key, RSUSB_KEY_ENDPOINT ) )
            return ( b & 1 ? 0x80 : 0 ) | ( b >> 1 ) % 9;
        if( ! strcmp( key, RSUSB_KEY_REQUEST_TYPE ) )
            return b & 1 ? 0x21 : b & 2 ? 0xA1 : b;
        if( ! strcmp( key, RSUSB_KEY_LENGTH ) )
            return b < 200 ? b : b < 250 ? uint64_t( b ) * 97 : RSUSB_MAX_REQUEST_LEN + ( b & 1 );
        if( ! strcmp( key, RSUSB_KEY_TIMEOUT ) )
            return b % 4 * 20;
        if( ! strcmp( key, RSUSB_KEY_VERSION ) )
            return b % 3;
        return b % 2 ? b : uint64_t( b ) << 8 | r.u8();
    }

    xpc_object_t make_message( reader & r )
    {
        xpc_object_t d = xpc_dictionary_create( nullptr, nullptr, 0 );
        unsigned nkeys = r.u8() % 9;
        for( unsigned k = 0; k < nkeys; ++k )
        {
            const char * key = KEYS[r.u8() % ( sizeof( KEYS ) / sizeof( KEYS[0] ) )];
            uint8_t type = r.u8() % 16;
            if( type < 11 )
                xpc_dictionary_set_uint64( d, key, interesting( r, key ) );
            else if( type == 11 )
                xpc_dictionary_set_int64( d, key, int64_t( r.u64() ) );
            else if( type == 12 )
            {
                auto v = r.bytes( 255 );
                if( r.u8() & 1 && v.size() >= 5 )
                    v[2] = 0xAB, v[3] = 0xCD;
                xpc_dictionary_set_data( d, key, v.data(), v.size() );
            }
            else if( type == 13 )
                xpc_dictionary_set_string( d, key, "x" );
            else if( type == 14 )
                xpc_dictionary_set_bool( d, key, r.u8() & 1 );
            else
            {
                xpc_object_t a = xpc_array_create( nullptr, 0 );
                xpc_dictionary_set_value( d, key, a );
                xpc_release( a );
            }
        }
        return d;
    }

    struct session_target
    {
        usbd_core * core = nullptr;
        mock_backend * mock = nullptr;
        fake_policy * pol = nullptr;
        std::atomic< uint64_t > sent{ 0 };

        session_target()
        {
            std::vector< fixture_device > f;
            std::string err;
            if( ! load_fixture_file( g_fixtures + "/d455.plist", f, err ) || ! load_fixture_file( g_fixtures + "/recovery.plist", f, err ) )
            {
                fprintf( stderr, "fixtures: %s\n", err.c_str() );
                exit( 2 );
            }
            mock = new mock_backend( f );
            mock->set_stream_period( 1, 0x82, 2 );
            mock->set_stream_period( 1, 0x87, 2 );
            pol = new fake_policy;
            config c;
            c.build_id = std::string( 64, 'a' );
            c.cdhashes = { std::string( 40, '0' ) };
            timing t;
            t.grace_ms = 50;
            t.watchdog_ms = 2000;
            paths p;
            char tmpl[] = "/private/tmp/rs-usbd-fuzz.XXXXXX";
            p.state_dir = std::string( mkdtemp( tmpl ) ) + "/state";
            p.require_root = false;
            core = new usbd_core( std::unique_ptr< usb_backend >( mock ), std::unique_ptr< peer_policy >( pol ), c, t, p );
            core->start( false );
            core->sweep();
        }

        void run( const uint8_t * data, size_t size )
        {
            reader r{ data, size };
            auto s = core->open_session( [this]( xpc_object_t m ) {
                if( xpc_get_type( m ) != XPC_TYPE_DICTIONARY )
                    abort();
                ++sent;
            } );
            if( r.u8() < 230 )   // usually start READY so later ops are reached
            {
                xpc_object_t h = xpc_dictionary_create( nullptr, nullptr, 0 );
                xpc_dictionary_set_uint64( h, RSUSB_KEY_OP, RSUSB_OP_HELLO );
                xpc_dictionary_set_uint64( h, RSUSB_KEY_VERSION, RSUSB_PROTOCOL_VERSION );
                core->session_message( s, h );
                xpc_release( h );
            }
            for( unsigned n = 0; ! r.done() && n < 64; ++n )
            {
                xpc_object_t m = make_message( r );
                core->session_message( s, m );
                xpc_release( m );
                if( r.u8() % 8 == 0 )
                    usleep( 1000 );   // let completions interleave
            }
            core->close_session( s );
            // every input must leave nothing behind (handles, requests, shm, leases)
            for( int i = 0; i < 4000; ++i )
            {
                auto st = core->stats();
                auto mc = mock->counters();
                if( st.connections == 0 && st.messengers == 0 && st.requests == 0 && st.shm_bytes == 0 && st.inflight_bytes == 0
                    && st.leases == 0 && mc.handles == 0 && mc.live_transfers == 0 )
                    return;
                usleep( 1000 );
            }
            auto st = core->stats();
            fprintf( stderr, "state leaked: connections %u messengers %u requests %u shm %llu inflight %llu leases %u\n", st.connections,
                     st.messengers, st.requests, (unsigned long long)st.shm_bytes, (unsigned long long)st.inflight_bytes, st.leases );
            abort();
        }
    };

    // ---------------------------------------------------------------------------------------------------------------
    // snapshot target

    void run_snapshot( const uint8_t * data, size_t size )
    {
        reader r{ data, size };
        libusb_device_descriptor dd{};
        dd.bLength = 18;
        dd.bDescriptorType = 1;
        dd.idVendor = r.u8() & 1 ? 0x8086 : 0x38E5;
        dd.idProduct = uint16_t( r.u8() << 8 | r.u8() );
        dd.bNumConfigurations = r.u8() % 4;
        std::vector< libusb_config_descriptor > cfgs( dd.bNumConfigurations );
        std::vector< const libusb_config_descriptor * > ptrs( dd.bNumConfigurations );
        std::vector< std::vector< libusb_interface > > ifs( dd.bNumConfigurations );
        std::vector< std::vector< std::vector< libusb_interface_descriptor > > > alts( dd.bNumConfigurations );
        std::deque< std::vector< uint8_t > > extras;   // stable addresses
        std::deque< std::vector< libusb_endpoint_descriptor > > eps;
        for( int c = 0; c < dd.bNumConfigurations; ++c )
        {
            auto & cfg = cfgs[c];
            cfg = {};
            cfg.wTotalLength = uint16_t( r.u8() << 8 | r.u8() );
            cfg.bNumInterfaces = r.u8() % 40;
            ifs[c].resize( cfg.bNumInterfaces );
            alts[c].resize( cfg.bNumInterfaces );
            for( int i = 0; i < cfg.bNumInterfaces; ++i )
            {
                int na = r.u8() % 4 == 0 ? r.u8() % 140 : r.u8() % 3;
                alts[c][i].resize( size_t( na ) );
                for( int a = 0; a < na; ++a )
                {
                    auto & d = alts[c][i][size_t( a )];
                    d = {};
                    d.bLength = 9;
                    d.bDescriptorType = 4;
                    d.bInterfaceNumber = r.u8();
                    d.bAlternateSetting = uint8_t( a );
                    d.bInterfaceClass = r.u8() % 2 ? 0x0E : r.u8();
                    d.bInterfaceSubClass = r.u8() % 4;
                    d.bInterfaceProtocol = r.u8() % 4;
                    d.bNumEndpoints = r.u8() % 40;
                    eps.emplace_back( d.bNumEndpoints );
                    for( auto & e : eps.back() )
                        e = { 7, 5, r.u8(), r.u8(), uint16_t( r.u8() << 8 | r.u8() ), r.u8(), 0, 0, nullptr, 0 };
                    d.endpoint = eps.back().data();
                    size_t xl = r.u8() % 8 == 0 ? size_t( r.u8() ) * 300 : r.u8() % 64;
                    extras.emplace_back( xl, uint8_t( r.u8() ) );
                    d.extra = extras.back().data();
                    d.extra_length = int( xl );
                }
                ifs[c][i].altsetting = alts[c][i].data();
                ifs[c][i].num_altsetting = na;
            }
            cfg.interface = ifs[c].data();
            ptrs[c] = r.u8() % 16 == 0 ? nullptr : &cfg;
        }
        uint8_t ports[8];
        int nports = r.u8() % 9;
        for( auto & p : ports )
            p = r.u8();
        snapshot_input in;
        in.bus = r.u8();
        in.ports = ports;
        in.port_count = nports;
        in.address = r.u8();
        in.desc = &dd;
        in.configs = ptrs.data();
        snapshot s;
        if( ! build_snapshot( in, s ) )
            return;
        if( ! snapshot_within_limits( s ) )
            abort();
        xpc_object_t x = snapshot_to_xpc( s, 7 );
        snapshot back;
        uint64_t tok;
        if( ! snapshot_from_xpc( x, back, tok ) || tok != 7 || back.ifs.size() != s.ifs.size() || back.path != s.path )
            abort();
        for( size_t i = 0; i < s.ifs.size(); ++i )
            if( back.ifs[i].eps != s.ifs[i].eps || back.ifs[i].alts.size() != s.ifs[i].alts.size() )
                abort();
#ifdef RSUSBD_CLIENT_VALIDATOR
        xpc_object_t reply = xpc_dictionary_create( nullptr, nullptr, 0 );
        xpc_object_t arr = xpc_array_create( &x, 1 );
        xpc_dictionary_set_int64( reply, RSUSB_KEY_STATUS, 0 );
        xpc_dictionary_set_value( reply, RSUSB_KEY_DEVICES, arr );
        std::vector< librealsense::platform::broker_device_snapshot > devs;
        if( ! librealsense::platform::parse_enumerate_reply( reply, devs ) )   // the client must accept what the daemon sends
        {
            fprintf( stderr, "client validator rejected a daemon snapshot\n" );
            abort();
        }
        xpc_release( arr );
        xpc_release( reply );
#endif
        xpc_release( x );
    }

    // ---------------------------------------------------------------------------------------------------------------
    // enumerate target

    xpc_object_t random_value( reader & r, int depth );

    xpc_object_t random_data( reader & r, size_t typical )
    {
        size_t len = r.u8() % 4 == 0 ? r.u8() : typical;
        std::vector< uint8_t > v( len );
        for( auto & b : v )
            b = r.u8();
        return xpc_data_create( v.data(), v.size() );
    }

    xpc_object_t random_value( reader & r, int depth )
    {
        switch( r.u8() % 6 )
        {
        case 0: return xpc_uint64_create( r.u8() % 4 ? r.u8() : r.u64() );
        case 1: return xpc_int64_create( int64_t( r.u64() ) );
        case 2: return random_data( r, r.u8() % 20 );
        case 3: return xpc_string_create( r.u8() % 2 ? "1-1-1" : std::string( r.u8(), 'x' ).c_str() );
        case 4:
        {
            xpc_object_t a = xpc_array_create( nullptr, 0 );
            for( int i = 0, n = depth > 3 ? 0 : r.u8() % 4; i < n; ++i )
            {
                xpc_object_t v = random_value( r, depth + 1 );
                xpc_array_append_value( a, v );
                xpc_release( v );
            }
            return a;
        }
        default: return xpc_bool_create( r.u8() & 1 );
        }
    }

    // mostly well-formed devices with one field perturbed at a time
    xpc_object_t random_device( reader & r )
    {
        xpc_object_t d = xpc_dictionary_create( nullptr, nullptr, 0 );
        auto field = [&]( const char * k, xpc_object_t good ) {
            uint8_t m = r.u8();
            if( m < 220 )
                xpc_dictionary_set_value( d, k, good );
            else if( m < 240 )
            {
                xpc_object_t v = random_value( r, 1 );
                xpc_dictionary_set_value( d, k, v );
                xpc_release( v );
            }
            xpc_release( good );
        };
        field( RSUSB_KEY_TOKEN, xpc_uint64_create( r.u8() ) );
        field( RSUSB_KEY_PATH, xpc_string_create( "1-2.3-4" ) );
        field( RSUSB_KEY_DEVICE_DESC, random_data( r, 18 ) );
        xpc_object_t ifs = xpc_array_create( nullptr, 0 );
        for( int i = 0, n = r.u8() % 8; i < n; ++i )
        {
            xpc_object_t xi = xpc_dictionary_create( nullptr, nullptr, 0 );
            xpc_dictionary_set_uint64( xi, RSUSB_KEY_CONFIG, r.u8() % 3 );
            xpc_object_t alts = xpc_array_create( nullptr, 0 );
            for( int a = 0, na = r.u8() % 3; a < na; ++a )
            {
                xpc_object_t xa = xpc_dictionary_create( nullptr, nullptr, 0 );
                xpc_object_t dd = random_data( r, 9 ), xx = random_data( r, r.u8() % 40 );
                xpc_dictionary_set_value( xa, RSUSB_KEY_INTERFACE_DESC, dd );
                xpc_dictionary_set_value( xa, RSUSB_KEY_EXTRA, xx );
                xpc_release( dd );
                xpc_release( xx );
                xpc_array_append_value( alts, xa );
                xpc_release( xa );
            }
            xpc_dictionary_set_value( xi, RSUSB_KEY_ALTSETTINGS, alts );
            xpc_release( alts );
            xpc_object_t e = random_data( r, 7 * ( r.u8() % 4 ) );
            xpc_dictionary_set_value( xi, RSUSB_KEY_ENDPOINTS, e );
            xpc_release( e );
            xpc_array_append_value( ifs, xi );
            xpc_release( xi );
        }
        field( RSUSB_KEY_INTERFACES, ifs );
        return d;
    }

    void run_enumerate( const uint8_t * data, size_t size )
    {
        reader r{ data, size };
        xpc_object_t reply = xpc_dictionary_create( nullptr, nullptr, 0 );
        xpc_dictionary_set_int64( reply, RSUSB_KEY_STATUS, r.u8() % 8 ? 0 : -4 );
        xpc_object_t devs = xpc_array_create( nullptr, 0 );
        for( int i = 0, n = r.u8() % 40; i < n; ++i )
        {
            xpc_object_t d = random_device( r );
            xpc_array_append_value( devs, d );
            snapshot s;
            uint64_t tok;
            if( snapshot_from_xpc( d, s, tok ) && ( ! tok || ! snapshot_within_limits( s ) ) )
                abort();
            xpc_release( d );
        }
        if( r.u8() % 16 )
            xpc_dictionary_set_value( reply, RSUSB_KEY_DEVICES, devs );
        xpc_release( devs );
#ifdef RSUSBD_CLIENT_VALIDATOR
        std::vector< librealsense::platform::broker_device_snapshot > out;
        if( librealsense::platform::parse_enumerate_reply( reply, out ) )
            for( auto & d : out )
                if( d.device_desc.size() != RSUSB_DEVICE_DESC_LEN || ! d.token || d.path.empty() || d.path.size() > RSUSB_MAX_PATH_LEN )
                    abort();
#endif
        xpc_release( reply );
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Driver

    std::vector< uint8_t > g_current;
    std::string g_crash_path = "rs-usbd-fuzz-crash.bin";

    void save_crash()
    {
        FILE * f = fopen( g_crash_path.c_str(), "wb" );
        if( f )
        {
            fwrite( g_current.data(), 1, g_current.size(), f );
            fclose( f );
            fprintf( stderr, "crashing input saved to %s\n", g_crash_path.c_str() );
        }
    }

    std::vector< uint8_t > read_file( const std::string & p )
    {
        std::ifstream in( p, std::ios::binary );
        return std::vector< uint8_t >( ( std::istreambuf_iterator< char >( in ) ), std::istreambuf_iterator< char >() );
    }

    size_t coverage()
    {
        size_t n = 0;
        for( size_t i = 0; i < g_cov_n; ++i )
            n += g_cov[i];
        return n;
    }
}

int main( int argc, char ** argv )
{
    if( argc < 2 )
        return fprintf( stderr, "usage: rs-usbd-fuzz session|snapshot|enumerate [--seconds N] [--corpus dir] [--seed N] [--fixtures dir] [--run files...]\n" ), 64;
    std::string target = argv[1], corpus;
    unsigned seconds = 60;
    uint64_t seed = 1;
    std::vector< std::string > replay;
    for( int i = 2; i < argc; ++i )
    {
        std::string a = argv[i];
        if( a == "--seconds" && i + 1 < argc )
            seconds = unsigned( atoi( argv[++i] ) );
        else if( a == "--corpus" && i + 1 < argc )
            corpus = argv[++i];
        else if( a == "--seed" && i + 1 < argc )
            seed = strtoull( argv[++i], nullptr, 10 );
        else if( a == "--fixtures" && i + 1 < argc )
            g_fixtures = argv[++i];
        else if( a == "--run" )
            while( i + 1 < argc )
                replay.push_back( argv[++i] );
    }
    session_target * session = target == "session" ? new session_target : nullptr;
    auto run = [&]( const std::vector< uint8_t > & in ) {
        g_current = in;
        if( session )
            session->run( in.data(), in.size() );
        else if( target == "snapshot" )
            run_snapshot( in.data(), in.size() );
        else if( target == "enumerate" )
            run_enumerate( in.data(), in.size() );
        else
        {
            fprintf( stderr, "unknown target %s\n", target.c_str() );
            exit( 64 );
        }
    };
    __sanitizer_set_death_callback( save_crash );
    if( ! replay.empty() )
    {
        for( auto & f : replay )
            run( read_file( f ) );
        printf( "replayed %zu inputs\n", replay.size() );
        fflush( stdout );
        _exit( 0 );
    }

    std::vector< std::vector< uint8_t > > pool;
    if( ! corpus.empty() )
    {
        mkdir( corpus.c_str(), 0755 );
        if( DIR * d = opendir( corpus.c_str() ) )
        {
            while( dirent * e = readdir( d ) )
                if( e->d_name[0] != '.' )
                    pool.push_back( read_file( corpus + "/" + e->d_name ) );
            closedir( d );
        }
    }
    std::mt19937_64 rng( seed );
    if( pool.empty() )
        pool.push_back( std::vector< uint8_t >( 64, 0 ) );
    for( auto & p : pool )
        run( p );
    size_t best = coverage(), initial = pool.size(), added = 0;
    uint64_t execs = 0, start = now_ms(), end = start + seconds * 1000ull, last_report = start;
    while( now_ms() < end )
    {
        std::vector< uint8_t > in = pool[rng() % pool.size()];
        int edits = 1 + int( rng() % 8 );
        for( int k = 0; k < edits; ++k )
        {
            switch( rng() % 6 )
            {
            case 0: if( ! in.empty() ) in[rng() % in.size()] ^= uint8_t( 1u << ( rng() % 8 ) ); break;
            case 1: if( ! in.empty() ) in[rng() % in.size()] = uint8_t( rng() ); break;
            case 2: if( in.size() < 4096 ) in.insert( in.begin() + long( rng() % ( in.size() + 1 ) ), uint8_t( rng() ) ); break;
            case 3: if( ! in.empty() ) in.erase( in.begin() + long( rng() % in.size() ) ); break;
            case 4:
            {
                auto & other = pool[rng() % pool.size()];
                if( ! other.empty() && ! in.empty() )
                {
                    size_t at = rng() % in.size(), from = rng() % other.size(), len = std::min< size_t >( rng() % 32, other.size() - from );
                    for( size_t q = 0; q < len && at + q < in.size(); ++q )
                        in[at + q] = other[from + q];
                }
                break;
            }
            default: if( in.size() < 4096 ) for( int q = 0; q < 8; ++q ) in.push_back( uint8_t( rng() ) ); break;
            }
        }
        run( in );
        ++execs;
        size_t cov = coverage();
        if( cov > best )
        {
            best = cov;
            pool.push_back( in );
            ++added;
            if( ! corpus.empty() )
            {
                char name[64];
                snprintf( name, sizeof( name ), "/%s-%06zu", target.c_str(), pool.size() );
                std::ofstream( corpus + name, std::ios::binary ).write( reinterpret_cast< const char * >( in.data() ), std::streamsize( in.size() ) );
            }
        }
        if( now_ms() - last_report > 60000 )
        {
            last_report = now_ms();
            fprintf( stderr, "%s: %llu execs, %zu edges, corpus %zu\n", target.c_str(), (unsigned long long)execs, best, pool.size() );
        }
    }
    printf( "%s: %llu execs in %us (%.0f/s), %zu edges covered, corpus %zu (+%zu new), no crash\n", target.c_str(),
            (unsigned long long)execs, seconds, execs / double( std::max( 1u, seconds ) ), best, pool.size(), added );
    (void)initial;
    fflush( stdout );
    _exit( 0 );   // the core's queues are still running
}
