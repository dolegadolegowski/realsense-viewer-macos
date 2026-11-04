// License: Apache 2.0. See LICENSE file in root directory.
#include "state.h"
#include "log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace usbd
{
    state_store::state_store( const paths & p )
        : _dir( p.state_dir )
        , _require_root( p.require_root )
    {
    }

    bool state_store::check_dir() const
    {
        struct stat st;
        return lstat( _dir.c_str(), &st ) == 0 && S_ISDIR( st.st_mode ) && ( st.st_mode & 07777 ) == 0700
            && ( ! _require_root || st.st_uid == 0 ) && ( _require_root || st.st_uid == geteuid() );
    }

    static int rm_entry( const char * p, const struct stat *, int, struct FTW * ) { return remove( p ); }

    bool state_store::prepare_dir()
    {
        for( int attempt = 0; attempt < 2; ++attempt )
        {
            mkdir( _dir.c_str(), 0700 );
            if( check_dir() )
                return _usable = true;
            struct stat st;
            if( lstat( _dir.c_str(), &st ) == 0 )
            {
                if( S_ISDIR( st.st_mode ) )
                    nftw( _dir.c_str(), rm_entry, 8, FTW_DEPTH | FTW_PHYS );
                else
                    unlink( _dir.c_str() );
            }
        }
        log_fault( "device", "state directory %{public}s is unsafe; no state file", _dir.c_str() );
        return _usable = false;
    }

    bool state_store::read( std::vector< captured_entry > & out ) const
    {
        out.clear();
        int fd = open( file().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC );
        if( fd < 0 )
            return false;
        struct stat st;
        std::vector< uint8_t > buf;
        bool ok = fstat( fd, &st ) == 0 && S_ISREG( st.st_mode ) && ( st.st_mode & 07777 ) == 0600
            && ( _require_root ? st.st_uid == 0 : st.st_uid == geteuid() ) && st.st_size <= 65536;
        if( ok )
        {
            buf.resize( size_t( st.st_size ) );
            ok = ::read( fd, buf.data(), buf.size() ) == ssize_t( buf.size() );
        }
        close( fd );
        if( ! ok )
            return false;
        CFDataRef d = CFDataCreate( nullptr, buf.data(), CFIndex( buf.size() ) );
        CFPropertyListRef pl = CFPropertyListCreateWithData( nullptr, d, kCFPropertyListImmutable, nullptr, nullptr );
        CFRelease( d );
        ok = pl && CFGetTypeID( pl ) == CFArrayGetTypeID();
        for( CFIndex i = 0; ok && i < CFArrayGetCount( static_cast< CFArrayRef >( pl ) ); ++i )
        {
            auto e = static_cast< CFDictionaryRef >( CFArrayGetValueAtIndex( static_cast< CFArrayRef >( pl ), i ) );
            if( CFGetTypeID( e ) != CFDictionaryGetTypeID() )
                continue;
            auto r = static_cast< CFNumberRef >( CFDictionaryGetValue( e, CFSTR( "RegistryID" ) ) );
            auto v = static_cast< CFNumberRef >( CFDictionaryGetValue( e, CFSTR( "VID" ) ) );
            auto p = static_cast< CFNumberRef >( CFDictionaryGetValue( e, CFSTR( "PID" ) ) );
            int64_t ri, vi, pi;
            if( ! r || ! v || ! p || CFGetTypeID( r ) != CFNumberGetTypeID() || CFGetTypeID( v ) != CFNumberGetTypeID()
                || CFGetTypeID( p ) != CFNumberGetTypeID() || ! CFNumberGetValue( r, kCFNumberSInt64Type, &ri )
                || ! CFNumberGetValue( v, kCFNumberSInt64Type, &vi ) || ! CFNumberGetValue( p, kCFNumberSInt64Type, &pi )
                || vi < 0 || vi > 0xFFFF || pi < 0 || pi > 0xFFFF )
                continue;
            out.push_back( { uint64_t( ri ), uint16_t( vi ), uint16_t( pi ) } );
        }
        if( pl )
            CFRelease( pl );
        return ok;
    }

    bool state_store::write( const std::vector< captured_entry > & entries )
    {
        if( ! _usable )
            return false;
        auto arr = CFArrayCreateMutable( nullptr, 0, &kCFTypeArrayCallBacks );
        for( auto & e : entries )
        {
            int64_t vals[3] = { int64_t( e.registry_id ), e.vid, e.pid };
            const void * keys[3] = { CFSTR( "RegistryID" ), CFSTR( "VID" ), CFSTR( "PID" ) };
            const void * nums[3];
            for( int i = 0; i < 3; ++i )
                nums[i] = CFNumberCreate( nullptr, kCFNumberSInt64Type, &vals[i] );
            auto d = CFDictionaryCreate( nullptr, keys, nums, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
            for( auto n : nums )
                CFRelease( n );
            CFArrayAppendValue( arr, d );
            CFRelease( d );
        }
        CFDataRef xml = CFPropertyListCreateData( nullptr, arr, kCFPropertyListXMLFormat_v1_0, 0, nullptr );
        CFRelease( arr );
        std::string tmpl = _dir + "/.captured.XXXXXX";
        int fd = mkstemp( &tmpl[0] );
        bool ok = fd >= 0;
        if( ok )
        {
            ok = fchmod( fd, 0600 ) == 0
                && ::write( fd, CFDataGetBytePtr( xml ), size_t( CFDataGetLength( xml ) ) ) == CFDataGetLength( xml );
            close( fd );
            ok = ok && rename( tmpl.c_str(), file().c_str() ) == 0;
            if( ! ok )
                unlink( tmpl.c_str() );
        }
        CFRelease( xml );
        if( ! ok )
            log_error( "device", "cannot write the state file" );
        return ok;
    }

    void state_store::add( const captured_entry & e )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        if( std::find( _entries.begin(), _entries.end(), e ) == _entries.end() )
            _entries.push_back( e );
        write( _entries );
    }

    void state_store::update( uint64_t old_id, const captured_entry & e )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _entries.erase( std::remove_if( _entries.begin(), _entries.end(),
                                        [&]( const captured_entry & x ) { return x.registry_id == old_id || x.registry_id == e.registry_id; } ),
                        _entries.end() );
        _entries.push_back( e );
        write( _entries );
    }

    void state_store::remove( uint64_t id )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto n = _entries.size();
        _entries.erase( std::remove_if( _entries.begin(), _entries.end(), [&]( const captured_entry & x ) { return x.registry_id == id; } ),
                        _entries.end() );
        if( n != _entries.size() )
            write( _entries );
    }

    std::vector< captured_entry > state_store::entries()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        return _entries;
    }

    void state_store::set_entries( const std::vector< captured_entry > & e )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _entries = e;
        write( _entries );
    }

    int state_store::lock( bool nonblocking )
    {
        int fd = open( ( _dir + "/lock" ).c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600 );
        if( fd < 0 )
            return -1;
        if( flock( fd, LOCK_EX | ( nonblocking ? LOCK_NB : 0 ) ) != 0 )
        {
            close( fd );
            return -1;
        }
        return fd;
    }

    std::vector< captured_entry > sweep_select( const std::vector< captured_entry > & recorded, const std::vector< live_device > & live )
    {
        std::vector< captured_entry > rv;
        for( auto & e : recorded )
            for( auto & l : live )
                if( l.registry_id == e.registry_id && l.vid == e.vid && l.pid == e.pid && vid_pid_allowed( l.vid, l.pid )
                    && ! l.drivers_attached )
                    rv.push_back( e );
        return rv;
    }
}
