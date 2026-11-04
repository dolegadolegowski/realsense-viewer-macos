// License: Apache 2.0. See LICENSE file in root directory.
#include "snapshot.h"
#include "rs-usb-broker-protocol.h"
#include "rs-usbd-pids.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace usbd
{
    bool refused_interface_class( uint8_t cls, uint8_t subclass, uint8_t protocol )
    {
        switch( cls )
        {
        case 0x01: case 0x02: case 0x08: case 0x09: case 0x0A: case 0xE0:
            return true;
        case 0x03:
            return subclass == 1 && ( protocol == 1 || protocol == 2 );
        default:
            return false;
        }
    }

    bool intel_pid_allowed( uint16_t pid )
    {
        return std::binary_search( std::begin( RSUSBD_INTEL_PIDS ), std::end( RSUSBD_INTEL_PIDS ), pid );
    }

    bool vid_pid_allowed( uint16_t vid, uint16_t pid )
    {
        return vid == RSUSB_VID_REALSENSE || ( vid == RSUSB_VID_INTEL && intel_pid_allowed( pid ) );
    }

    bool device_allowed( const snapshot & s )
    {
        if( s.refused_class || ! vid_pid_allowed( s.vid(), s.pid() ) )
            return false;
        for( auto & i : s.ifs )
            for( auto & a : i.alts )
                if( refused_interface_class( a.cls(), a.subclass(), a.protocol() ) )
                    return false;
        return true;
    }

    bool snapshot_within_limits( const snapshot & s )
    {
        if( s.path.empty() || s.path.size() > RSUSB_MAX_PATH_LEN )
            return false;
        std::map< int, unsigned > per_cfg;
        for( auto & i : s.ifs )
        {
            if( ++per_cfg[i.cfg] > RSUSB_MAX_INTERFACES || i.alts.empty() || i.alts.size() > RSUSB_MAX_ALTSETTINGS
                || i.eps.size() > RSUSB_MAX_ENDPOINTS )
                return false;
            for( auto & a : i.alts )
                if( a.x.size() > RSUSB_MAX_EXTRA_LEN )
                    return false;
        }
        return true;
    }

    const snap_interface * find_interface( const snapshot & s, int cfg, uint8_t iface )
    {
        for( auto & i : s.ifs )
            if( i.cfg == cfg && i.number() == iface )
                return &i;
        return nullptr;
    }

    bool interface_in_any_config( const snapshot & s, unsigned iface )
    {
        for( auto & i : s.ifs )
            if( i.number() == iface )
                return true;
        return false;
    }

    std::vector< uint8_t > claim_set( const snapshot & s, int cfg, uint8_t iface )
    {
        // device-libusb.cpp: a VIDEO_CONTROL interface becomes current, later VIDEO_STREAMING ones belong to it
        std::vector< uint8_t > rv;
        const snap_interface * current = nullptr;
        bool found = false;
        for( auto & i : s.ifs )
        {
            if( i.cfg != cfg )
                continue;
            const snap_alt & a = i.alts[0];
            if( a.cls() == 0x0E && a.subclass() == 1 )
                current = &i;
            if( a.number() == iface && ! found )
            {
                found = true;
                rv.push_back( iface );
                if( current != &i )
                    break;
                continue;
            }
            if( found && a.cls() == 0x0E && a.subclass() == 2 && current && current->number() == iface )
                rv.push_back( a.number() );
        }
        return rv;
    }

    xpc_object_t snapshot_to_xpc( const snapshot & s, uint64_t tok )
    {
        xpc_object_t d = xpc_dictionary_create( nullptr, nullptr, 0 );
        xpc_dictionary_set_uint64( d, RSUSB_KEY_TOKEN, tok );
        xpc_dictionary_set_string( d, RSUSB_KEY_PATH, s.path.c_str() );
        xpc_dictionary_set_data( d, RSUSB_KEY_DEVICE_DESC, s.dd.data(), s.dd.size() );
        xpc_object_t ifs = xpc_array_create( nullptr, 0 );
        for( auto & i : s.ifs )
        {
            xpc_object_t xi = xpc_dictionary_create( nullptr, nullptr, 0 );
            xpc_dictionary_set_uint64( xi, RSUSB_KEY_CONFIG, i.cfg );
            xpc_object_t alts = xpc_array_create( nullptr, 0 );
            for( auto & a : i.alts )
            {
                xpc_object_t xa = xpc_dictionary_create( nullptr, nullptr, 0 );
                xpc_dictionary_set_data( xa, RSUSB_KEY_INTERFACE_DESC, a.d.data(), a.d.size() );
                xpc_dictionary_set_data( xa, RSUSB_KEY_EXTRA, a.x.data(), a.x.size() );
                xpc_array_append_value( alts, xa );
                xpc_release( xa );
            }
            xpc_dictionary_set_value( xi, RSUSB_KEY_ALTSETTINGS, alts );
            xpc_release( alts );
            std::vector< uint8_t > eps;
            for( auto & e : i.eps )
                eps.insert( eps.end(), e.begin(), e.end() );
            xpc_dictionary_set_data( xi, RSUSB_KEY_ENDPOINTS, eps.data(), eps.size() );
            xpc_array_append_value( ifs, xi );
            xpc_release( xi );
        }
        xpc_dictionary_set_value( d, RSUSB_KEY_INTERFACES, ifs );
        xpc_release( ifs );
        return d;
    }

    namespace
    {
        bool xdata( xpc_object_t d, const char * k, std::vector< uint8_t > & v )
        {
            xpc_object_t o = xpc_dictionary_get_value( d, k );
            if( ! o || xpc_get_type( o ) != XPC_TYPE_DATA )
                return false;
            auto p = static_cast< const uint8_t * >( xpc_data_get_bytes_ptr( o ) );
            v.assign( p, p + xpc_data_get_length( o ) );
            return true;
        }
    }

    bool snapshot_from_xpc( xpc_object_t d, snapshot & s, uint64_t & tok )
    {
        if( ! d || xpc_get_type( d ) != XPC_TYPE_DICTIONARY )
            return false;
        xpc_object_t o = xpc_dictionary_get_value( d, RSUSB_KEY_TOKEN );
        if( ! o || xpc_get_type( o ) != XPC_TYPE_UINT64 || ! ( tok = xpc_uint64_get_value( o ) ) )
            return false;
        o = xpc_dictionary_get_value( d, RSUSB_KEY_PATH );
        if( ! o || xpc_get_type( o ) != XPC_TYPE_STRING )
            return false;
        s.path.assign( xpc_string_get_string_ptr( o ), xpc_string_get_length( o ) );
        std::vector< uint8_t > v;
        if( ! xdata( d, RSUSB_KEY_DEVICE_DESC, v ) || v.size() != RSUSB_DEVICE_DESC_LEN )
            return false;
        std::copy( v.begin(), v.end(), s.dd.begin() );
        xpc_object_t ifs = xpc_dictionary_get_value( d, RSUSB_KEY_INTERFACES );
        if( ! ifs || xpc_get_type( ifs ) != XPC_TYPE_ARRAY )
            return false;
        s.ifs.clear();
        for( size_t n = 0; n < xpc_array_get_count( ifs ); ++n )
        {
            xpc_object_t xi = xpc_array_get_value( ifs, n );
            if( xpc_get_type( xi ) != XPC_TYPE_DICTIONARY )
                return false;
            snap_interface i;
            o = xpc_dictionary_get_value( xi, RSUSB_KEY_CONFIG );
            if( ! o || xpc_get_type( o ) != XPC_TYPE_UINT64 || xpc_uint64_get_value( o ) > 255 )
                return false;
            i.cfg = uint8_t( xpc_uint64_get_value( o ) );
            xpc_object_t alts = xpc_dictionary_get_value( xi, RSUSB_KEY_ALTSETTINGS );
            if( ! alts || xpc_get_type( alts ) != XPC_TYPE_ARRAY )
                return false;
            for( size_t m = 0; m < xpc_array_get_count( alts ); ++m )
            {
                xpc_object_t xa = xpc_array_get_value( alts, m );
                if( xpc_get_type( xa ) != XPC_TYPE_DICTIONARY )
                    return false;
                snap_alt a;
                if( ! xdata( xa, RSUSB_KEY_INTERFACE_DESC, v ) || v.size() != RSUSB_INTERFACE_DESC_LEN )
                    return false;
                std::copy( v.begin(), v.end(), a.d.begin() );
                if( ! xdata( xa, RSUSB_KEY_EXTRA, a.x ) )
                    return false;
                i.alts.push_back( std::move( a ) );
            }
            if( ! xdata( xi, RSUSB_KEY_ENDPOINTS, v ) || v.size() % RSUSB_ENDPOINT_WIRE_LEN )
                return false;
            for( size_t k = 0; k < v.size(); k += RSUSB_ENDPOINT_WIRE_LEN )
            {
                std::array< uint8_t, 7 > e;
                std::copy( v.begin() + k, v.begin() + k + 7, e.begin() );
                i.eps.push_back( e );
            }
            if( i.alts.empty() || i.alts.size() > RSUSB_MAX_ALTSETTINGS || i.eps.size() > RSUSB_MAX_ENDPOINTS )
                return false;
            s.ifs.push_back( std::move( i ) );
        }
        return snapshot_within_limits( s );
    }

    namespace
    {
        void cf_set( CFMutableDictionaryRef d, const char * k, CFTypeRef v )
        {
            CFStringRef key = CFStringCreateWithCString( nullptr, k, kCFStringEncodingUTF8 );
            CFDictionarySetValue( d, key, v );
            CFRelease( key );
            CFRelease( v );
        }
        CFDataRef cf_data( const uint8_t * p, size_t n ) { return CFDataCreate( nullptr, p, CFIndex( n ) ); }
        CFNumberRef cf_num( int64_t v ) { return CFNumberCreate( nullptr, kCFNumberSInt64Type, &v ); }

        CFTypeRef cf_get( CFDictionaryRef d, const char * k, CFTypeID type )
        {
            CFStringRef key = CFStringCreateWithCString( nullptr, k, kCFStringEncodingUTF8 );
            CFTypeRef v = CFDictionaryGetValue( d, key );
            CFRelease( key );
            return v && CFGetTypeID( v ) == type ? v : nullptr;
        }
        bool cf_int( CFDictionaryRef d, const char * k, int64_t & v )
        {
            auto n = static_cast< CFNumberRef >( cf_get( d, k, CFNumberGetTypeID() ) );
            return n && ! CFNumberIsFloatType( n ) && CFNumberGetValue( n, kCFNumberSInt64Type, &v );
        }
        bool cf_bytes( CFDictionaryRef d, const char * k, std::vector< uint8_t > & v )
        {
            auto o = static_cast< CFDataRef >( cf_get( d, k, CFDataGetTypeID() ) );
            if( ! o )
                return false;
            v.assign( CFDataGetBytePtr( o ), CFDataGetBytePtr( o ) + CFDataGetLength( o ) );
            return true;
        }
    }

    CFMutableDictionaryRef snapshot_to_cf( const fixture_device & f )
    {
        auto d = CFDictionaryCreateMutable( nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
        cf_set( d, RSUSB_KEY_PATH, CFStringCreateWithCString( nullptr, f.snap.path.c_str(), kCFStringEncodingUTF8 ) );
        cf_set( d, RSUSB_KEY_DEVICE_DESC, cf_data( f.snap.dd.data(), f.snap.dd.size() ) );
        auto ifs = CFArrayCreateMutable( nullptr, 0, &kCFTypeArrayCallBacks );
        for( auto & i : f.snap.ifs )
        {
            auto di = CFDictionaryCreateMutable( nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
            cf_set( di, RSUSB_KEY_CONFIG, cf_num( i.cfg ) );
            auto alts = CFArrayCreateMutable( nullptr, 0, &kCFTypeArrayCallBacks );
            for( auto & a : i.alts )
            {
                auto da = CFDictionaryCreateMutable( nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
                cf_set( da, RSUSB_KEY_INTERFACE_DESC, cf_data( a.d.data(), a.d.size() ) );
                cf_set( da, RSUSB_KEY_EXTRA, cf_data( a.x.data(), a.x.size() ) );
                CFArrayAppendValue( alts, da );
                CFRelease( da );
            }
            cf_set( di, RSUSB_KEY_ALTSETTINGS, alts );
            std::vector< uint8_t > eps;
            for( auto & e : i.eps )
                eps.insert( eps.end(), e.begin(), e.end() );
            cf_set( di, RSUSB_KEY_ENDPOINTS, cf_data( eps.data(), eps.size() ) );
            CFArrayAppendValue( ifs, di );
            CFRelease( di );
        }
        cf_set( d, RSUSB_KEY_INTERFACES, ifs );
        cf_set( d, "RegistryID", cf_num( int64_t( f.registry_id ) ) );
        cf_set( d, "DriversAttached", CFRetain( f.drivers_attached ? kCFBooleanTrue : kCFBooleanFalse ) );
        cf_set( d, "ActiveConfig", cf_num( f.active_config ) );
        return d;
    }

    bool snapshot_from_cf( CFDictionaryRef d, fixture_device & f, std::string & error )
    {
        auto path = static_cast< CFStringRef >( cf_get( d, RSUSB_KEY_PATH, CFStringGetTypeID() ) );
        char buf[256];
        if( ! path || ! CFStringGetCString( path, buf, sizeof( buf ), kCFStringEncodingUTF8 ) )
            return error = "path", false;
        f.snap.path = buf;
        std::vector< uint8_t > v;
        if( ! cf_bytes( d, RSUSB_KEY_DEVICE_DESC, v ) || v.size() != RSUSB_DEVICE_DESC_LEN )
            return error = "dd", false;
        std::copy( v.begin(), v.end(), f.snap.dd.begin() );
        auto ifs = static_cast< CFArrayRef >( cf_get( d, RSUSB_KEY_INTERFACES, CFArrayGetTypeID() ) );
        if( ! ifs )
            return error = "ifs", false;
        for( CFIndex n = 0; n < CFArrayGetCount( ifs ); ++n )
        {
            auto di = static_cast< CFDictionaryRef >( CFArrayGetValueAtIndex( ifs, n ) );
            int64_t cfg;
            if( CFGetTypeID( di ) != CFDictionaryGetTypeID() || ! cf_int( di, RSUSB_KEY_CONFIG, cfg ) || cfg < 0 || cfg > 255 )
                return error = "cfg", false;
            snap_interface i;
            i.cfg = uint8_t( cfg );
            auto alts = static_cast< CFArrayRef >( cf_get( di, RSUSB_KEY_ALTSETTINGS, CFArrayGetTypeID() ) );
            if( ! alts )
                return error = "alts", false;
            for( CFIndex m = 0; m < CFArrayGetCount( alts ); ++m )
            {
                auto da = static_cast< CFDictionaryRef >( CFArrayGetValueAtIndex( alts, m ) );
                snap_alt a;
                if( CFGetTypeID( da ) != CFDictionaryGetTypeID() || ! cf_bytes( da, RSUSB_KEY_INTERFACE_DESC, v )
                    || v.size() != RSUSB_INTERFACE_DESC_LEN || ! cf_bytes( da, RSUSB_KEY_EXTRA, a.x ) )
                    return error = "alt", false;
                std::copy( v.begin(), v.end(), a.d.begin() );
                i.alts.push_back( std::move( a ) );
            }
            if( ! cf_bytes( di, RSUSB_KEY_ENDPOINTS, v ) || v.size() % RSUSB_ENDPOINT_WIRE_LEN )
                return error = "eps", false;
            for( size_t k = 0; k < v.size(); k += RSUSB_ENDPOINT_WIRE_LEN )
            {
                std::array< uint8_t, 7 > e;
                std::copy( v.begin() + k, v.begin() + k + 7, e.begin() );
                i.eps.push_back( e );
            }
            if( i.alts.empty() )
                return error = "alts empty", false;
            f.snap.ifs.push_back( std::move( i ) );
        }
        int64_t x = 0;
        if( ! cf_int( d, "RegistryID", x ) )
            return error = "RegistryID", false;
        f.registry_id = uint64_t( x );
        auto b = static_cast< CFBooleanRef >( cf_get( d, "DriversAttached", CFBooleanGetTypeID() ) );
        f.drivers_attached = b ? CFBooleanGetValue( b ) : true;
        f.active_config = cf_int( d, "ActiveConfig", x ) ? int( x ) : 0;
        f.mock = static_cast< CFDictionaryRef >( cf_get( d, "Mock", CFDictionaryGetTypeID() ) );
        if( f.mock )
            CFRetain( f.mock );
        if( ! snapshot_within_limits( f.snap ) )
            return error = "limits", false;
        return true;
    }

    bool load_fixture_file( const std::string & path, std::vector< fixture_device > & out, std::string & error )
    {
        std::ifstream in( path, std::ios::binary );
        std::vector< uint8_t > bytes( ( std::istreambuf_iterator< char >( in ) ), std::istreambuf_iterator< char >() );
        if( bytes.empty() )
            return error = "cannot read " + path, false;
        CFDataRef data = CFDataCreate( nullptr, bytes.data(), CFIndex( bytes.size() ) );
        CFPropertyListRef pl = CFPropertyListCreateWithData( nullptr, data, kCFPropertyListImmutable, nullptr, nullptr );
        CFRelease( data );
        bool ok = false;
        if( pl && CFGetTypeID( pl ) == CFDictionaryGetTypeID() )
        {
            auto devs = static_cast< CFArrayRef >( cf_get( static_cast< CFDictionaryRef >( pl ), "Devices", CFArrayGetTypeID() ) );
            ok = devs != nullptr;
            for( CFIndex n = 0; ok && n < CFArrayGetCount( devs ); ++n )
            {
                fixture_device f;
                auto d = static_cast< CFDictionaryRef >( CFArrayGetValueAtIndex( devs, n ) );
                ok = CFGetTypeID( d ) == CFDictionaryGetTypeID() && snapshot_from_cf( d, f, error );
                if( ok )
                    out.push_back( std::move( f ) );
            }
        }
        else
            error = "not a plist dictionary";
        if( pl )
            CFRelease( pl );
        return ok;
    }

    std::string fixture_xml( const std::vector< fixture_device > & devices )
    {
        auto arr = CFArrayCreateMutable( nullptr, 0, &kCFTypeArrayCallBacks );
        for( auto & f : devices )
        {
            auto d = snapshot_to_cf( f );
            CFArrayAppendValue( arr, d );
            CFRelease( d );
        }
        auto root = CFDictionaryCreateMutable( nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
        cf_set( root, "Devices", arr );
        CFDataRef xml = CFPropertyListCreateData( nullptr, root, kCFPropertyListXMLFormat_v1_0, 0, nullptr );
        std::string rv( reinterpret_cast< const char * >( CFDataGetBytePtr( xml ) ), size_t( CFDataGetLength( xml ) ) );
        CFRelease( xml );
        CFRelease( root );
        return rv;
    }
}
