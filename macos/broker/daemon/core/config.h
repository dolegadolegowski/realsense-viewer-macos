// License: Apache 2.0. See LICENSE file in root directory.
// usbd.plist (DESIGN 5.2, 5.3).
#pragma once

#include <bitset>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace usbd
{
    struct config
    {
        std::string build_id;
        int64_t build_number = 0;
        std::vector< std::string > cdhashes;   // AllowedCDHashes
        std::string daemon_cdhash;
        unsigned grace_s = 5;                  // HandBackGraceSeconds
        unsigned idle_exit_s = 60;             // IdleExitSeconds
        std::bitset< 256 > denied_opcodes;     // DeniedHWMOpcodes
        std::vector< std::pair< uint64_t, uint64_t > > protected_ranges;   // {Start, Length}
    };

    const char * const CONFIG_PATH = "/Library/Application Support/RealSense Viewer/usbd.plist";

    bool parse_config( const uint8_t * data, size_t len, config & out, std::string & error );
    // 5.3: O_NOFOLLOW, regular, <= 64 KiB, not group/world-writable, owned by root (and the same for every parent
    // directory) unless `require_root` is false (tests)
    bool load_config( const std::string & path, bool require_root, config & out, std::string & error );
}
