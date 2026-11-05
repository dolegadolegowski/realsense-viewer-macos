// License: Apache 2.0. See LICENSE file in root directory.
#include "config.h"
#include "rs-usb-broker-protocol.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <set>

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

        bool parse_extra( CFDictionaryRef d, const std::string & expected_name, extra_list & l, std::string & e )
        {
            int64_t v;
            std::string s;
            if( ! as_int( get( d, "Version" ), v ) || v != 1 )
                return e = "Version", false;
            if( ! as_string( get( d, "Name" ), l.name ) || l.name != expected_name || ! valid_list_name( l.name ) )
                return e = "Name", false;
            if( ! as_string( get( d, "BuildID" ), s ) || ! lower_hex( s, 64 ) )
                return e = "BuildID", false;
            if( ! as_int( get( d, "BuildNumber" ), l.build_number ) || l.build_number < 0 || l.build_number > 999999999999999999 )
                return e = "BuildNumber", false;
            if( ! as_int( get( d, "ProtocolVersion" ), l.protocol ) || l.protocol < 1 || l.protocol > 2147483647 )
                return e = "ProtocolVersion", false;
            CFTypeRef a = get( d, "AllowedCDHashes" );
            if( ! a || CFGetTypeID( a ) != CFArrayGetTypeID() )
                return e = "AllowedCDHashes", false;
            auto arr = static_cast< CFArrayRef >( a );
            if( CFArrayGetCount( arr ) < 1 || CFArrayGetCount( arr ) > CFIndex( MAX_EXTRA_HASHES ) )
                return e = "AllowedCDHashes count", false;
            std::set< std::string > seen;
            for( CFIndex i = 0; i < CFArrayGetCount( arr ); ++i )
            {
                if( ! as_string( CFArrayGetValueAtIndex( arr, i ), s ) || ! lower_hex( s, 40 ) )
                    return e = "AllowedCDHashes entry", false;
                if( ! seen.insert( s ).second )
                    return e = "AllowedCDHashes duplicate", false;
                l.cdhashes.push_back( s );
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

    bool valid_list_name( const std::string & name )
    {
        if( name.size() < 3 || name.size() > 100 )
            return false;
        // labels of [a-z0-9-] that neither start nor end with '-', at least two of them
        size_t labels = 0;
        for( size_t start = 0; start <= name.size(); ++labels )
        {
            size_t end = name.find( '.', start );
            if( end == std::string::npos )
                end = name.size();
            if( end == start || name[start] == '-' || name[end - 1] == '-' )
                return false;
            for( size_t i = start; i < end; ++i )
                if( ! ( ( name[i] >= 'a' && name[i] <= 'z' ) || ( name[i] >= '0' && name[i] <= '9' ) || name[i] == '-' ) )
                    return false;
            start = end + 1;
        }
        return labels >= 2;
    }

    bool parse_extra_list( const uint8_t * data, size_t len, const std::string & expected_name, extra_list & out,
                           std::string & error )
    {
        CFDataRef cfd = CFDataCreate( nullptr, data, CFIndex( len ) );
        CFPropertyListRef pl = CFPropertyListCreateWithData( nullptr, cfd, kCFPropertyListImmutable, nullptr, nullptr );
        CFRelease( cfd );
        extra_list l;
        bool dict = pl && CFGetTypeID( pl ) == CFDictionaryGetTypeID();
        bool ok = dict && parse_extra( static_cast< CFDictionaryRef >( pl ), expected_name, l, error );
        if( ! dict )
            error = "not a plist dictionary";
        if( pl )
            CFRelease( pl );
        if( ok )
            out = l;
        return ok;
    }

    std::vector< extra_list > load_extra_lists( const std::string & dir, bool require_root, std::vector< std::string > & problems )
    {
        std::vector< extra_list > lists;
        int dfd = open( dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC );
        if( dfd < 0 )
        {
            if( errno != ENOENT )
                problems.push_back( "usbd.d: unsafe directory" );
            return lists;
        }
        struct stat st;
        if( fstat( dfd, &st ) != 0 || ! S_ISDIR( st.st_mode ) || ( require_root && st.st_uid != 0 ) || ( st.st_mode & 022 ) )
        {
            close( dfd );
            problems.push_back( "usbd.d: unsafe directory" );
            return lists;
        }
        std::vector< std::string > names;
        int dup_fd = dup( dfd );
        DIR * d = dup_fd < 0 ? nullptr : fdopendir( dup_fd );
        if( ! d )
        {
            if( dup_fd >= 0 )
                close( dup_fd );
            close( dfd );
            problems.push_back( "usbd.d: unreadable directory" );
            return lists;
        }
        const std::string suffix = ".plist";
        while( struct dirent * e = readdir( d ) )
        {
            std::string n = e->d_name;
            if( n.empty() || n[0] == '.' )   // ".", "..", and the list tool's temporary files
                continue;
            if( n.size() <= suffix.size() || n.compare( n.size() - suffix.size(), suffix.size(), suffix ) != 0
                || ! valid_list_name( n.substr( 0, n.size() - suffix.size() ) ) )
            {
                problems.push_back( "usbd.d/" + n + ": not <reverse-dns-name>.plist" );
                continue;
            }
            names.push_back( n );
        }
        closedir( d );
        std::sort( names.begin(), names.end() );
        for( size_t i = 0; i < names.size(); ++i )
        {
            const std::string & n = names[i];
            std::string where = "usbd.d/" + n + ": ";
            if( i >= MAX_EXTRA_LISTS )
            {
                problems.push_back( where + "more than 16 lists" );
                continue;
            }
            int fd = openat( dfd, n.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC );   // a FIFO must not block
            if( fd < 0 )
            {
                problems.push_back( where + ( errno == ELOOP ? "symlink" : "cannot open" ) );
                continue;
            }
            std::string why;
            if( fstat( fd, &st ) != 0 || ! S_ISREG( st.st_mode ) )
                why = "not a regular file";
            else if( ( require_root && st.st_uid != 0 ) || ( st.st_mode & 022 ) )
                why = "not owned by root or writable by others";
            else if( st.st_nlink != 1 )
                why = "hard-linked";
            else if( st.st_size > 65536 )
                why = "larger than 64 KiB";
            std::vector< uint8_t > buf;
            if( why.empty() )
            {
                buf.resize( size_t( st.st_size ) );
                size_t got = 0;
                while( got < buf.size() )
                {
                    ssize_t r = read( fd, buf.data() + got, buf.size() - got );
                    if( r <= 0 )
                        break;
                    got += size_t( r );
                }
                if( got != buf.size() )
                    why = "read failed";
            }
            close( fd );
            extra_list l;
            std::string error;
            if( why.empty() && ! parse_extra_list( buf.data(), buf.size(), n.substr( 0, n.size() - suffix.size() ), l, error ) )
                why = "invalid " + error;
            if( ! why.empty() )
            {
                problems.push_back( where + why );
                continue;
            }
            lists.push_back( std::move( l ) );
        }
        close( dfd );
        return lists;
    }

    void apply_extra_lists( config & c, const std::vector< extra_list > & lists, std::vector< std::string > & problems )
    {
        std::vector< extra_list > ordered = lists;
        std::stable_sort( ordered.begin(), ordered.end(), []( const extra_list & a, const extra_list & b ) { return a.name < b.name; } );
        std::vector< std::string > admitted = c.cdhashes;
        std::set< std::string > seen( admitted.begin(), admitted.end() );
        c.extra_lists.clear();
        for( auto & l : ordered )
        {
            std::vector< std::string > add;
            for( auto & h : l.cdhashes )
                if( ! seen.count( h ) && std::find( add.begin(), add.end(), h ) == add.end() )
                    add.push_back( h );
            if( admitted.size() + add.size() > MAX_ADMITTED_HASHES )
            {
                problems.push_back( "usbd.d/" + l.name + ".plist: more than 256 code hashes together with the lists before it" );
                continue;
            }
            admitted.insert( admitted.end(), add.begin(), add.end() );
            seen.insert( add.begin(), add.end() );
            c.extra_lists.push_back( l );
        }
        c.admitted_cdhashes = admitted;
    }

    std::string lists_admitting( const config & c, const std::string & cdhash )
    {
        std::string rv;
        if( std::find( c.cdhashes.begin(), c.cdhashes.end(), cdhash ) != c.cdhashes.end() )
            rv = "usbd.plist";
        for( auto & l : c.extra_lists )
            if( std::find( l.cdhashes.begin(), l.cdhashes.end(), cdhash ) != l.cdhashes.end() )
                rv += ( rv.empty() ? "" : ", " ) + l.name;
        return rv;
    }
}
