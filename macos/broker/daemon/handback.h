// License: Apache 2.0. See LICENSE file in root directory.
// IOKit side of the backend: registry IDs, drivers-attached, held-elsewhere check, hand-back (port of
// tools/release/rs-macos-release.c).
#pragma once

#include "backend.h"

#include <cstdint>
#include <vector>

namespace usbd
{
    std::vector< live_device > iokit_live_devices();
    uint64_t iokit_find_registry_id( uint8_t bus, uint16_t address, uint16_t vid, uint16_t pid );   // 0: none
    int iokit_drivers_attached( uint64_t registry_id );   // 1, 0, or -1 when not found
    bool iokit_interfaces_held_elsewhere( uint64_t registry_id );
    // OpenSeize + ReEnumerate(0) + Close, then waits for drivers on the same location; libusb-style status
    int iokit_hand_back( const handback_target & t, unsigned poll_ms, unsigned wait_ms );
    int iokit_reenumerate( uint64_t registry_id );
}
