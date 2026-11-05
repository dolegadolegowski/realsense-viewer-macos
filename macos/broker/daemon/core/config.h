// License: Apache 2.0. See LICENSE file in root directory.
// usbd.plist (DESIGN 5.2, 5.3) and the lists of other projects in usbd.d (5.4, 5.5).
#pragma once

#include <bitset>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace usbd
{
    // usbd.d/<name>.plist (5.4)
    struct extra_list
    {
        std::string name;
        int64_t build_number = 0;
        int64_t protocol = 0;                  // kept even if not RSUSB_PROTOCOL_VERSION: its programs get AUTH_VERSION
        std::vector< std::string > cdhashes;   // AllowedCDHashes, file order
    };

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
        std::vector< extra_list > extra_lists;          // admitted extra lists, name order (set by apply_extra_lists)
        std::vector< std::string > admitted_cdhashes;   // own AllowedCDHashes first, then admitted extras (apply_extra_lists)
    };

    const char * const CONFIG_PATH = "/Library/Application Support/RealSense Viewer/usbd.plist";
    const char * const EXTRA_DIR = "/Library/Application Support/RealSense Viewer/usbd.d";
    constexpr size_t MAX_EXTRA_LISTS = 16, MAX_EXTRA_HASHES = 64, MAX_ADMITTED_HASHES = 256;

    bool parse_config( const uint8_t * data, size_t len, config & out, std::string & error );
    // 5.3: O_NOFOLLOW, regular, <= 64 KiB, not group/world-writable, owned by root (and the same for every parent
    // directory) unless `require_root` is false (tests)
    bool load_config( const std::string & path, bool require_root, config & out, std::string & error );

    // 5.4: reverse DNS in lower case with at least one dot, 3..100 characters
    bool valid_list_name( const std::string & name );
    bool parse_extra_list( const uint8_t * data, size_t len, const std::string & expected_name, extra_list & out,
                           std::string & error );
    // 5.5: the valid lists of `dir` in name order; every skipped file or an unsafe directory adds a line to `problems`.
    // An absent directory is no problem. `require_root` false (tests) skips only the owner checks.
    std::vector< extra_list > load_extra_lists( const std::string & dir, bool require_root, std::vector< std::string > & problems );
    // 5.5: own hashes first, then each list in name order unless it would exceed MAX_ADMITTED_HASHES (skipped whole)
    void apply_extra_lists( config & c, const std::vector< extra_list > & lists, std::vector< std::string > & problems );
    // "usbd.plist" and/or the names of the admitted lists holding `cdhash`, ", "-joined; "" if none
    std::string lists_admitting( const config & c, const std::string & cdhash );
}
