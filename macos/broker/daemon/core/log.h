// License: Apache 2.0. See LICENSE file in root directory.
// os_log subsystem io.github.dolegadolegowski.realsense-usbd, categories conn, device, policy, xfer (DESIGN 2.10).
#pragma once

#include <os/log.h>
#include <sys/types.h>

#include <cstdint>

namespace usbd
{
    os_log_t logger( const char * category );
    // One line per (pid, reason) per 10 s; returns false while suppressed, `suppressed` = lines dropped before this one
    bool log_rate( pid_t pid, const char * reason, uint32_t & suppressed );
}

#define log_info( cat, ... ) os_log( usbd::logger( cat ), __VA_ARGS__ )
#define log_error( cat, ... ) os_log_error( usbd::logger( cat ), __VA_ARGS__ )
#define log_fault( cat, ... ) os_log_fault( usbd::logger( cat ), __VA_ARGS__ )
#define log_debug( cat, ... ) os_log_debug( usbd::logger( cat ), __VA_ARGS__ )
