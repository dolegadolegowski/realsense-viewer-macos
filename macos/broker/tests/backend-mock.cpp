// License: Apache 2.0. See LICENSE file in root directory.
#include "backend-mock.h"
#include "daemon/core/hwm.h"
#include "daemon/core/xpc-util.h"
#include "rs-usb-broker-protocol.h"

#include <cstdlib>

namespace usbd
{
    struct mock_backend::xfer : usb_transfer
    {
        bool inflight = false, held = false;
        uint64_t gen = 0, key = 0;
        int pend_xs = RSUSB_XS_COMPLETED;
        uint32_t pend_n = 0;
    };

    struct mock_backend::mdev
    {
        size_t fixture = 0;
        fixture_device f;
        bool present = true, captured = false, drivers = true, incomplete = false;
        uint64_t registry_id = 0;
        std::map< uint8_t, uint64_t > claimed;   // interface -> handle
        std::set< uint8_t > held_elsewhere, hold;
        std::map< uint8_t, uint64_t > seq;
        std::map< uint8_t, unsigned > period;
        struct { bool pending = false; uint8_t opcode = 0, sel = 0, unit = 0; int iface = -1; } hwm;
        struct scripted { uint64_t rt, rq, val, idx; std::vector< uint8_t > data; };
        std::vector< scripted > script;

        int iface_of( uint8_t ep ) const
        {
            for( auto & i : f.snap.ifs )
                if( i.cfg == f.active_config )
                    for( auto & e : i.eps )
                        if( e[2] == ep )
                            return i.number();
            return -1;
        }
    };

    namespace
    {
        uint64_t next_gen = 0;   // under the mock mutex; globally unique so a recycled transfer never matches a stale timer

        int64_t num( CFDictionaryRef d, CFStringRef k, int64_t dflt )
        {
            auto n = static_cast< CFNumberRef >( CFDictionaryGetValue( d, k ) );
            int64_t v = dflt;
            if( n && CFGetTypeID( n ) == CFNumberGetTypeID() )
                CFNumberGetValue( n, kCFNumberSInt64Type, &v );
            return v;
        }
    }

    mock_backend::mock_backend( std::vector< fixture_device > fixtures )
        : _fixtures( std::move( fixtures ) )
    {
        _q = dispatch_queue_create( "mock.events", DISPATCH_QUEUE_SERIAL );
        for( size_t i = 0; i < _fixtures.size(); ++i )   // tokens 1, 2, ... exist before start() so tests can arm controls
            add_device( i, _fixtures[i].drivers_attached );
    }

    mock_backend::~mock_backend() = default;

    uint64_t mock_backend::add_device( size_t fi, bool drivers )
    {
        static std::map< size_t, unsigned > instances;
        auto d = std::make_unique< mdev >();
        d->fixture = fi;
        d->f = _fixtures[fi];
        d->drivers = drivers;
        d->registry_id = d->f.registry_id + 0x1000000ull * instances[fi]++;
        if( CFDictionaryRef m = d->f.mock )
        {
            auto ci = static_cast< CFArrayRef >( CFDictionaryGetValue( m, CFSTR( "ControlIn" ) ) );
            for( CFIndex i = 0; ci && CFGetTypeID( ci ) == CFArrayGetTypeID() && i < CFArrayGetCount( ci ); ++i )
            {
                auto e = static_cast< CFDictionaryRef >( CFArrayGetValueAtIndex( ci, i ) );
                mdev::scripted s{ uint64_t( num( e, CFSTR( "rt" ), 0 ) ), uint64_t( num( e, CFSTR( "rq" ), 0 ) ),
                                  uint64_t( num( e, CFSTR( "val" ), 0 ) ), uint64_t( num( e, CFSTR( "idx" ), 0 ) ), {} };
                auto data = static_cast< CFDataRef >( CFDictionaryGetValue( e, CFSTR( "data" ) ) );
                if( data && CFGetTypeID( data ) == CFDataGetTypeID() )
                    s.data.assign( CFDataGetBytePtr( data ), CFDataGetBytePtr( data ) + CFDataGetLength( data ) );
                d->script.push_back( s );
            }
            auto sp = static_cast< CFDictionaryRef >( CFDictionaryGetValue( m, CFSTR( "StreamPeriodMs" ) ) );
            if( sp && CFGetTypeID( sp ) == CFDictionaryGetTypeID() )
            {
                CFIndex n = CFDictionaryGetCount( sp );
                std::vector< const void * > keys( static_cast< size_t >( n ) ), vals( static_cast< size_t >( n ) );
                CFDictionaryGetKeysAndValues( sp, keys.data(), vals.data() );
                for( CFIndex i = 0; i < n; ++i )
                {
                    char buf[16];
                    int64_t v = 0;
                    if( CFGetTypeID( keys[size_t( i )] ) == CFStringGetTypeID()
                        && CFStringGetCString( static_cast< CFStringRef >( keys[size_t( i )] ), buf, sizeof( buf ), kCFStringEncodingUTF8 )
                        && CFGetTypeID( vals[size_t( i )] ) == CFNumberGetTypeID()
                        && CFNumberGetValue( static_cast< CFNumberRef >( vals[size_t( i )] ), kCFNumberSInt64Type, &v ) )
                        d->period[uint8_t( strtoul( buf, nullptr, 0 ) )] = unsigned( v );
                }
            }
        }
        uint64_t key = _next_key++;
        _devs[key] = std::move( d );
        _last_plugged = key;
        return key;
    }

