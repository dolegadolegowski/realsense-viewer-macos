// License: Apache 2.0. See LICENSE file in root directory.
#include "handback.h"
#include "core/log.h"
#include "rs-usb-broker-protocol.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <unistd.h>

namespace usbd
{
    namespace
    {
        int64_t prop( io_service_t s, CFStringRef key )
        {
            int64_t v = -1;
            CFTypeRef r = IORegistryEntryCreateCFProperty( s, key, kCFAllocatorDefault, 0 );
            if( r && CFGetTypeID( r ) == CFNumberGetTypeID() )
                CFNumberGetValue( static_cast< CFNumberRef >( r ), kCFNumberSInt64Type, &v );
            if( r )
                CFRelease( r );
            return v;
        }

        // Same test libusb uses: an interface without a child entry has no driver
        bool drivers_attached( io_service_t dev )
        {
            io_iterator_t it;
            int interfaces = 0, with_driver = 0;
            if( IORegistryEntryGetChildIterator( dev, kIOServicePlane, &it ) != KERN_SUCCESS )
                return false;
            while( io_service_t child = IOIteratorNext( it ) )
            {
                if( IOObjectConformsTo( child, "IOUSBHostInterface" ) )
                {
                    io_service_t grandchild = IO_OBJECT_NULL;
                    ++interfaces;
                    if( IORegistryEntryGetChildEntry( child, kIOServicePlane, &grandchild ) == KERN_SUCCESS && grandchild )
                    {
                        ++with_driver;
                        IOObjectRelease( grandchild );
                    }
                }
                IOObjectRelease( child );
            }
            IOObjectRelease( it );
            return interfaces > 0 && interfaces == with_driver;
        }

        io_service_t by_registry_id( uint64_t id )
        {
            return id ? IOServiceGetMatchingService( kIOMainPortDefault, IORegistryEntryIDMatching( id ) ) : IO_OBJECT_NULL;
        }

        template< class F >
        void for_each_device( F && f )
        {
            io_iterator_t it;
            if( IOServiceGetMatchingServices( kIOMainPortDefault, IOServiceMatching( "IOUSBHostDevice" ), &it ) != KERN_SUCCESS )
                return;
            while( io_service_t dev = IOIteratorNext( it ) )
            {
                live_device l;
                IORegistryEntryGetRegistryEntryID( dev, &l.registry_id );
                l.vid = uint16_t( prop( dev, CFSTR( kUSBVendorID ) ) );
                l.pid = uint16_t( prop( dev, CFSTR( kUSBProductID ) ) );
                l.location = uint32_t( prop( dev, CFSTR( kUSBDevicePropertyLocationID ) ) );
                l.address = uint16_t( prop( dev, CFSTR( kUSBDevicePropertyAddress ) ) );
                l.drivers_attached = drivers_attached( dev );
                bool stop = f( dev, l );
                IOObjectRelease( dev );
                if( stop )
                    break;
            }
            IOObjectRelease( it );
        }

        int to_status( IOReturn kr )
        {
            switch( kr )
            {
            case kIOReturnSuccess: return 0;
            case kIOReturnExclusiveAccess: return RSUSB_ST_ACCESS;
            case kIOReturnNoDevice: case kIOReturnNotOpen: return RSUSB_ST_NO_DEVICE;
            case kIOReturnNotFound: return RSUSB_ST_NOT_FOUND;
            default: return RSUSB_ST_OTHER;
            }
        }

        template< class T >
        T ** plugin( io_service_t s, CFUUIDRef type, CFUUIDRef iid )
        {
            IOCFPlugInInterface ** p = nullptr;
            SInt32 score;
            T ** rv = nullptr;
            if( IOCreatePlugInInterfaceForService( s, type, kIOCFPlugInInterfaceID, &p, &score ) != kIOReturnSuccess || ! p )
                return nullptr;
            ( *p )->QueryInterface( p, CFUUIDGetUUIDBytes( iid ), reinterpret_cast< LPVOID * >( &rv ) );
            ( *p )->Release( p );
            return rv;
        }

