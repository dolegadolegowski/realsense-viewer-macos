// License: Apache 2.0. See LICENSE file in root directory.
// Prints what librealsense's RSUSB layer sees: every usb_enumerator::query_devices_info() entry, then per device the
// interfaces, endpoints and descriptors of create_usb_device(). Opens nothing, so it runs as any user, through the
// USB broker when one answers and through in-process libusb otherwise; the two outputs must match (DESIGN.md 9.0.6).
// Uses librealsense internals (exported by librealsense2.dylib): build with -I<librealsense>/src and the libusb headers.

#include "usb/usb-enumerator.h"
#include "libusb/interface-libusb.h"

#include <cstdio>
#include <set>
#include <string>

using namespace librealsense::platform;

int main( int argc, char * argv[] )
{
    if( argc > 1 )
    {
        fprintf( stderr, "usage: %s\n", argv[0] );
        return 2;
    }
    auto infos = usb_enumerator::query_devices_info();
    for( auto & i : infos )
        printf( "info id=%s vid=%04x pid=%04x mi=%u cls=%u uid=%s spec=%04x\n", i.id.c_str(), i.vid, i.pid, i.mi,
                unsigned( i.cls ), i.unique_id.c_str(), unsigned( i.conn_spec ) );

    std::set< std::string > seen;
    for( auto & i : infos )
    {
        if( ! seen.insert( i.id ).second )
            continue;
        auto dev = usb_enumerator::create_usb_device( i );
        if( ! dev )
        {
            printf( "device %s unavailable\n", i.id.c_str() );
            continue;
        }
        printf( "device %s\n", i.id.c_str() );
        for( auto & intf : dev->get_interfaces() )
        {
            std::string assoc;
            if( auto li = std::dynamic_pointer_cast< usb_interface_libusb >( intf ) )
                for( auto & a : li->get_associated_interfaces() )
                    assoc += ( assoc.empty() ? "" : "," ) + std::to_string( a->get_number() );
            printf( "  intf %u cls=%u sub=%u assoc=[%s]\n", intf->get_number(), intf->get_class(), intf->get_subclass(),
                    assoc.c_str() );
            for( auto & ep : intf->get_endpoints() )
                printf( "    ep %02x type=%u dir=%02x intf=%u\n", ep->get_address(), unsigned( ep->get_type() ),
                        unsigned( ep->get_direction() ), ep->get_interface_number() );
        }
        for( auto & d : dev->get_descriptors() )
        {
            printf( "  desc type=%02x len=%u ", d.type, d.length );
            for( auto b : d.data )
                printf( "%02x", b );
            printf( "\n" );
        }
    }
    return 0;
}