    void mock_backend::start( const events & ev )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _ev = ev;
        if( _started )
            return;
        _started = true;
        for( auto & d : _devs )
        {
            uint64_t key = d.first;
            async( _q, [this, key] { _ev.arrived( key ); } );
        }
    }

    void mock_backend::stop() {}

    std::vector< uint64_t > mock_backend::list()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        std::vector< uint64_t > rv;
        for( auto & d : _devs )
            if( d.second->present && ( ! _ev.wanted || _ev.wanted( d.second->f.snap.vid(), d.second->f.snap.pid() ) ) )
                rv.push_back( d.first );
        return rv;
    }

    bool mock_backend::describe( uint64_t key, device_description & out )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto it = _devs.find( key );
        if( it == _devs.end() || ! it->second->present )
            return false;
        out.key = key;
        out.registry_id = it->second->registry_id;
        out.drivers_attached = it->second->drivers;
        out.complete = ! it->second->incomplete;
        out.active_config = it->second->f.active_config;
        out.snap = it->second->f.snap;
        return true;
    }

    void mock_backend::forget( uint64_t ) {}

    int mock_backend::injected( uint64_t key, op o )
    {
        auto it = _fail.find( { key, o } );
        if( it == _fail.end() || it->second.second <= 0 )
            return 0;
        int st = it->second.first;
        if( --it->second.second == 0 )
            _fail.erase( it );
        return st;
    }

    int mock_backend::wait_wedge( std::unique_lock< std::mutex > & lock, uint64_t key, op o )
    {
        auto present = [&] { auto it = _devs.find( key ); return it != _devs.end() && it->second->present; };
        while( _wedged.count( { key, o } ) && present() )
            _cv.wait( lock );
        return present() ? 0 : int( RSUSB_ST_NO_DEVICE );
    }

    int mock_backend::open( uint64_t key, bool anchor, uint64_t & handle )
    {
        std::unique_lock< std::mutex > lock( _mutex );
        if( int r = wait_wedge( lock, key, op::open ) )
            return r;
        if( int r = injected( key, op::open ) )
            return r;
        handle = _next_handle++;
        _handles[handle] = { key, anchor, {} };
        return 0;
    }

    int mock_backend::open_anchor( uint64_t key, uint64_t & handle ) { return open( key, true, handle ); }
    int mock_backend::open_handle( uint64_t key, uint64_t & handle ) { return open( key, false, handle ); }

    void mock_backend::close_handle( uint64_t handle )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto it = _handles.find( handle );
        if( it == _handles.end() )
            return;
        auto d = _devs.find( it->second.key );
        if( d != _devs.end() )
            for( uint8_t i : it->second.claimed )
                d->second->claimed.erase( i );
        _handles.erase( it );
    }

    void mock_backend::close_anchor( uint64_t handle ) { close_handle( handle ); }

    int mock_backend::kernel_driver_active( uint64_t handle, uint8_t )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto h = _handles.find( handle );
        if( h == _handles.end() || ! _devs[h->second.key]->present )
            return RSUSB_ST_NO_DEVICE;
        auto & d = *_devs[h->second.key];
        return d.drivers && ! d.captured ? 1 : 0;
    }

    int mock_backend::capture( uint64_t handle, uint8_t )
    {
        std::unique_lock< std::mutex > lock( _mutex );
        auto h = _handles.find( handle );
        if( h == _handles.end() )
            return RSUSB_ST_NO_DEVICE;
        uint64_t key = h->second.key;
        if( int r = wait_wedge( lock, key, op::detach ) )
            return r;
        if( int r = injected( key, op::detach ) )
            return r;
        auto & d = *_devs[key];
        d.captured = true;
        d.drivers = false;
        d.registry_id += 0x100;   // lets H2 check that the core re-reads the registry ID after the capture
        ++_counters.captures;
        return 0;
    }

    int mock_backend::active_config( uint64_t handle, int & index )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto h = _handles.find( handle );
        if( h == _handles.end() )
            return RSUSB_ST_NO_DEVICE;
        index = _devs[h->second.key]->f.active_config;
        return 0;
    }

    int mock_backend::claim( uint64_t handle, uint8_t iface )
    {
        std::unique_lock< std::mutex > lock( _mutex );
        auto h = _handles.find( handle );
        if( h == _handles.end() )
            return RSUSB_ST_NO_DEVICE;
        uint64_t key = h->second.key;
        if( int r = wait_wedge( lock, key, op::claim ) )
            return r;
        if( int r = injected( key, op::claim ) )
            return r;
        h = _handles.find( handle );
        if( h == _handles.end() )
            return RSUSB_ST_NO_DEVICE;
        auto & d = *_devs[key];
        if( ! find_interface( d.f.snap, d.f.active_config, iface ) )
            return RSUSB_ST_NOT_FOUND;
        auto c = d.claimed.find( iface );
        if( ( c != d.claimed.end() && c->second != handle ) || d.held_elsewhere.count( iface ) )
            return RSUSB_ST_ACCESS;
        d.claimed[iface] = handle;
        h->second.claimed.insert( iface );
        return 0;
    }

    int mock_backend::release( uint64_t handle, uint8_t iface )
    {
        std::unique_lock< std::mutex > lock( _mutex );
        auto h = _handles.find( handle );
        if( h == _handles.end() )
            return RSUSB_ST_NO_DEVICE;
        uint64_t key = h->second.key;
        if( int r = wait_wedge( lock, key, op::release ) )
            return r;
        if( int r = injected( key, op::release ) )
            return r;
        h = _handles.find( handle );
        if( h == _handles.end() || ! h->second.claimed.erase( iface ) )
            return RSUSB_ST_NOT_FOUND;
        _devs[key]->claimed.erase( iface );
        return 0;
    }

    int mock_backend::clear_halt( uint64_t handle, uint8_t ep )
    {
        std::unique_lock< std::mutex > lock( _mutex );
        auto h = _handles.find( handle );
        if( h == _handles.end() )
            return RSUSB_ST_NO_DEVICE;
        uint64_t key = h->second.key;
        if( int r = wait_wedge( lock, key, op::clear_halt ) )
            return r;
        if( int r = injected( key, op::clear_halt ) )
            return r;
        h = _handles.find( handle );
        auto & d = *_devs[key];
        if( ! d.present )
            return RSUSB_ST_NO_DEVICE;
        int i = d.iface_of( ep );
        return i >= 0 && h != _handles.end() && h->second.claimed.count( uint8_t( i ) ) ? 0 : int( RSUSB_ST_NOT_FOUND );
    }

    usb_transfer * mock_backend::alloc_transfer() { return new xfer; }

    void mock_backend::free_transfer( usb_transfer * t )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _inflight.erase( static_cast< xfer * >( t ) );
        delete static_cast< xfer * >( t );
    }

    void mock_backend::schedule( xfer * x, unsigned delay_ms, int xs, uint32_t n )
    {
        uint64_t gen = x->gen;
        if( delay_ms )
            after_ms( delay_ms, _q, [this, x, gen, xs, n] { complete( x, gen, xs, n ); } );
        else
            async( _q, [this, x, gen, xs, n] { complete( x, gen, xs, n ); } );
    }

    void mock_backend::fill_in( mdev & d, xfer & x, uint32_t & n )
    {
        n = x.length;
        int iface = d.iface_of( x.endpoint );
        if( d.hwm.pending && d.hwm.iface >= 0 && d.hwm.iface == iface && n >= 4 )
        {
            d.hwm.pending = false;
            uint8_t op4[4] = { d.hwm.opcode, 0, 0, 0 };
            std::copy( op4, op4 + 4, x.buffer );
            for( uint32_t i = 4; i < n; ++i )
                x.buffer[i] = uint8_t( i );
            return;
        }
        uint64_t seq = d.seq[x.endpoint]++;
        for( uint32_t i = 0; i < n; ++i )
            x.buffer[i] = i < 8 ? uint8_t( seq >> ( 8 * i ) ) : i == 8 ? x.endpoint : uint8_t( seq + i );
    }

    void mock_backend::complete( xfer * x, uint64_t gen, int xs, uint32_t n )
    {
        {
            std::lock_guard< std::mutex > lock( _mutex );
            if( ! _inflight.count( x ) || ! x->inflight || x->gen != gen )
                return;
            auto d = _devs.find( x->key );
            if( xs == RSUSB_XS_COMPLETED && x->kind != usb_transfer::control && ( x->endpoint & 0x80 ) && d != _devs.end() )
                fill_in( *d->second, *x, n );
            x->inflight = false;
            x->held = false;
            _inflight.erase( x );
        }
        x->done( xs, n );
    }

    void mock_backend::abort_all( uint64_t key, int xs, bool control, int ep )
    {
        for( xfer * t : _inflight )
            if( t->key == key && ( ep < -1 || ( control ? t->kind == usb_transfer::control : t->kind != usb_transfer::control && t->endpoint == ep ) ) )
            {
                t->gen = ++next_gen;
                t->held = false;
                schedule( t, 0, xs, 0 );
            }
    }

    int mock_backend::submit( usb_transfer * t )
    {
        auto x = static_cast< xfer * >( t );
        std::lock_guard< std::mutex > lock( _mutex );
        auto h = _handles.find( x->handle );
        if( h == _handles.end() )
            return RSUSB_ST_NO_DEVICE;
        uint64_t key = h->second.key;
        auto & d = *_devs[key];
        _last_timeout = x->timeout_ms;
        if( int r = injected( key, op::submit ) )
            return r;
        if( ! d.present )
            return RSUSB_ST_NO_DEVICE;
        if( x->inflight )
            return RSUSB_ST_BUSY;
        if( x->kind != usb_transfer::control )
        {
            int i = d.iface_of( x->endpoint );
            if( i < 0 || ! h->second.claimed.count( uint8_t( i ) ) )
                return RSUSB_ST_NOT_FOUND;
        }
        x->inflight = true;
        x->key = key;
        x->gen = ++next_gen;
        _inflight.insert( x );
        bool control = x->kind == usb_transfer::control;
        if( int xs = injected( key, control ? op::control : op::bulk ) )
        {
            schedule( x, 1, xs, 0 );
            return 0;
        }
        uint32_t n = 0;
        unsigned delay = 0;
        bool in;
        uint8_t hold_key;
        if( control )
        {
            const uint8_t * s = x->buffer;
            uint8_t rt = s[0], rq = s[1];
            uint16_t val = uint16_t( s[2] | s[3] << 8 ), idx = uint16_t( s[4] | s[5] << 8 ), wlen = uint16_t( s[6] | s[7] << 8 );
            uint8_t * data = x->buffer + 8;
            in = rt & 0x80;
            hold_key = 0;
            n = wlen;
            if( in )
            {
                bool done = false;
                for( auto & sc : d.script )
                    if( ! done && sc.rt == rt && sc.rq == rq && sc.val == val && sc.idx == idx )
                    {
                        n = uint32_t( std::min< size_t >( wlen, sc.data.size() ) );
                        std::copy( sc.data.begin(), sc.data.begin() + n, data );
                        done = true;
                    }
                if( ! done && d.hwm.pending && d.hwm.iface < 0 && d.hwm.sel == ( val >> 8 ) && d.hwm.unit == ( idx >> 8 ) && wlen >= 4 )
                {
                    d.hwm.pending = false;
                    uint8_t op4[4] = { d.hwm.opcode, 0, 0, 0 };
                    std::copy( op4, op4 + 4, data );
                    for( uint32_t i = 4; i < wlen; ++i )
                        data[i] = uint8_t( i );
                    done = true;
                }
                for( uint32_t i = 0; ! done && i < wlen; ++i )
                    data[i] = uint8_t( rq + val + i );
            }
            else
            {
                _log.push_back( "control out rt=" + std::to_string( rt ) + " rq=" + std::to_string( rq ) + " val=" + std::to_string( val )
                                + " idx=" + std::to_string( idx ) + " len=" + std::to_string( wlen ) );
                if( is_hwm_packet( data, wlen, true, val, idx ) )
                    d.hwm = { true, data[RSUSB_HWM_OPCODE_OFFSET], uint8_t( val >> 8 ), uint8_t( idx >> 8 ), -1 };
            }
        }
        else
        {
            in = x->endpoint & 0x80;
            hold_key = x->endpoint;
            n = x->length;
            if( ! in )
            {
                _log.push_back( "bulk out ep=" + std::to_string( x->endpoint ) + " len=" + std::to_string( x->length ) );
                if( is_hwm_packet( x->buffer, x->length, false, 0, 0 ) )
                    d.hwm = { true, x->buffer[RSUSB_HWM_OPCODE_OFFSET], 0, 0, d.iface_of( x->endpoint ) };
            }
            else
            {
                auto p = d.period.find( x->endpoint );
                delay = p != d.period.end() ? p->second : x->kind == usb_transfer::bulk ? 33 : 5;
            }
        }
        x->pend_xs = RSUSB_XS_COMPLETED;
        x->pend_n = n;
        if( d.hold.count( hold_key ) )
        {
            x->held = true;
            if( x->timeout_ms )
                schedule( x, x->timeout_ms, RSUSB_XS_TIMED_OUT, 0 );
        }
        else if( x->timeout_ms && delay > x->timeout_ms )
            schedule( x, x->timeout_ms, RSUSB_XS_TIMED_OUT, 0 );
        else
            schedule( x, delay, RSUSB_XS_COMPLETED, n );
        return 0;
    }

    int mock_backend::cancel( usb_transfer * t )
    {
        auto x = static_cast< xfer * >( t );
        std::lock_guard< std::mutex > lock( _mutex );
        if( int r = injected( x->key, op::cancel ) )
            return r;
        if( ! x->inflight || ! _inflight.count( x ) )
            return RSUSB_ST_NOT_FOUND;
        abort_all( x->key, RSUSB_XS_CANCELLED, x->kind == usb_transfer::control, x->endpoint );
        return 0;
    }

    uint64_t mock_backend::registry_id_of( uint64_t key )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto it = _devs.find( key );
        return it == _devs.end() ? 0 : it->second->registry_id;
    }

    bool mock_backend::interfaces_held_elsewhere( uint64_t rid )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        for( auto & d : _devs )
            if( d.second->present && d.second->registry_id == rid )
                return ! d.second->held_elsewhere.empty();
        return false;
    }

    int mock_backend::hand_back( const handback_target & t, unsigned, unsigned )
    {
        std::unique_lock< std::mutex > lock( _mutex );
        uint64_t key = 0;
        for( auto & d : _devs )
            if( d.second->present && d.second->registry_id == t.registry_id )
                key = d.first;
        if( ! key )
            return RSUSB_ST_NOT_FOUND;
        if( int r = wait_wedge( lock, key, op::hand_back ) )
            return r;
        if( int r = injected( key, op::hand_back ) )
            return r;
        auto & d = *_devs[key];
        if( ! d.held_elsewhere.empty() )
            return RSUSB_ST_ACCESS;
        _counters.hand_backs.push_back( t.registry_id );
        d.present = false;
        abort_all( key, RSUSB_XS_NO_DEVICE, false, -2 );
        uint64_t fresh = add_device( d.fixture, true );
        async( _q, [this, key, fresh] {
            _ev.left( key );
            _ev.arrived( fresh );
        } );
        return 0;
    }

    void mock_backend::force_reenumerate( uint64_t rid )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        ++_counters.force_reenumerations;
        for( auto & e : _devs )
            if( e.second->present && e.second->registry_id == rid )
            {
                uint64_t key = e.first;
                e.second->present = false;
                abort_all( key, RSUSB_XS_NO_DEVICE, false, -2 );
                uint64_t fresh = add_device( e.second->fixture, true );
                async( _q, [this, key, fresh] {
                    _ev.left( key );
                    _ev.arrived( fresh );
                } );
                break;
            }
        _cv.notify_all();
    }

    std::vector< live_device > mock_backend::live_devices()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        std::vector< live_device > rv;
        for( auto & e : _devs )
            if( e.second->present )
            {
                auto & s = e.second->f.snap;
                live_device l;
                l.registry_id = e.second->registry_id;
                l.vid = s.vid();
                l.pid = s.pid();
                l.location = uint32_t( std::atoi( s.path.c_str() ) ) << 24;
                l.address = uint16_t( std::atoi( s.path.c_str() + s.path.find_last_of( '-' ) + 1 ) );
                l.drivers_attached = e.second->drivers;
                rv.push_back( l );
            }
        return rv;
    }

    void mock_backend::unplug( uint64_t tok )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto it = _devs.find( tok );
        if( it == _devs.end() || ! it->second->present )
            return;
        it->second->present = false;
        abort_all( tok, RSUSB_XS_NO_DEVICE, false, -2 );
        async( _q, [this, tok] { _ev.left( tok ); } );
        _cv.notify_all();
    }

    uint64_t mock_backend::plug( size_t fi )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        uint64_t key = add_device( fi, _fixtures[fi].drivers_attached );
        async( _q, [this, key] { _ev.arrived( key ); } );
        return key;
    }

    void mock_backend::fail_next( uint64_t tok, op o, int status, int count )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _fail[{ tok, o }] = { status, count };
    }

    void mock_backend::hold_completions( uint64_t tok, uint8_t ep, bool hold )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto & d = *_devs[tok];
        if( hold )
            d.hold.insert( ep );
        else
        {
            d.hold.erase( ep );
            for( xfer * t : _inflight )
                if( t->key == tok && t->held && ( ep == 0 ? t->kind == usb_transfer::control : t->kind != usb_transfer::control && t->endpoint == ep ) )
                {
                    t->gen = ++next_gen;
                    t->held = false;
                    schedule( t, 0, t->pend_xs, t->pend_n );
                }
        }
    }

    void mock_backend::wedge( uint64_t tok, op o )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _wedged.insert( { tok, o } );
    }

    void mock_backend::unwedge()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _wedged.clear();
        _cv.notify_all();
    }

    void mock_backend::set_held_elsewhere( uint64_t tok, uint8_t iface, bool held )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        if( held )
            _devs[tok]->held_elsewhere.insert( iface );
        else
            _devs[tok]->held_elsewhere.erase( iface );
    }

    void mock_backend::set_drivers_attached( uint64_t tok, bool attached )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _devs[tok]->drivers = attached;
    }

    void mock_backend::set_incomplete( uint64_t tok, bool incomplete )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _devs[tok]->incomplete = incomplete;
    }

    void mock_backend::set_stream_period( uint64_t tok, uint8_t ep, unsigned ms )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _devs[tok]->period[ep] = ms;
    }

    mock_backend::counters_t mock_backend::counters()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        counters_t c = _counters;
        for( auto & h : _handles )
        {
            ( h.second.anchor ? c.anchors : c.handles )++;
            c.claimed += unsigned( h.second.claimed.size() );
        }
        c.live_transfers = unsigned( _inflight.size() );
        return c;
    }

    std::vector< std::string > mock_backend::out_log()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        return _log;
    }

    uint32_t mock_backend::last_timeout()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        return _last_timeout;
    }

    uint64_t mock_backend::registry_id( uint64_t tok ) { return registry_id_of( tok ); }

    uint64_t mock_backend::last_plugged()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        return _last_plugged;
    }
}
