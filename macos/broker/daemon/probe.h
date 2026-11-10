// License: Apache 2.0. See LICENSE file in root directory.
// What `realsense-usbd --probe` reports for the answer to its HELLO (DESIGN 2.1.3): one line and the exit status,
// and when it sends the HELLO again. Applications and the installer scripts read both, so neither changes without a
// new service version.
#pragma once

#include "core/xpc-util.h"
#include "rs-usb-broker-protocol.h"

#include <xpc/xpc.h>

#include <cstdint>
#include <string>

namespace usbd
{
    // `reply`: what came back for a HELLO sent with protocol version `pv` (a dictionary, or an XPC error object when
    // nothing answered). Returns the exit status: 0 OK, 2 unreachable/rejected, 3 AUTH_USER, 4 AUTH_CAMERA_DISABLED,
    // 5 AUTH_VERSION, 6 AUTH_LIMIT. On 0 the line is "ok <service version>"; a daemon of the first generation sends
    // no service version and counts as 1.
    inline int probe_result( xpc_object_t reply, uint64_t pv, std::string & line )
    {
        if( xpc_get_type( reply ) != XPC_TYPE_DICTIONARY )
            return line = "unreachable: the camera service is not installed, switched off, or does not admit this "
                          "program", 2;
        int64_t st;
        uint64_t reply_pv, auth, sv = 1;
        if( ! get_i64( reply, RSUSB_KEY_STATUS, st ) || ! get_u64( reply, RSUSB_KEY_VERSION, reply_pv )
            || ! get_u64( reply, RSUSB_KEY_AUTH, auth ) || st != 0 )
            return line = "rejected: invalid reply", 2;
        get_u64( reply, RSUSB_KEY_SERVICE_VERSION, sv );
        switch( auth )
        {
        case RSUSB_AUTH_OK:
            if( reply_pv != pv )
                return line = "protocol version " + std::to_string( reply_pv ), 5;
            return line = "ok " + std::to_string( sv ), 0;
        case RSUSB_AUTH_USER: return line = "not the console user", 3;
        case RSUSB_AUTH_CAMERA_DISABLED: return line = "camera disabled by policy", 4;
        case RSUSB_AUTH_VERSION: return line = "protocol version " + std::to_string( reply_pv ), 5;
        case RSUSB_AUTH_LIMIT: return line = "too many clients", 6;
        default: return line = "rejected: auth " + std::to_string( auth ), 2;
        }
    }

    const int PROBE_ATTEMPTS = 4;   // HELLOs the probe sends at most, each on a new connection
    const int PROBE_AGAIN = -1;     // probe_attempt: send the next one

    // What the probe does with the outcome of its HELLO number `attempt` (counted from 0). `reply` is as for
    // probe_result, or nullptr when nothing came before the probe's deadline; `time_left` says that the deadline has
    // not passed. An interrupted connection is tried again (PROBE_AGAIN): the daemon may have been exiting. When the
    // last attempt is interrupted too (a daemon that cannot start, or one that cuts off a program its
    // administrator's list does not have), the probe ends like for every other reply: with the line and the exit
    // status of probe_result.
    inline int probe_attempt( xpc_object_t reply, uint64_t pv, int attempt, bool time_left, std::string & line )
    {
        if( ! reply )
            return line = "unreachable: no answer", 2;
        if( reply == XPC_ERROR_CONNECTION_INTERRUPTED && time_left && attempt + 1 < PROBE_ATTEMPTS )
            return PROBE_AGAIN;
        return probe_result( reply, pv, line );
    }
}
