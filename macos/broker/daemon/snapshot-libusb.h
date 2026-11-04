// License: Apache 2.0. See LICENSE file in root directory.
// Snapshot builder: a pure function of libusb's descriptor structs (no libusb calls; fuzzed in H5).
#pragma once

#include "core/snapshot.h"

#include <libusb.h>

namespace usbd
{
    struct snapshot_input
    {
        uint8_t bus = 0;
        const uint8_t * ports = nullptr;   // libusb_get_port_numbers
        int port_count = 0;
        uint8_t address = 0;
        const libusb_device_descriptor * desc = nullptr;
        const libusb_config_descriptor * const * configs = nullptr;   // bNumConfigurations entries, null = read failed
    };

    std::string device_path( uint8_t bus, const uint8_t * ports, int port_count, uint8_t address );

    // false: incomplete (a configuration could not be read); interfaces over a header limit are left out
    bool build_snapshot( const snapshot_input & in, snapshot & out, std::vector< std::string > * omitted = nullptr );
}
