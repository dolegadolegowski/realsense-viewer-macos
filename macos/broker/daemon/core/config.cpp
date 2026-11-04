// License: Apache 2.0. See LICENSE file in root directory.
#include "config.h"
#include "rs-usb-broker-protocol.h"

#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace usbd
{
    namespace
    {
        CFTypeRef get( CFDictionaryRef d, const char * k )
        {
            CFStringRef key = CFStringCreateWithCString( nullptr, k, kCFStringEncodingUTF8 );
            CFTypeRef v = CFDictionaryGetValue( d, key );
            CFRelease( key );
            return v;
        }
        bool as_int( CFTypeRef v, int64_t & out )
        {
            return v && CFGetTypeID( v ) == CFNumberGetTypeID() && ! CFNumberIsFloatType( static_cast< CFNumberRef >( v ) )
                && CFNumberGetValue( static_cast< CFNumberRef >( v ), kCFNumberSInt64Type, &out );
        }
        bool as_string( CFTypeRef v, std::string & out )
        {
            char buf[256];
            if( ! v || CFGetTypeID( v ) != CFStringGetTypeID()
                || ! CFStringGetCString( static_cast< CFStringRef >( v ), buf, sizeof( buf ), kCFStringEncodingUTF8 ) )
                return false;
            out = buf;
            return true;
        }
        bool lower_hex( const std::string & s, size_t n )
        {
            return s.size() == n && std::all_of( s.begin(), s.end(), []( char c ) { return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ); } );
        }
        bool parse( CFDictionaryRef d, config & c, std::string & e )
        {
            int64_t v;
            if( ! as_int( get( d, "Version" ), v ) || v != 1 )
                return e = "Version", false;
            if( ! as_string( get( d, "BuildID" ), c.build_id ) || ! lower_hex( c.build_id, 64 ) )
                return e = "BuildID", false;
            if( ! as_int( get( d, "BuildNumber" ), c.build_number ) || c.build_number < 0 )
                return e = "BuildNumber", false;
            if( ! as_int( get( d, "ProtocolVersion" ), v ) || v != RSUSB_PROTOCOL_VERSION )
                return e = "ProtocolVersion", false;
            CFTypeRef a = get( d, "AllowedCDHashes" );
            if( ! a || CFGetTypeID( a ) != CFArrayGetTypeID() )
                return e = "AllowedCDHashes", false;
            auto arr = static_cast< CFArrayRef >( a );
            if( CFArrayGetCount( arr ) < 1 || CFArrayGetCount( arr ) > 256 )
                return e = "AllowedCDHashes count", false;
            for( CFIndex i = 0; i < CFArrayGetCount( arr ); ++i )
            {
                std::string h;
                if( ! as_string( CFArrayGetValueAtIndex( arr, i ), h ) || ! lower_hex( h, 40 ) )
                    return e = "AllowedCDHashes entry", false;
                c.cdhashes.push_back( h );
            }
            if( ! as_string( get( d, "DaemonCDHash" ), c.daemon_cdhash )
                || std::find( c.cdhashes.begin(), c.cdhashes.end(), c.daemon_cdhash ) == c.cdhashes.end() )
                return e = "DaemonCDHash", false;
            if( CFTypeRef g = get( d, "HandBackGraceSeconds" ) )
            {
                if( ! as_int( g, v ) || v < 0 || v > 600 )
                    return e = "HandBackGraceSeconds", false;
                c.grace_s = unsigned( v );
            }
            if( CFTypeRef g = get( d, "IdleExitSeconds" ) )
            {
                if( ! as_int( g, v ) || v < 10 || v > 3600 )
                    return e = "IdleExitSeconds", false;
                c.idle_exit_s = unsigned( v );
            }
            if( CFTypeRef g = get( d, "DeniedHWMOpcodes" ) )
            {
                if( CFGetTypeID( g ) != CFArrayGetTypeID() )
                    return e = "DeniedHWMOpcodes", false;
                for( CFIndex i = 0; i < CFArrayGetCount( static_cast< CFArrayRef >( g ) ); ++i )
                {
                    if( ! as_int( CFArrayGetValueAtIndex( static_cast< CFArrayRef >( g ), i ), v ) || v < 0 || v > 255 )
                        return e = "DeniedHWMOpcodes entry", false;
                    c.denied_opcodes.set( size_t( v ) );
                }
            }
            if( CFTypeRef g = get( d, "ProtectedFlashRanges" ) )
            {
                if( CFGetTypeID( g ) != CFArrayGetTypeID() )
                    return e = "ProtectedFlashRanges", false;
                for( CFIndex i = 0; i < CFArrayGetCount( static_cast< CFArrayRef >( g ) ); ++i )
                {
                    CFTypeRef r = CFArrayGetValueAtIndex( static_cast< CFArrayRef >( g ), i );
                    int64_t start, length;
                    if( CFGetTypeID( r ) != CFDictionaryGetTypeID()
                        || ! as_int( get( static_cast< CFDictionaryRef >( r ), "Start" ), start )
                        || ! as_int( get( static_cast< CFDictionaryRef >( r ), "Length" ), length ) || start < 0
                        || length <= 0 || uint64_t( start ) + uint64_t( length ) > ( 1ull << 32 ) )
                        return e = "ProtectedFlashRanges entry", false;
                    c.protected_ranges.emplace_back( uint64_t( start ), uint64_t( length ) );
                }
            }
            return true;
        }

        bool safe_owner( const struct stat & st, bool require_root )
        {
            return ( ! require_root || st.st_uid == 0 ) && ! ( st.st_mode & ( S_IWGRP | S_IWOTH ) );
        }
    }

    bool parse_config( const uint8_t * data, size_t len, config & out, std::string & error )
    {
        CFDataRef cfd = CFDataCreate( nullptr, data, CFIndex( len ) );
        CFPropertyListRef pl = CFPropertyListCreateWithData( nullptr, cfd, kCFPropertyListImmutable, nullptr, nullptr );
        CFRelease( cfd );
        config c;
        bool ok = pl && CFGetTypeID( pl ) == CFDictionaryGetTypeID() && parse( static_cast< CFDictionaryRef >( pl ), c, error );
        if( ! pl || CFGetTypeID( pl ) != CFDictionaryGetTypeID() )
            error = "not a plist dictionary";
        if( pl )
            CFRelease( pl );
        if( ok )
            out = c;
        return ok;
    }

    bool load_config( const std::string & path, bool require_root, config & out, std::string & error )
    {
        int fd = open( path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC );
        if( fd < 0 )
            return error = "cannot open " + path, false;
        struct stat st;
        std::vector< uint8_t > buf;
        bool ok = fstat( fd, &st ) == 0 && S_ISREG( st.st_mode ) && safe_owner( st, require_root ) && st.st_size <= 65536;
        if( ! ok )
            error = "unsafe or oversized file";
        if( ok )
        {
            buf.resize( size_t( st.st_size ) );
            ok = read( fd, buf.data(), buf.size() ) == ssize_t( buf.size() );
            if( ! ok )
                error = "read failed";
        }
        close( fd );
        // every parent directory: root-owned (if required), not group/world-writable, not a symlink
        for( std::string dir = path; ok; )
        {
            auto slash = dir.find_last_of( '/' );
            if( slash == std::string::npos )
                break;
            dir = slash ? dir.substr( 0, slash ) : "/";
            struct stat ds;
            if( lstat( dir.c_str(), &ds ) != 0 || ! S_ISDIR( ds.st_mode )
                || ! ( safe_owner( ds, require_root ) || ( ! require_root && ds.st_mode & S_ISVTX ) ) )
            {
                ok = false;
                error = "unsafe directory " + dir;
            }
            if( dir == "/" )
                break;
        }
        return ok && parse_config( buf.data(), buf.size(), out, error );
    }
}
