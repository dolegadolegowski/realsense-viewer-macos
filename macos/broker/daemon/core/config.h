// License: Apache 2.0. See LICENSE file in root directory.
// usbd.plist (DESIGN 5.2, 5.3, 13) and what this build of the daemon is.
#pragma once

#include "../version.h"
#include "rs-usb-broker-protocol.h"

#include <bitset>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace usbd
{
    struct config
    {
        // From usbd.plist
        std::vector< std::string > cdhashes;   // AllowedCDHashes; empty: no restriction to programs (DESIGN 13)
        unsigned grace_s = 5;                  // HandBackGraceSeconds
        unsigned idle_exit_s = 60;             // IdleExitSeconds
        std::bitset< 256 > denied_opcodes;     // DeniedHWMOpcodes
        std::vector< std::pair< uint64_t, uint64_t > > protected_ranges;   // {Start, Length}

        // Not from usbd.plist: the versions this binary answers HELLO with. parse_config never takes them from the
        // file (its ServiceVersion, ProtocolMin and ProtocolMax are for installers and applications); tests set others.
        uint64_t service_version = RSUSBD_SERVICE_VERSION;
        uint64_t protocol_min = RSUSB_PROTOCOL_MIN_VERSION;   // every pv in [protocol_min, protocol_max] is served
        uint64_t protocol_max = RSUSB_PROTOCOL_VERSION;
    };

    // In the service's own folder (named like RSUSB_SERVICE_NAME), not in an application's
    const char * const CONFIG_PATH =
        "/Library/Application Support/io.github.dolegadolegowski.realsense-usbd/usbd.plist";
    const int64_t CONFIG_SCHEMA_VERSION = 2;   // usbd.plist `Version`; 1 was the allowlist of one build
    // A later schema keeps ServiceVersion and DaemonCDHash in this file under these names: the installers inside
    // applications that are already shipped read them from any schema >= 2 to decide what to keep (DESIGN 13.5.3)

    // `Version` must be CONFIG_SCHEMA_VERSION; AllowedCDHashes (0..256 strings of 40 lowercase hex) and the other
    // administrator keys are optional and checked for type and range; every other key is ignored
    bool parse_config( const uint8_t * data, size_t len, config & out, std::string & error );
    // 5.3: O_NOFOLLOW, regular, <= 64 KiB, not group/world-writable, owned by root (and the same for every parent
    // directory) unless `require_root` is false (tests)
    bool load_config( const std::string & path, bool require_root, config & out, std::string & error );
}
