// License: Apache 2.0. See LICENSE file in root directory.
// State directory, captured.plist and the startup sweep selection (DESIGN 2.7.6, 2.7.7).
#pragma once

#include "../backend.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace usbd
{
    const char * const STATE_DIR = "/var/run/io.github.dolegadolegowski.realsense-usbd";

    struct paths
    {
        std::string state_dir = STATE_DIR;
        bool require_root = true;   // tests use a temp dir owned by the tester
    };

    struct captured_entry
    {
        uint64_t registry_id = 0;
        uint16_t vid = 0, pid = 0;
        bool operator==( const captured_entry & o ) const { return registry_id == o.registry_id && vid == o.vid && pid == o.pid; }
    };

    class state_store
    {
    public:
        explicit state_store( const paths & p );
        bool prepare_dir();   // mkdir 0700, verify (lstat: dir, owner, mode), else remove and recreate once
        bool usable() const { return _usable; }
        bool read( std::vector< captured_entry > & out ) const;   // regular, owner, 0600, <= 64 KiB
        bool write( const std::vector< captured_entry > & entries );   // mkstemp + rename
        // In-memory list mirrored to the file
        void add( const captured_entry & e );
        void update( uint64_t old_registry_id, const captured_entry & e );
        void remove( uint64_t registry_id );
        std::vector< captured_entry > entries();
        void set_entries( const std::vector< captured_entry > & e );
        int lock( bool nonblocking );   // flock(LOCK_EX) on <dir>/lock: fd, or -1
        std::string file() const { return _dir + "/captured.plist"; }

    private:
        bool check_dir() const;
        std::string _dir;
        bool _require_root, _usable = false;
        std::mutex _mutex;
        std::vector< captured_entry > _entries;
    };

    // Entries whose live device has the recorded registry ID, an allowed VID/PID and no drivers attached.
    std::vector< captured_entry > sweep_select( const std::vector< captured_entry > & recorded,
                                                const std::vector< live_device > & live );
}