        IOReturn reenumerate( io_service_t dev )
        {
            auto usb = plugin< IOUSBDeviceInterface650 >( dev, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID650 );
            if( ! usb )
                return kIOReturnError;
            IOReturn kr = ( *usb )->USBDeviceOpenSeize( usb );
            if( kr == kIOReturnSuccess )
            {
                kr = ( *usb )->USBDeviceReEnumerate( usb, 0 );
                ( *usb )->USBDeviceClose( usb );
            }
            ( *usb )->Release( usb );
            return kr;
        }
    }

    std::vector< live_device > iokit_live_devices()
    {
        std::vector< live_device > rv;
        for_each_device( [&]( io_service_t, const live_device & l ) {
            rv.push_back( l );
            return false;
        } );
        return rv;
    }

    uint64_t iokit_find_registry_id( uint8_t bus, uint16_t address, uint16_t vid, uint16_t pid )
    {
        uint64_t rv = 0;
        for_each_device( [&]( io_service_t, const live_device & l ) {
            if( ( l.location >> 24 ) == bus && l.address == address && l.vid == vid && l.pid == pid )
                rv = l.registry_id;
            return rv != 0;
        } );
        return rv;
    }

    int iokit_drivers_attached( uint64_t registry_id )
    {
        io_service_t s = by_registry_id( registry_id );
        if( ! s )
            return -1;
        int rv = drivers_attached( s );
        IOObjectRelease( s );
        return rv;
    }

    bool iokit_interfaces_held_elsewhere( uint64_t registry_id )
    {
        io_service_t dev = by_registry_id( registry_id );
        if( ! dev )
            return false;
        bool held = false;
        io_iterator_t it;
        if( IORegistryEntryGetChildIterator( dev, kIOServicePlane, &it ) == KERN_SUCCESS )
        {
            while( io_service_t child = IOIteratorNext( it ) )
            {
                if( ! held && IOObjectConformsTo( child, "IOUSBHostInterface" ) )
                    if( auto intf = plugin< IOUSBInterfaceInterface650 >( child, kIOUSBInterfaceUserClientTypeID, kIOUSBInterfaceInterfaceID650 ) )
                    {
                        IOReturn kr = ( *intf )->USBInterfaceOpen( intf );
                        if( kr == kIOReturnSuccess )
                            ( *intf )->USBInterfaceClose( intf );
                        held = kr == kIOReturnExclusiveAccess;
                        ( *intf )->Release( intf );
                    }
                IOObjectRelease( child );
            }
            IOObjectRelease( it );
        }
        IOObjectRelease( dev );
        return held;
    }

    int iokit_reenumerate( uint64_t registry_id )
    {
        io_service_t dev = by_registry_id( registry_id );
        if( ! dev )
            return RSUSB_ST_NOT_FOUND;
        IOReturn kr = reenumerate( dev );
        IOObjectRelease( dev );
        return to_status( kr );
    }

    int iokit_hand_back( const handback_target & t, unsigned poll_ms, unsigned wait_ms )
    {
        io_service_t dev = by_registry_id( t.registry_id );
        uint32_t location = 0;
        if( ! dev )   // fallback: the 2.5.4 match
            if( uint64_t id = iokit_find_registry_id( t.bus, t.address, t.vid, t.pid ) )
                dev = by_registry_id( id );
        if( ! dev )
            return RSUSB_ST_NOT_FOUND;
        location = uint32_t( prop( dev, CFSTR( kUSBDevicePropertyLocationID ) ) );
        IOReturn kr = reenumerate( dev );
        IOObjectRelease( dev );
        if( kr != kIOReturnSuccess )
            return to_status( kr );
        for( unsigned waited = 0; waited < wait_ms; waited += poll_ms )
        {
            usleep( poll_ms * 1000 );
            bool ready = false;
            for_each_device( [&]( io_service_t, const live_device & l ) {
                ready = l.location == location && l.vid == t.vid && l.pid == t.pid && l.registry_id != t.registry_id
                     && l.drivers_attached;
                return ready;
            } );
            if( ready )
                return 0;
        }
        log_info( "device", "device %04x:%04x re-enumerated, macOS drivers not attached yet", t.vid, t.pid );
        return 0;
    }
}
