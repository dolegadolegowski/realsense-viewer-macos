// License: Apache 2.0. See LICENSE file in root directory.
#include "policy-macos.h"
#include "core/log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/AuthSession.h>
#include <Security/SecCode.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace usbd
{
    std::string cdhash_requirement( const std::vector< std::string > & cdhashes )
    {
        std::string r;
        for( auto & h : cdhashes )
            r += ( r.empty() ? "" : " or " ) + std::string( "cdhash H\"" ) + h + "\"";
        return r;
    }

    xpc_object_t cdhash_lightweight_requirement( const std::vector< std::string > & cdhashes )
    {
        xpc_object_t list = xpc_array_create( nullptr, 0 );
        for( auto & h : cdhashes )
        {
            uint8_t bytes[20];
            for( size_t i = 0; i < 20 && 2 * i + 1 < h.size(); ++i )
                bytes[i] = uint8_t( std::stoi( h.substr( 2 * i, 2 ), nullptr, 16 ) );
            xpc_array_set_data( list, XPC_ARRAY_APPEND, bytes, sizeof( bytes ) );
        }
        xpc_object_t in = xpc_dictionary_create( nullptr, nullptr, 0 );
        xpc_dictionary_set_value( in, "$in", list );
        xpc_object_t req = xpc_dictionary_create( nullptr, nullptr, 0 );
        xpc_dictionary_set_value( req, "cdhash", in );
        xpc_release( list );
        xpc_release( in );
        return req;
    }

    namespace
    {
        // -1: file absent, 0: deny (present value not exactly true, or unreadable), 1: allowed
        int read_flag( const std::string & path, bool mcx )
        {
            int fd = open( path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC );
            if( fd < 0 )
                return errno == ENOENT || errno == ENOTDIR ? -1 : 0;
            struct stat st;
            std::vector< uint8_t > buf;
            bool ok = fstat( fd, &st ) == 0 && S_ISREG( st.st_mode ) && st.st_size <= ( 1 << 20 );
            if( ok )
            {
                buf.resize( size_t( st.st_size ) );
                ok = read( fd, buf.data(), buf.size() ) == ssize_t( buf.size() );
            }
            close( fd );
            if( ! ok )
                return 0;
            CFDataRef d = CFDataCreate( nullptr, buf.data(), CFIndex( buf.size() ) );
            CFPropertyListRef pl = CFPropertyListCreateWithData( nullptr, d, kCFPropertyListImmutable, nullptr, nullptr );
            CFRelease( d );
            int rv = 0;
            if( pl && CFGetTypeID( pl ) == CFDictionaryGetTypeID() )
            {
                CFTypeRef v = pl;
                if( mcx )
                {
                    v = CFDictionaryGetValue( static_cast< CFDictionaryRef >( v ), CFSTR( "com.apple.applicationaccess" ) );
                    if( ! v )
                        rv = -1;
                    else if( CFGetTypeID( v ) != CFDictionaryGetTypeID() )
                        v = nullptr;
                }
                if( v && rv != -1 )
                {
                    v = CFDictionaryGetValue( static_cast< CFDictionaryRef >( v ), CFSTR( "allowCamera" ) );
                    if( ! v )
                        rv = -1;
                    else if( mcx )
                        v = CFGetTypeID( v ) == CFDictionaryGetTypeID() ? CFDictionaryGetValue( static_cast< CFDictionaryRef >( v ), CFSTR( "value" ) ) : nullptr;
                    if( rv != -1 )
                        rv = v && CFGetTypeID( v ) == CFBooleanGetTypeID() && CFBooleanGetValue( static_cast< CFBooleanRef >( v ) ) ? 1 : 0;
                }
            }
            if( pl )
                CFRelease( pl );
            return rv;
        }

        bool valid_user_name( const std::string & u )
        {
            return ! u.empty() && u != "." && u != ".." && u.find( '/' ) == std::string::npos;
        }
    }

    bool camera_allowed_in( const std::string & root, const std::string & user )
    {
        if( read_flag( root + "/com.apple.applicationaccess.plist", false ) == 0 )
            return false;
        if( ! valid_user_name( user ) )
            return true;
        return read_flag( root + "/" + user + "/com.apple.applicationaccess.plist", false ) != 0
            && read_flag( root + "/" + user + "/complete.plist", true ) != 0;
    }

    macos_policy::macos_policy( const macos_policy_options & o )
        : _o( o )
    {
        if( ! _o.queue )
            _o.queue = dispatch_get_main_queue();
        _requirement = cdhash_requirement( _o.cdhashes );
        if( _o.lightweight && ! _o.cdhashes.empty() )
            _lwcr = cdhash_lightweight_requirement( _o.cdhashes );
        read_console();
    }

    macos_policy::~macos_policy()
    {
        for( auto s : _watches )
        {
            dispatch_source_cancel( s );
            dispatch_release( s );
        }
        if( _store )
        {
            SCDynamicStoreSetDispatchQueue( _store, nullptr );
            CFRelease( _store );
        }
        if( _lwcr )
            xpc_release( _lwcr );
    }

    bool macos_policy::prepare( xpc_connection_t peer )
    {
        if( _o.cdhashes.empty() )
            return false;
        int r = _lwcr ? xpc_connection_set_peer_lightweight_code_requirement( peer, _lwcr )
                      : xpc_connection_set_peer_code_signing_requirement( peer, _requirement.c_str() );
        if( r )
            log_error( "policy", "setting the peer code-signing requirement failed (%d)", r );
        return r == 0;
    }

    uid_t macos_policy::peer_uid( xpc_connection_t peer ) { return peer ? xpc_connection_get_euid( peer ) : uid_t( -1 ); }
    pid_t macos_policy::peer_pid( xpc_connection_t peer ) { return peer ? xpc_connection_get_pid( peer ) : 0; }

    rsusb_auth macos_policy::hello( xpc_connection_t peer )
    {
        uid_t euid = xpc_connection_get_euid( peer );
        if( euid != 0 )
        {
            console_state cs = console();
            if( euid != cs.uid || cs.uid < 501 )
                return RSUSB_AUTH_USER;
            SecuritySessionId sid = 0;
            SessionAttributeBits attrs = 0;
            if( SessionGetInfo( SecuritySessionId( xpc_connection_get_asid( peer ) ), &sid, &attrs ) != errSecSuccess
                || ! ( attrs & sessionHasGraphicAccess ) || ( attrs & sessionIsRemote ) )
                return RSUSB_AUTH_USER;
        }
        return camera_allowed() ? RSUSB_AUTH_OK : RSUSB_AUTH_CAMERA_DISABLED;
    }

    std::string macos_policy::peer_cdhash( xpc_object_t message )
    {
        // The message's audit token identifies the sender (no pid reuse race)
        SecCodeRef code = nullptr;
        if( ! message || SecCodeCreateWithXPCMessage( message, kSecCSDefaultFlags, &code ) != errSecSuccess || ! code )
            return {};
        CFDictionaryRef info = nullptr;
        std::string rv;
        if( SecCodeCopySigningInformation( code, kSecCSDefaultFlags, &info ) == errSecSuccess && info )
        {
            CFTypeRef u = CFDictionaryGetValue( info, kSecCodeInfoUnique );
            if( u && CFGetTypeID( u ) == CFDataGetTypeID() && CFDataGetLength( static_cast< CFDataRef >( u ) ) == 20 )
            {
                static const char hex[] = "0123456789abcdef";
                const UInt8 * b = CFDataGetBytePtr( static_cast< CFDataRef >( u ) );
                for( int i = 0; i < 20; ++i )
                {
                    rv += hex[b[i] >> 4];
                    rv += hex[b[i] & 15];
                }
            }
            CFRelease( info );
        }
        CFRelease( code );
        return rv;
    }

    bool macos_policy::camera_allowed()
    {
        std::string user;
        {
            std::lock_guard< std::mutex > lock( _mutex );
            user = _user;
        }
        return camera_allowed_in( _o.managed_root, user );
    }

    console_state macos_policy::console()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        return _console;
    }

    void macos_policy::read_console()
    {
        uid_t uid = uid_t( -1 );
        gid_t gid;
        std::string name;
        if( CFStringRef n = SCDynamicStoreCopyConsoleUser( _store, &uid, &gid ) )
        {
            char buf[256];
            if( CFStringGetCString( n, buf, sizeof( buf ), kCFStringEncodingUTF8 ) )
                name = buf;
            CFRelease( n );
        }
        else
            uid = uid_t( -1 );
        if( _o.force_console_uid != uid_t( -1 ) )
            uid = _o.force_console_uid;
        if( ! valid_user_name( name ) )
            name.clear();
        std::lock_guard< std::mutex > lock( _mutex );
        if( uid != _console.uid || name != _user )
            ++_console.gen;
        _console.uid = uid;
        _user = name;
    }

    void macos_policy::store_changed( SCDynamicStoreRef, CFArrayRef, void * info )
    {
        auto self = static_cast< macos_policy * >( info );
        console_state before = self->console();
        self->read_console();
        self->arm_watches();
        console_state after = self->console();
        log_info( "policy", "console user changed: uid %{public}d -> %{public}d", int( before.uid ), int( after.uid ) );
        if( self->_handler )
        {
            self->_handler( change::console );
            self->_handler( change::camera );
        }
    }

    void macos_policy::arm_watches()
    {
        for( auto s : _watches )
        {
            dispatch_source_cancel( s );
            dispatch_release( s );
        }
        _watches.clear();
        std::string user;
        {
            std::lock_guard< std::mutex > lock( _mutex );
            user = _user;
        }
        std::vector< std::string > dirs = { _o.managed_root };
        if( ! user.empty() )
            dirs.push_back( _o.managed_root + "/" + user );
        if( access( _o.managed_root.c_str(), F_OK ) != 0 )
            dirs = { _o.managed_root.substr( 0, _o.managed_root.find_last_of( '/' ) ) };   // wait for its creation
        for( auto & dir : dirs )
        {
            int fd = open( dir.c_str(), O_EVTONLY | O_CLOEXEC );
            if( fd < 0 )
                continue;
            auto s = dispatch_source_create( DISPATCH_SOURCE_TYPE_VNODE, uintptr_t( fd ),
                                             DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_EXTEND
                                                 | DISPATCH_VNODE_ATTRIB | DISPATCH_VNODE_LINK | DISPATCH_VNODE_REVOKE,
                                             _o.queue );
            dispatch_set_context( s, this );
            dispatch_source_set_event_handler_f( s, []( void * c ) {
                auto self = static_cast< macos_policy * >( c );
                // re-arm on every event: covers delete/rename and newly created user directories
                dispatch_async_f( self->_o.queue, self, []( void * c2 ) {
                    auto me = static_cast< macos_policy * >( c2 );
                    me->arm_watches();
                    if( me->_handler )
                        me->_handler( change::camera );
                } );
            } );
            auto * pfd = new int( fd );
            dispatch_source_set_cancel_handler( s, ^{
                close( *pfd );
                delete pfd;
            } );
            dispatch_resume( s );
            _watches.push_back( s );
        }
    }

    void macos_policy::set_change_handler( std::function< void( change ) > handler )
    {
        _handler = std::move( handler );
        SCDynamicStoreContext ctx = { 0, this, nullptr, nullptr, nullptr };
        _store = SCDynamicStoreCreate( nullptr, CFSTR( "io.github.dolegadolegowski.realsense-usbd" ), store_changed, &ctx );
        if( _store )
        {
            CFStringRef key = SCDynamicStoreKeyCreateConsoleUser( nullptr );
            CFArrayRef keys = CFArrayCreate( nullptr, reinterpret_cast< const void ** >( &key ), 1, &kCFTypeArrayCallBacks );
            SCDynamicStoreSetNotificationKeys( _store, keys, nullptr );
            SCDynamicStoreSetDispatchQueue( _store, _o.queue );
            CFRelease( keys );
            CFRelease( key );
        }
        read_console();
        dispatch_async_f( _o.queue, this, []( void * c ) { static_cast< macos_policy * >( c )->arm_watches(); } );
    }
}
