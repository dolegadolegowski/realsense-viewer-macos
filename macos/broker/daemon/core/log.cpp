// License: Apache 2.0. See LICENSE file in root directory.
#include "log.h"
#include "xpc-util.h"

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <time.h>

namespace usbd
{
    uint64_t now_ms() { return clock_gettime_nsec_np( CLOCK_UPTIME_RAW ) / 1000000; }

    os_log_t logger( const char * category )
    {
        static os_log_t conn = os_log_create( "io.github.dolegadolegowski.realsense-usbd", "conn" );
        static os_log_t device = os_log_create( "io.github.dolegadolegowski.realsense-usbd", "device" );
        static os_log_t policy = os_log_create( "io.github.dolegadolegowski.realsense-usbd", "policy" );
        static os_log_t xfer = os_log_create( "io.github.dolegadolegowski.realsense-usbd", "xfer" );
        if( ! strcmp( category, "conn" ) )
            return conn;
        if( ! strcmp( category, "device" ) )
            return device;
        if( ! strcmp( category, "policy" ) )
            return policy;
        return xfer;
    }

    bool log_rate( pid_t pid, const char * reason, uint32_t & suppressed )
    {
        struct slot { uint64_t since = 0; uint32_t dropped = 0; };
        static std::mutex m;
        static std::map< std::pair< pid_t, std::string >, slot > slots;
        std::lock_guard< std::mutex > lock( m );
        if( slots.size() > 4096 )
            slots.clear();
        slot & s = slots[{ pid, reason }];
        uint64_t now = now_ms();
        if( s.since && now - s.since < 10000 )
        {
            ++s.dropped;
            return false;
        }
        suppressed = s.dropped;
        s.since = now;
        s.dropped = 0;
        return true;
    }
}
