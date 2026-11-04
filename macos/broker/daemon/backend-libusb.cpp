// License: Apache 2.0. See LICENSE file in root directory.
#include "backend-libusb.h"
#include "core/log.h"
#include "handback.h"
#include "snapshot-libusb.h"

#include <pthread/qos.h>

#include <algorithm>

namespace usbd
{
    namespace
    {
        struct libusb_xfer : usb_transfer
        {
            libusb_transfer * t = nullptr;
        };

        void LIBUSB_CALL log_cb( libusb_context *, enum libusb_log_level, const char * str )
        {
            log_debug( "device", "libusb: %{public}s", str );
        }

        void LIBUSB_CALL transfer_cb( libusb_transfer * t )
        {
            auto x = static_cast< libusb_xfer * >( t->user_data );
            x->done( int( t->status ), uint32_t( std::max( 0, t->actual_length ) ) );
        }
    }

    libusb_backend::libusb_backend()
    {
        if( libusb_init( &_ctx ) != LIBUSB_SUCCESS )
            _ctx = nullptr;
        else
        {
            libusb_set_log_cb( _ctx, log_cb, LIBUSB_LOG_CB_CONTEXT );
            libusb_set_option( _ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING );
        }
    }

    libusb_backend::~libusb_backend()
    {
        stop();
        for( auto & h : _handles )
            libusb_close( h.second );
        for( auto & d : _devices )
            libusb_unref_device( d.second );
        if( _ctx )
            libusb_exit( _ctx );
    }

    int LIBUSB_CALL libusb_backend::hotplug_cb( libusb_context *, libusb_device * dev, libusb_hotplug_event event, void * p )
    {
        auto self = static_cast< libusb_backend * >( p );
        libusb_device_descriptor desc;
        if( libusb_get_device_descriptor( dev, &desc ) != LIBUSB_SUCCESS || ! self->_ev.wanted( desc.idVendor, desc.idProduct ) )
            return 0;
        uint64_t key = 0;
        {
            std::lock_guard< std::mutex > lock( self->_mutex );
            auto it = self->_keys.find( dev );
            if( it != self->_keys.end() )
                key = it->second;
            else if( event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED )
            {
                key = self->_next_key++;
                self->_keys[dev] = key;
                self->_devices[key] = libusb_ref_device( dev );
            }
        }
        if( key && event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED )
            self->_ev.arrived( key );
        else if( key )
            self->_ev.left( key );
        return 0;
    }

    void libusb_backend::start( const events & ev )
    {
        _ev = ev;
        if( ! _ctx || _started.exchange( true ) )
            return;
        _events = std::thread( [this] {
            pthread_set_qos_class_self_np( QOS_CLASS_USER_INTERACTIVE, 0 );
            while( ! _stop )
                libusb_handle_events_completed( _ctx, &_stop );
        } );
        int r = libusb_hotplug_register_callback( _ctx, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                                  LIBUSB_HOTPLUG_ENUMERATE, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
                                                  LIBUSB_HOTPLUG_MATCH_ANY, hotplug_cb, this, &_hotplug );
        if( r != LIBUSB_SUCCESS )
            log_error( "device", "libusb hotplug registration failed (%d)", r );
    }

    void libusb_backend::stop()
    {
        if( ! _started.exchange( false ) )
            return;
        if( _hotplug )
            libusb_hotplug_deregister_callback( _ctx, _hotplug );
        _hotplug = 0;
        _stop = 1;
        libusb_interrupt_event_handler( _ctx );
        if( _events.joinable() )
            _events.join();
    }

    std::vector< uint64_t > libusb_backend::list()
    {
        std::vector< uint64_t > rv;
        libusb_device ** devs = nullptr;
        ssize_t n = _ctx ? libusb_get_device_list( _ctx, &devs ) : 0;
        for( ssize_t i = 0; i < n; ++i )
        {
            libusb_device_descriptor desc;
            if( libusb_get_device_descriptor( devs[i], &desc ) != LIBUSB_SUCCESS || ! _ev.wanted || ! _ev.wanted( desc.idVendor, desc.idProduct ) )
                continue;
            std::lock_guard< std::mutex > lock( _mutex );
            auto it = _keys.find( devs[i] );
            if( it == _keys.end() )
            {
                it = _keys.emplace( devs[i], _next_key++ ).first;
                _devices[it->second] = libusb_ref_device( devs[i] );
            }
            rv.push_back( it->second );
        }
        if( n > 0 )
            libusb_free_device_list( devs, 1 );
        return rv;
    }

