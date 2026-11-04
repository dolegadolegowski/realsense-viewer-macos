// License: Apache 2.0. See LICENSE file in root directory.
// Device snapshot as served by ENUMERATE (protocol header), its XPC and plist (fixture / --dump) forms.
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <xpc/xpc.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace usbd
{
    struct snap_alt
    {
        std::array< uint8_t, 9 > d{};
        std::vector< uint8_t > x;
        uint8_t number() const { return d[2]; }
        uint8_t cls() const { return d[5]; }
        uint8_t subclass() const { return d[6]; }
        uint8_t protocol() const { return d[7]; }
    };

    struct snap_interface
    {
        uint8_t cfg = 0;
        std::vector< snap_alt > alts;                    // 1..RSUSB_MAX_ALTSETTINGS
        std::vector< std::array< uint8_t, 7 > > eps;     // altsetting 0
        uint8_t number() const { return alts[0].number(); }
    };

    struct snapshot
    {
        std::string path;
        std::array< uint8_t, 18 > dd{};
        std::vector< snap_interface > ifs;
        bool refused_class = false;   // an interface (also one left out of `ifs`) has a refused class (2.4.1)
        uint16_t vid() const { return uint16_t( dd[8] | dd[9] << 8 ); }
        uint16_t pid() const { return uint16_t( dd[10] | dd[11] << 8 ); }
        uint8_t num_configs() const { return dd[17]; }
    };

    bool refused_interface_class( uint8_t cls, uint8_t subclass, uint8_t protocol );
    bool intel_pid_allowed( uint16_t pid );
    bool vid_pid_allowed( uint16_t vid, uint16_t pid );
    bool device_allowed( const snapshot & s );   // full 2.4.1 filter
    bool snapshot_within_limits( const snapshot & s );

    // Interfaces claimed by OPEN(if) on configuration `cfg` (2.5.3); empty if `if` is not in that configuration.
    std::vector< uint8_t > claim_set( const snapshot & s, int cfg, uint8_t iface );
    bool interface_in_any_config( const snapshot & s, unsigned iface );
    const snap_interface * find_interface( const snapshot & s, int cfg, uint8_t iface );

    xpc_object_t snapshot_to_xpc( const snapshot & s, uint64_t tok );   // ENUMERATE device dictionary
    bool snapshot_from_xpc( xpc_object_t dict, snapshot & s, uint64_t & tok );   // strict, header limits

    // Fixture / --dump form (DESIGN 9.0.2): ENUMERATE keys without tok plus RegistryID, DriversAttached, ActiveConfig.
    struct fixture_device
    {
        snapshot snap;
        uint64_t registry_id = 0;
        bool drivers_attached = true;
        int active_config = 0;
        CFDictionaryRef mock = nullptr;   // optional "Mock" dictionary, retained
    };
    CFMutableDictionaryRef snapshot_to_cf( const fixture_device & f );
    bool snapshot_from_cf( CFDictionaryRef dict, fixture_device & f, std::string & error );
    bool load_fixture_file( const std::string & path, std::vector< fixture_device > & out, std::string & error );
    std::string fixture_xml( const std::vector< fixture_device > & devices );
}
