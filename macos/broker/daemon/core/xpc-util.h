// License: Apache 2.0. See LICENSE file in root directory.
// Typed XPC getters (protocol header: never xpc_dictionary_get_uint64/_get_data alone) and small helpers.
#pragma once

#include <dispatch/dispatch.h>
#include <xpc/xpc.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace usbd
{
    inline bool get_u64( xpc_object_t d, const char * k, uint64_t & v )
    {
        xpc_object_t o = xpc_dictionary_get_value( d, k );
        if( ! o || xpc_get_type( o ) != XPC_TYPE_UINT64 )
            return false;
        v = xpc_uint64_get_value( o );
        return true;
    }

    inline bool get_i64( xpc_object_t d, const char * k, int64_t & v )
    {
        xpc_object_t o = xpc_dictionary_get_value( d, k );
        if( ! o || xpc_get_type( o ) != XPC_TYPE_INT64 )
            return false;
        v = xpc_int64_get_value( o );
        return true;
    }

    // 0: absent, 1: data, -1: present with another type
    inline int get_data( xpc_object_t d, const char * k, const uint8_t *& p, size_t & n )
    {
        xpc_object_t o = xpc_dictionary_get_value( d, k );
        if( ! o )
            return 0;
        if( xpc_get_type( o ) != XPC_TYPE_DATA )
            return -1;
        p = static_cast< const uint8_t * >( xpc_data_get_bytes_ptr( o ) );
        n = xpc_data_get_length( o );
        return 1;
    }

    inline xpc_object_t get_typed( xpc_object_t d, const char * k, xpc_type_t t )
    {
        xpc_object_t o = xpc_dictionary_get_value( d, k );
        return o && xpc_get_type( o ) == t ? o : nullptr;
    }

    // dispatch_async with a C++ callable (no block captures of C++ objects)
    inline void async( dispatch_queue_t q, std::function< void() > f )
    {
        auto * p = new std::function< void() >( std::move( f ) );
        dispatch_async_f( q, p, []( void * c ) {
            std::unique_ptr< std::function< void() > > fn( static_cast< std::function< void() > * >( c ) );
            ( *fn )();
        } );
    }

    inline void after_ms( uint64_t ms, dispatch_queue_t q, std::function< void() > f )
    {
        auto * p = new std::function< void() >( std::move( f ) );
        dispatch_after_f( dispatch_time( DISPATCH_TIME_NOW, int64_t( ms ) * NSEC_PER_MSEC ), q, p, []( void * c ) {
            std::unique_ptr< std::function< void() > > fn( static_cast< std::function< void() > * >( c ) );
            ( *fn )();
        } );
    }

    inline void sync( dispatch_queue_t q, std::function< void() > f )
    {
        dispatch_sync_f( q, &f, []( void * c ) { ( *static_cast< std::function< void() > * >( c ) )(); } );
    }

    uint64_t now_ms();
}