    libusb_device * libusb_backend::device( uint64_t key )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto it = _devices.find( key );
        return it == _devices.end() ? nullptr : it->second;
    }

    libusb_device_handle * libusb_backend::handle( uint64_t h )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto it = _handles.find( h );
        return it == _handles.end() ? nullptr : it->second;
    }

    bool libusb_backend::describe_device( libusb_device * dev, uint64_t key, device_description & out )
    {
        libusb_device_descriptor desc;
        if( libusb_get_device_descriptor( dev, &desc ) != LIBUSB_SUCCESS )
            return false;
        uint8_t ports[8] = {};
        int nports = libusb_get_port_numbers( dev, ports, 8 );
        std::vector< libusb_config_descriptor * > cfgs( desc.bNumConfigurations, nullptr );
        for( uint8_t c = 0; c < desc.bNumConfigurations; ++c )
            if( libusb_get_config_descriptor( dev, c, &cfgs[c] ) != LIBUSB_SUCCESS )
                cfgs[c] = nullptr;
        snapshot_input in;
        in.bus = libusb_get_bus_number( dev );
        in.ports = ports;
        in.port_count = std::max( 0, nports );
        in.address = libusb_get_device_address( dev );
        in.desc = &desc;
        in.configs = cfgs.data();
        std::vector< std::string > omitted;
        out.key = key;
        out.complete = build_snapshot( in, out.snap, &omitted );
        for( auto & o : omitted )
            log_info( "device", "%04x:%04x: interface left out of the snapshot (%{public}s)", desc.idVendor, desc.idProduct, o.c_str() );
        libusb_config_descriptor * active = nullptr;
        if( libusb_get_active_config_descriptor( dev, &active ) == LIBUSB_SUCCESS )
        {
            for( uint8_t c = 0; c < desc.bNumConfigurations; ++c )
                if( cfgs[c] && cfgs[c]->bConfigurationValue == active->bConfigurationValue )
                    out.active_config = c;
            libusb_free_config_descriptor( active );
        }
        out.registry_id = iokit_find_registry_id( in.bus, in.address, desc.idVendor, desc.idProduct );
        out.drivers_attached = iokit_drivers_attached( out.registry_id ) == 1;
        for( auto c : cfgs )
            if( c )
                libusb_free_config_descriptor( c );
        return true;
    }

    bool libusb_backend::describe( uint64_t key, device_description & out )
    {
        libusb_device * dev = device( key );
        return dev && describe_device( dev, key, out );
    }

    std::vector< device_description > libusb_backend::enumerate_once()
    {
        std::vector< device_description > rv;
        libusb_device ** devs = nullptr;
        ssize_t n = _ctx ? libusb_get_device_list( _ctx, &devs ) : 0;
        for( ssize_t i = 0; i < n; ++i )
        {
            device_description d;
            if( describe_device( devs[i], uint64_t( i + 1 ), d ) )
                rv.push_back( std::move( d ) );
        }
        if( n > 0 )
            libusb_free_device_list( devs, 1 );
        return rv;
    }

    void libusb_backend::forget( uint64_t key )
    {
        std::lock_guard< std::mutex > lock( _mutex );
        auto it = _devices.find( key );
        if( it == _devices.end() )
            return;
        _keys.erase( it->second );
        libusb_unref_device( it->second );
        _devices.erase( it );
    }

    int libusb_backend::open_handle( uint64_t key, uint64_t & h )
    {
        libusb_device * dev = device( key );
        if( ! dev )
            return LIBUSB_ERROR_NO_DEVICE;
        libusb_device_handle * lh = nullptr;
        int r = libusb_open( dev, &lh );
        if( r )
            return r;
        std::lock_guard< std::mutex > lock( _mutex );
        h = _next_handle++;
        _handles[h] = lh;
        return 0;
    }

    void libusb_backend::close_handle( uint64_t h )
    {
        libusb_device_handle * lh;
        {
            std::lock_guard< std::mutex > lock( _mutex );
            auto it = _handles.find( h );
            if( it == _handles.end() )
                return;
            lh = it->second;
            _handles.erase( it );
        }
        libusb_close( lh );
    }

    int libusb_backend::kernel_driver_active( uint64_t h, uint8_t iface )
    {
        auto lh = handle( h );
        return lh ? libusb_kernel_driver_active( lh, iface ) : LIBUSB_ERROR_NO_DEVICE;
    }

    int libusb_backend::capture( uint64_t h, uint8_t iface )
    {
        auto lh = handle( h );
        return lh ? libusb_detach_kernel_driver( lh, iface ) : LIBUSB_ERROR_NO_DEVICE;
    }

    int libusb_backend::active_config( uint64_t h, int & index )
    {
        auto lh = handle( h );
        if( ! lh )
            return LIBUSB_ERROR_NO_DEVICE;
        int value = 0;
        int r = libusb_get_configuration( lh, &value );
        if( r )
            return r;
        libusb_device * dev = libusb_get_device( lh );
        libusb_device_descriptor desc;
        libusb_get_device_descriptor( dev, &desc );
        index = 0;
        for( uint8_t c = 0; c < desc.bNumConfigurations; ++c )
        {
            libusb_config_descriptor * cfg = nullptr;
            if( libusb_get_config_descriptor( dev, c, &cfg ) == LIBUSB_SUCCESS )
            {
                if( cfg->bConfigurationValue == value )
                    index = c;
                libusb_free_config_descriptor( cfg );
            }
        }
        return 0;
    }

    int libusb_backend::claim( uint64_t h, uint8_t iface )
    {
        auto lh = handle( h );
        return lh ? libusb_claim_interface( lh, iface ) : LIBUSB_ERROR_NO_DEVICE;
    }

    int libusb_backend::release( uint64_t h, uint8_t iface )
    {
        auto lh = handle( h );
        return lh ? libusb_release_interface( lh, iface ) : LIBUSB_ERROR_NO_DEVICE;
    }

    int libusb_backend::clear_halt( uint64_t h, uint8_t ep )
    {
        auto lh = handle( h );
        return lh ? libusb_clear_halt( lh, ep ) : LIBUSB_ERROR_NO_DEVICE;
    }

    usb_transfer * libusb_backend::alloc_transfer()
    {
        auto x = new libusb_xfer;
        x->t = libusb_alloc_transfer( 0 );
        return x;
    }

    void libusb_backend::free_transfer( usb_transfer * t )
    {
        auto x = static_cast< libusb_xfer * >( t );
        libusb_free_transfer( x->t );
        delete x;
    }

    int libusb_backend::submit( usb_transfer * t )
    {
        auto x = static_cast< libusb_xfer * >( t );
        auto lh = handle( x->handle );
        if( ! lh || ! x->t )
            return LIBUSB_ERROR_NO_DEVICE;
        switch( x->kind )
        {
        case usb_transfer::control:
            libusb_fill_control_transfer( x->t, lh, x->buffer, transfer_cb, x, x->timeout_ms );
            break;
        case usb_transfer::bulk:
            libusb_fill_bulk_transfer( x->t, lh, x->endpoint, x->buffer, int( x->length ), transfer_cb, x, x->timeout_ms );
            break;
        case usb_transfer::interrupt:
            libusb_fill_interrupt_transfer( x->t, lh, x->endpoint, x->buffer, int( x->length ), transfer_cb, x, x->timeout_ms );
            break;
        }
        return libusb_submit_transfer( x->t );
    }

    int libusb_backend::cancel( usb_transfer * t )
    {
        return libusb_cancel_transfer( static_cast< libusb_xfer * >( t )->t );
    }

    uint64_t libusb_backend::registry_id_of( uint64_t key )
    {
        device_description d;
        libusb_device * dev = device( key );
        if( ! dev )
            return 0;
        libusb_device_descriptor desc;
        libusb_get_device_descriptor( dev, &desc );
        return iokit_find_registry_id( libusb_get_bus_number( dev ), libusb_get_device_address( dev ), desc.idVendor, desc.idProduct );
    }

    bool libusb_backend::interfaces_held_elsewhere( uint64_t id ) { return iokit_interfaces_held_elsewhere( id ); }
    int libusb_backend::hand_back( const handback_target & t, unsigned poll_ms, unsigned wait_ms ) { return iokit_hand_back( t, poll_ms, wait_ms ); }
    void libusb_backend::force_reenumerate( uint64_t id ) { iokit_reenumerate( id ); }
    std::vector< live_device > libusb_backend::live_devices() { return iokit_live_devices(); }
}
