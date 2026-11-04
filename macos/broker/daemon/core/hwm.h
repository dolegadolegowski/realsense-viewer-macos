// License: Apache 2.0. See LICENSE file in root directory.
// HWM filter (DESIGN 3.5): true = deny.
#pragma once

#include "config.h"

#include <cstddef>
#include <cstdint>

namespace usbd
{
    struct hwm_policy
    {
        std::bitset< 256 > denied;
        std::vector< std::pair< uint64_t, uint64_t > > ranges;
        explicit hwm_policy( const config & c );
    };

    bool is_hwm_packet( const uint8_t * p, size_t len, bool control, uint16_t value, uint16_t index );
    bool hwm_denied( const hwm_policy & pol, const uint8_t * p, size_t len, bool control, uint16_t value, uint16_t index,
                     uint8_t * opcode = nullptr );
}
