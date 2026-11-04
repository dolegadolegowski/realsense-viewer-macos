// License: Apache 2.0. See LICENSE file in root directory.
#include "hwm.h"
#include "rs-usb-broker-protocol.h"

namespace usbd
{
    hwm_policy::hwm_policy( const config & c )
        : denied( c.denied_opcodes )
        , ranges( c.protected_ranges )
    {
        denied.set( RSUSB_HWM_OP_FEF );
        denied.set( RSUSB_HWM_OP_FSRU );
        denied.set( RSUSB_HWM_OP_FPLOCK );
    }

    bool is_hwm_packet( const uint8_t * p, size_t len, bool control, uint16_t value, uint16_t index )
    {
        if( len < RSUSB_HWM_MIN_LEN )
            return false;
        return ( p[2] == RSUSB_HWM_MAGIC_0 && p[3] == RSUSB_HWM_MAGIC_1 )
            || ( control && ( value >> 8 ) == RSUSB_HWM_XU_SELECTOR && ( index >> 8 ) == RSUSB_HWM_XU_UNIT );
    }

    static uint64_t u32( const uint8_t * p ) { return uint64_t( p[0] ) | uint64_t( p[1] ) << 8 | uint64_t( p[2] ) << 16 | uint64_t( p[3] ) << 24; }

    bool hwm_denied( const hwm_policy & pol, const uint8_t * p, size_t len, bool control, uint16_t value, uint16_t index,
                     uint8_t * opcode )
    {
        if( ! is_hwm_packet( p, len, control, value, index ) )
            return false;
        uint8_t op = p[RSUSB_HWM_OPCODE_OFFSET];
        if( opcode )
            *opcode = op;
        if( pol.denied.test( op ) )
            return true;
        if( pol.ranges.empty() || len < 16 || ( op != RSUSB_HWM_OP_FES && op != RSUSB_HWM_OP_FWB ) )
            return false;
        uint64_t p1 = u32( p + RSUSB_HWM_PARAM1_OFFSET ), p2 = u32( p + RSUSB_HWM_PARAM2_OFFSET );
        if( ! p2 )
            return false;
        uint64_t begin = p1, end = p1 + p2;
        if( op == RSUSB_HWM_OP_FES )
            begin *= RSUSB_FLASH_SECTOR_SIZE, end *= RSUSB_FLASH_SECTOR_SIZE;   // < 2^45, no overflow
        for( auto & r : pol.ranges )
            if( begin < r.first + r.second && r.first < end )
                return true;
        return false;
    }
}
