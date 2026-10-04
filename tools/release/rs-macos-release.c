// License: Apache 2.0. See LICENSE file in root directory.
// Hands RealSense cameras back to macOS after librealsense captured them through libusb.
//
// librealsense (RSUSB backend) detaches the macOS UVC/HID drivers by capturing the whole USB device, which
// requires root. After the capturing process exits the device stays captured: macOS drivers are not
// re-attached and only root can open it until it is re-plugged. Re-enumerating the device releases the
// capture, re-attaches the system drivers and also resets a camera whose firmware stopped responding.
//
// Usage: sudo rs-macos-release [-w]     (-w: wait until the system drivers are attached again)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const SInt32 REALSENSE_VIDS[] = { 0x8086, 0x38E5 };  // Intel, RealSense Inc. (src/usb/usb-types.h)

static int is_realsense( io_service_t dev )
{
    int result = 0;
    CFNumberRef vid_ref = IORegistryEntryCreateCFProperty( dev, CFSTR( kUSBVendorID ), kCFAllocatorDefault, 0 );
    CFStringRef name_ref = IORegistryEntryCreateCFProperty( dev, CFSTR( kUSBProductString ), kCFAllocatorDefault, 0 );
    SInt32 vid = 0;
    char name[256] = "";
    if( vid_ref )
        CFNumberGetValue( vid_ref, kCFNumberSInt32Type, &vid );
    if( name_ref )
        CFStringGetCString( name_ref, name, sizeof( name ), kCFStringEncodingUTF8 );
    for( size_t i = 0; i < sizeof( REALSENSE_VIDS ) / sizeof( REALSENSE_VIDS[0] ); ++i )
        if( vid == REALSENSE_VIDS[i] && strstr( name, "RealSense" ) )
            result = 1;
    if( vid_ref )
        CFRelease( vid_ref );
    if( name_ref )
        CFRelease( name_ref );
    return result;
}

// An interface without a child entry in the registry has no driver attached (same test libusb uses)
static int drivers_attached( io_service_t dev )
{
    io_iterator_t it;
    int interfaces = 0, with_driver = 0;
    if( IORegistryEntryGetChildIterator( dev, kIOServicePlane, &it ) != KERN_SUCCESS )
        return 0;
    io_service_t child;
    while( ( child = IOIteratorNext( it ) ) )
    {
        if( IOObjectConformsTo( child, "IOUSBHostInterface" ) )
        {
            io_service_t grandchild = IO_OBJECT_NULL;
            interfaces++;
            if( IORegistryEntryGetChildEntry( child, kIOServicePlane, &grandchild ) == KERN_SUCCESS && grandchild )
            {
                with_driver++;
                IOObjectRelease( grandchild );
            }
        }
        IOObjectRelease( child );
    }
    IOObjectRelease( it );
    return interfaces > 0 && interfaces == with_driver;
}

static IOReturn reenumerate( io_service_t dev )
{
    IOCFPlugInInterface ** plugin = NULL;
    IOUSBDeviceInterface650 ** usb = NULL;
    SInt32 score;
    IOReturn kr = IOCreatePlugInInterfaceForService( dev, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score );
    if( kr != kIOReturnSuccess || ! plugin )
        return kr ? kr : kIOReturnError;
    kr = ( *plugin )->QueryInterface( plugin, CFUUIDGetUUIDBytes( kIOUSBDeviceInterfaceID650 ), (LPVOID *)&usb );
    ( *plugin )->Release( plugin );
    if( kr != kIOReturnSuccess || ! usb )
        return kr ? kr : kIOReturnError;
    kr = ( *usb )->USBDeviceOpenSeize( usb );
    if( kr == kIOReturnSuccess )
    {
        kr = ( *usb )->USBDeviceReEnumerate( usb, 0 );
        ( *usb )->USBDeviceClose( usb );
    }
    ( *usb )->Release( usb );
    return kr;
}

static int count_released( int wait )
{
    io_iterator_t it;
    int total = 0, ready = 0;
    if( IOServiceGetMatchingServices( kIOMainPortDefault, IOServiceMatching( "IOUSBHostDevice" ), &it ) != KERN_SUCCESS )
        return -1;
    io_service_t dev;
    while( ( dev = IOIteratorNext( it ) ) )
    {
        if( is_realsense( dev ) )
        {
            total++;
            ready += drivers_attached( dev );
        }
        IOObjectRelease( dev );
    }
    IOObjectRelease( it );
    return wait ? ( total > 0 && ready == total ) : total;
}

int main( int argc, char ** argv )
{
    int wait = argc > 1 && strcmp( argv[1], "-w" ) == 0;
    io_iterator_t it;
    int found = 0, failed = 0;

    if( IOServiceGetMatchingServices( kIOMainPortDefault, IOServiceMatching( "IOUSBHostDevice" ), &it ) != KERN_SUCCESS )
    {
        fprintf( stderr, "failed to enumerate USB devices\n" );
        return 2;
    }
    io_service_t dev;
    while( ( dev = IOIteratorNext( it ) ) )
    {
        if( is_realsense( dev ) )
        {
            found++;
            if( drivers_attached( dev ) )
                printf( "RealSense device already attached to macOS drivers\n" );
            else
            {
                IOReturn kr = reenumerate( dev );
                printf( "RealSense device re-enumerated: %s (0x%08x)\n", kr == kIOReturnSuccess ? "ok" : "failed", kr );
                failed += kr != kIOReturnSuccess;
            }
        }
        IOObjectRelease( dev );
    }
    IOObjectRelease( it );

    if( ! found )
        printf( "No RealSense device found\n" );
    else if( wait && ! failed )
    {
        for( int i = 0; i < 50 && count_released( 1 ) != 1; ++i )
            usleep( 100 * 1000 );
        printf( "macOS drivers %s\n", count_released( 1 ) == 1 ? "attached" : "not attached yet" );
    }
    return failed ? 1 : 0;
}
