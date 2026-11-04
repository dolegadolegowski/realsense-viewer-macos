// License: Apache 2.0. See LICENSE file in root directory.
#include "snapshot-libusb.h"
#include "rs-usb-broker-protocol.h"

#include <map>

namespace usbd
{
    std::string device_path( uint8_t bus, const uint8_t * ports, int port_count, uint8_t address )
    {
        // get_device_path() in src/libusb/enumerator-libusb.cpp
        std::string p = std::to_string( bus ) + "-";
        for( int i = 0; i < port_count && i < 8; ++i )
            p += std::to_string( ports[i] ) + ( i + 1 < port_count ? "." : "" );
        return p + "-" + std::to_string( address );
    }

    bool build_snapshot( const snapshot_input & in, snapshot & out, std::vector< std::string > * omitted )
    {
        const libusb_device_descriptor & d = *in.desc;
        out = snapshot();
        out.path = device_path( in.bus, in.ports, in.port_count, in.address );
        uint8_t dd[RSUSB_DEVICE_DESC_LEN] = { d.bLength, d.bDescriptorType, uint8_t( d.bcdUSB ), uint8_t( d.bcdUSB >> 8 ),
                                              d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol, d.bMaxPacketSize0,
                                              uint8_t( d.idVendor ), uint8_t( d.idVendor >> 8 ), uint8_t( d.idProduct ),
                                              uint8_t( d.idProduct >> 8 ), uint8_t( d.bcdDevice ), uint8_t( d.bcdDevice >> 8 ),
                                              d.iManufacturer, d.iProduct, d.iSerialNumber, d.bNumConfigurations };
        std::copy( dd, dd + RSUSB_DEVICE_DESC_LEN, out.dd.begin() );

        for( int c = 0; c < d.bNumConfigurations; ++c )
        {
            const libusb_config_descriptor * cfg = in.configs[c];
            if( ! cfg )
                return false;
            uint64_t bytes = 0;
            unsigned listed = 0;
            for( int i = 0; i < cfg->bNumInterfaces; ++i )
            {
                const libusb_interface & intf = cfg->interface[i];
                const libusb_interface_descriptor * alt0 = intf.num_altsetting > 0 ? intf.altsetting : nullptr;
                for( int a = 0; a < intf.num_altsetting; ++a )
                {
                    const libusb_interface_descriptor & s = intf.altsetting[a];
                    if( refused_interface_class( s.bInterfaceClass, s.bInterfaceSubClass, s.bInterfaceProtocol ) )
                        out.refused_class = true;
                }
                const char * why = nullptr;
                if( ! alt0 )
                    why = "no altsetting";
                else if( intf.num_altsetting > int( RSUSB_MAX_ALTSETTINGS ) )
                    why = "altsettings";
                else if( alt0->bNumEndpoints > RSUSB_MAX_ENDPOINTS || ( alt0->bNumEndpoints && ! alt0->endpoint ) )
                    why = "endpoints";
                else if( listed >= RSUSB_MAX_INTERFACES )
                    why = "interfaces";
                uint64_t size = 0;
                for( int a = 0; ! why && a < intf.num_altsetting; ++a )
                {
                    const libusb_interface_descriptor & s = intf.altsetting[a];
                    if( s.extra_length < 0 || unsigned( s.extra_length ) > RSUSB_MAX_EXTRA_LEN || ( s.extra_length && ! s.extra ) )
                        why = "extra";
                    size += RSUSB_INTERFACE_DESC_LEN + uint64_t( s.extra_length > 0 ? s.extra_length : 0 );
                }
                if( ! why && bytes + size > cfg->wTotalLength )
                    why = "wTotalLength";
                if( why )
                {
                    if( omitted )
                        omitted->push_back( "cfg " + std::to_string( c ) + " if " + std::to_string( i ) + ": " + why );
                    continue;
                }
                bytes += size;
                ++listed;
                snap_interface si;
                si.cfg = uint8_t( c );
                for( int a = 0; a < intf.num_altsetting; ++a )
                {
                    const libusb_interface_descriptor & s = intf.altsetting[a];
                    snap_alt sa;
                    sa.d = { s.bLength, s.bDescriptorType, s.bInterfaceNumber, s.bAlternateSetting, s.bNumEndpoints,
                             s.bInterfaceClass, s.bInterfaceSubClass, s.bInterfaceProtocol, s.iInterface };
                    if( s.extra_length > 0 )
                        sa.x.assign( s.extra, s.extra + s.extra_length );
                    si.alts.push_back( std::move( sa ) );
                }
                for( int e = 0; e < alt0->bNumEndpoints; ++e )
                {
                    const libusb_endpoint_descriptor & ep = alt0->endpoint[e];
                    si.eps.push_back( { ep.bLength, ep.bDescriptorType, ep.bEndpointAddress, ep.bmAttributes,
                                        uint8_t( ep.wMaxPacketSize ), uint8_t( ep.wMaxPacketSize >> 8 ), ep.bInterval } );
                }
                out.ifs.push_back( std::move( si ) );
            }
        }
        return true;
    }
}
