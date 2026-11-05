// License: Apache 2.0. See LICENSE file in root directory.
#include "policy-fake.h"
#include "daemon/core/xpc-util.h"

#include <unistd.h>

namespace usbd
{
    fake_policy::fake_policy() { _q = dispatch_queue_create( "fake.policy", DISPATCH_QUEUE_SERIAL ); }

    void fake_policy::set_next_peer( uid_t euid, bool asid_ok )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _next.euid = euid;
        _next.asid_ok = asid_ok;
        _next_set = true;
    }

    void fake_policy::set_reject_signature( bool reject )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _next.rejected = reject;
        _next_set = true;
    }

    void fake_policy::set_console( uid_t uid, bool notify )
    {
        std::function< void( change ) > h;
        {
            std::lock_guard< std::mutex > lock( _mutex );
            _console.uid = uid;
            ++_console.gen;
            h = _handler;
        }
        if( h && notify )
            async( _q, [h] { h( change::console ); } );
    }

    void fake_policy::set_camera_allowed( bool allowed, bool notify )
    {
        std::function< void( change ) > h;
        {
            std::lock_guard< std::mutex > lock( _mutex );
            _camera = allowed;
            h = _handler;
        }
        if( h && notify )
            async( _q, [h] { h( change::camera ); } );
    }

    bool fake_policy::prepare( xpc_connection_t peer )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _peers[peer] = _next;
        _peers[peer].rejected |= _reject_all;
        _next = { _console.uid, true, false };
        _next_set = false;
        return true;
    }

    fake_policy::peer_info fake_policy::info( xpc_connection_t peer )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto it = _peers.find( peer );
        if( it != _peers.end() )
            return it->second;
        return _next_set ? _next : peer_info{ _console.uid, true, false };
    }

    void fake_policy::set_reject_all_signatures( bool reject )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _reject_all = reject;
    }

    bool fake_policy::deliver( xpc_connection_t peer ) { return ! info( peer ).rejected; }

    uid_t fake_policy::peer_uid( xpc_connection_t peer )
    {
        if( peer )
            return info( peer ).euid;
        std::lock_guard< std::mutex > lock( _mutex );   // sessions without a peer (fuzz): the next-peer setting
        return _next_set ? _next.euid : _console.uid;
    }

    pid_t fake_policy::peer_pid( xpc_connection_t ) { return getpid(); }

    rsusb_auth fake_policy::hello( xpc_connection_t peer )
    {
        peer_info p = info( peer );
        std::lock_guard< std::mutex > lock( _mutex );
        if( p.euid != 0 && ( p.euid != _console.uid || _console.uid < 501 || ! p.asid_ok ) )
            return RSUSB_AUTH_USER;
        return _camera ? RSUSB_AUTH_OK : RSUSB_AUTH_CAMERA_DISABLED;
    }

    void fake_policy::set_next_peer_cdhash( std::string cdhash )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _next_cdhash = std::move( cdhash );
    }

    unsigned fake_policy::peer_cdhash_calls()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        return _cdhash_calls;
    }

    std::string fake_policy::peer_cdhash( xpc_object_t )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        ++_cdhash_calls;
        std::string h;
        h.swap( _next_cdhash );
        return h;
    }

    bool fake_policy::camera_allowed()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        return _camera;
    }

    console_state fake_policy::console()
    {
        std::lock_guard< std::mutex > lock( _mutex );
        return _console;
    }

    void fake_policy::forget( xpc_connection_t peer )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _peers.erase( peer );
    }

    void fake_policy::set_change_handler( std::function< void( change ) > handler )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        _handler = std::move( handler );
    }
}
