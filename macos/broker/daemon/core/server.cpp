// License: Apache 2.0. See LICENSE file in root directory.
#include "server.h"
#include "hwm.h"
#include "log.h"
#include "xpc-util.h"

#include <libproc.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace usbd
{
    namespace
    {
        enum class dstate { listed, capturing, handing_back, gone };

        // A capture (first OPEN) is in progress on the device queue; the device stays visible and leasable meanwhile
        bool usable( dstate s ) { return s == dstate::listed || s == dstate::capturing; }

        struct connection;
        struct messenger;

        struct owner_entry
        {
            messenger * m = nullptr;
            uint64_t conn = 0;
            bool releasing = false;
        };

        struct device
        {
            uint64_t tok = 0;
            snapshot snap;
            xpc_object_t xdict = nullptr;
            dispatch_queue_t q = nullptr;
            std::atomic< bool > gone{ false };
            // registry mutex
            uint64_t registry_id = 0;
            dstate state = dstate::listed;
            uint64_t anchor = 0;
            bool captured = false, capture_on_claim = false, finalized = false, forced = false;
            int active_cfg = -1;
            std::map< uint8_t, owner_entry > owners;
            std::map< messenger *, std::vector< std::function< void() > > > parked;
            std::map< uint64_t, std::weak_ptr< connection > > leases;
            std::map< uint64_t, std::pair< std::weak_ptr< connection >, int > > handle_conns;
            int messengers = 0;
            bool grace_armed = false;
            uint64_t grace_gen = 0;
            ~device()
            {
                if( xdict )
                    xpc_release( xdict );
                if( q )
                    dispatch_release( q );
            }
        };

        struct once
        {
            std::atomic< bool > done{ false };
            bool claim() { return ! done.exchange( true ); }
        };

        struct sync_op
        {
            std::shared_ptr< messenger > m;
            usb_transfer * x = nullptr;
            std::vector< uint8_t > buf;
            xpc_object_t msg = nullptr;
            bool in = false, control = false;
        };

        struct request
        {
            usbd_core::impl * owner = nullptr;
            uint64_t r = 0;
            std::shared_ptr< struct connection > c;
            std::shared_ptr< struct messenger > m;
            usb_transfer * x = nullptr;
            void * map = nullptr;
            size_t map_len = 0;
            uint32_t len = 0;
            xpc_object_t shm = nullptr;
            bool inflight = false, in_table = true;
            std::shared_ptr< request > self;   // the in-flight reference (2.6.6)
            ~request();
        };

        struct connection : std::enable_shared_from_this< connection >
        {
            enum class st_t { fresh, ready, dead };
            uint64_t id = 0;
            xpc_connection_t peer = nullptr;
            dispatch_queue_t q = nullptr;
            uid_t euid = 0;
            pid_t pid = 0;
            std::string path;   // proc_pidpath of the peer, diagnostics only
            std::function< void( xpc_object_t ) > sink;
            std::function< void( std::function< void() > ) > barrier;
            // connection queue
            st_t st = st_t::fresh;
            uint64_t gen = 0;
            uint64_t pv = 0;   // the protocol version agreed at HELLO: what this connection speaks
            std::map< uint64_t, std::shared_ptr< messenger > > handles;
            std::map< uint64_t, std::shared_ptr< request > > requests;
            uint64_t next_h = 1, next_r = 1;
            unsigned pending_opens = 0, sync_ops = 0;
            uint64_t shm = 0, inflight = 0, sent = 0, acked = 0;
            std::deque< std::pair< uint64_t, uint64_t > > barriers;   // (send count, start ms)
            // registry mutex
            std::set< uint64_t > leased;
            bool dead = false;
            std::atomic< bool > dead_flag{ false };
            ~connection()
            {
                if( peer )
                    xpc_release( peer );
                if( q )
                    dispatch_release( q );
            }
        };

        struct messenger
        {
            uint64_t h = 0, bh = 0;
            std::shared_ptr< device > dev;
            std::shared_ptr< connection > conn;
            std::vector< uint8_t > ifs;
            bool backend_closed = false;   // device queue
            // connection queue
            bool closing = false, gone_close = false;
            int inflight = 0;
            std::set< sync_op * > ops;
            std::function< void() > on_drained;
        };
    }

    class session : public connection {};

    class usbd_core::impl : public std::enable_shared_from_this< usbd_core::impl >
    {
    public:
        std::unique_ptr< usb_backend > be;
        std::unique_ptr< peer_policy > pol;
        config cfg;
        hwm_policy hwm;
        timing tm;
        state_store states;

        std::mutex mu;   // registry mutex
        std::map< uint64_t, std::shared_ptr< device > > devices;
        std::set< uint64_t > pending, ignored;
        std::map< uint64_t, std::shared_ptr< connection > > conns;
        std::map< uid_t, unsigned > new_per_uid;
        unsigned ready = 0;
        uint64_t next_conn = 1;
        struct watch { std::weak_ptr< device > dev; uint64_t start; std::function< void() > fire; bool fired = false; };
        std::map< uint64_t, watch > watches;
        uint64_t next_watch = 1;
        bool sweep_done = false, stopping = false;

        std::atomic< uint64_t > shm_total{ 0 }, inflight_total{ 0 }, requests_live{ 0 };
        uint64_t shm_limit = RSUSB_MAX_SHM_TOTAL;
        std::atomic< uint64_t > last_refresh{ 0 };

        dispatch_queue_t reg_q, aux_q, main_q;
        dispatch_source_t wd_timer = nullptr;
        xpc_connection_t listener = nullptr;

        impl( std::unique_ptr< usb_backend > b, std::unique_ptr< peer_policy > p, const config & c, const timing & t, const paths & ps )
            : be( std::move( b ) ), pol( std::move( p ) ), cfg( c ), hwm( c ), tm( t ), states( ps )
        {
            reg_q = dispatch_queue_create( "usbd.registry", DISPATCH_QUEUE_SERIAL );
            aux_q = dispatch_queue_create( "usbd.aux", DISPATCH_QUEUE_CONCURRENT );
            main_q = dispatch_queue_create( "usbd.main", DISPATCH_QUEUE_SERIAL );
            uint64_t mem = 0;
            size_t len = sizeof( mem );
            if( sysctlbyname( "hw.memsize", &mem, &len, nullptr, 0 ) == 0 && mem / 8 < shm_limit )
                shm_limit = mem / 8;
        }

        static dispatch_queue_t interactive_queue( const char * label )
        {
            return dispatch_queue_create( label, dispatch_queue_attr_make_with_qos_class( DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0 ) );
        }

        // ---------------------------------------------------------------------------------------------------------
        // Registry

        void start( bool bg_sweep )
        {
            states.prepare_dir();
            usb_backend::events ev;
            ev.wanted = []( uint16_t vid, uint16_t pid ) { return vid_pid_allowed( vid, pid ); };
            auto self = shared_from_this();
            ev.arrived = [self]( uint64_t ) { async( self->reg_q, [self] { self->refresh(); } ); };
            ev.left = [self]( uint64_t key ) {
                self->lost( key, "unplugged" );
                async( self->reg_q, [self] { self->refresh(); } );
            };
            be->start( ev );
            pol->set_change_handler( [self]( peer_policy::change c ) { self->policy_changed( c ); } );
            wd_timer = dispatch_source_create( DISPATCH_SOURCE_TYPE_TIMER, 0, 0, main_q );
            uint64_t period = std::min< uint64_t >( 1000, std::max< uint64_t >( 20, tm.watchdog_ms / 4 ) );
            dispatch_source_set_timer( wd_timer, dispatch_time( DISPATCH_TIME_NOW, int64_t( period * NSEC_PER_MSEC ) ),
                                       period * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC );
            std::weak_ptr< impl > weak = self;
            auto * holder = new std::weak_ptr< impl >( weak );
            dispatch_set_context( wd_timer, holder );
            dispatch_source_set_event_handler_f( wd_timer, []( void * c ) {
                if( auto s = static_cast< std::weak_ptr< impl > * >( c )->lock() )
                    s->tick();
            } );
            dispatch_resume( wd_timer );
            sync( reg_q, [this] { refresh(); } );
            if( bg_sweep )
                async( aux_q, [self] { self->sweep(); } );
        }

        void refresh()
        {
            last_refresh = now_ms();
            std::vector< uint64_t > keys = be->list();
            std::set< uint64_t > present( keys.begin(), keys.end() );
            for( uint64_t k : keys )
            {
                {
                    std::lock_guard< std::mutex > lock( mu );
                    if( devices.count( k ) || ignored.count( k ) )
                        continue;
                }
                device_description desc;
                if( ! be->describe( k, desc ) || ! desc.complete )
                {
                    std::lock_guard< std::mutex > lock( mu );
                    pending.insert( k );
                    continue;
                }
                if( ! device_allowed( desc.snap ) )
                {
                    log_info( "device", "device %04x:%04x at %{public}s refused by the device filter", desc.snap.vid(),
                              desc.snap.pid(), desc.snap.path.c_str() );
                    std::lock_guard< std::mutex > lock( mu );
                    pending.erase( k );
                    ignored.insert( k );
                    continue;
                }
                auto d = std::make_shared< device >();
                d->tok = k;
                d->snap = desc.snap;
                d->registry_id = desc.registry_id;
                d->xdict = snapshot_to_xpc( d->snap, k );
                d->q = interactive_queue( "usbd.device" );
                {
                    std::lock_guard< std::mutex > lock( mu );
                    pending.erase( k );
                    devices[k] = d;
                }
                log_info( "device", "device %04x:%04x arrived at %{public}s (token %llu)", d->snap.vid(), d->snap.pid(),
                          d->snap.path.c_str(), (unsigned long long)k );
            }
            std::vector< std::shared_ptr< device > > missing;
            {
                std::lock_guard< std::mutex > lock( mu );
                for( auto & e : devices )
                    if( ! present.count( e.first ) && ! e.second->gone )
                        missing.push_back( e.second );
                for( auto it = pending.begin(); it != pending.end(); )
                    it = present.count( *it ) ? std::next( it ) : pending.erase( it );
            }
            for( auto & d : missing )
                device_lost( d, "no longer listed" );
        }

        void maybe_refresh()
        {
            if( now_ms() - last_refresh > 1000 )
                sync( reg_q, [this] { refresh(); } );
        }

        std::shared_ptr< device > find( uint64_t tok )
        {
            auto it = devices.find( tok );
            return it == devices.end() ? nullptr : it->second;
        }

        void lost( uint64_t key, const char * why )
        {
            std::shared_ptr< device > d;
            {
                std::lock_guard< std::mutex > lock( mu );
                d = find( key );
                pending.erase( key );
                ignored.erase( key );
            }
            if( d )
                device_lost( d, why );
        }

        // 2.8.1: first loss report wins
        void device_lost( const std::shared_ptr< device > & d, const char * why )
        {
            std::vector< std::shared_ptr< connection > > notify;
            {
                std::lock_guard< std::mutex > lock( mu );
                if( d->state == dstate::gone )
                    return;
                d->state = dstate::gone;
                d->gone = true;
                cancel_grace( *d );
                std::set< uint64_t > ids;
                for( auto & l : d->leases )
                    if( auto c = l.second.lock() )
                        ids.insert( c->id ), notify.push_back( c );
                for( auto & h : d->handle_conns )
                    if( auto c = h.second.first.lock() )
                        if( ids.insert( c->id ).second )
                            notify.push_back( c );
            }
            log_info( "device", "device %04x:%04x token %llu lost (%{public}s)", d->snap.vid(), d->snap.pid(),
                      (unsigned long long)d->tok, why );
            auto self = shared_from_this();
            for( auto & c : notify )
                async( c->q, [self, c, d] { self->on_device_gone( c, d ); } );
            async( d->q, [self, d] { self->maybe_finalize( d ); } );
        }

        void maybe_finalize( const std::shared_ptr< device > & d )
        {
            uint64_t anchor = 0, rid = 0;
            {
                std::lock_guard< std::mutex > lock( mu );
                if( d->state != dstate::gone || d->finalized || d->messengers > 0 )
                    return;
                d->finalized = true;
                anchor = d->anchor;
                d->anchor = 0;
                rid = d->registry_id;
                for( auto & l : d->leases )
                    if( auto c = l.second.lock() )
                        c->leased.erase( d->tok );
                d->leases.clear();
                devices.erase( d->tok );
            }
            if( anchor )
                be->close_anchor( anchor );
            states.remove( rid );
            be->forget( d->tok );
        }

        void cancel_grace( device & d )
        {
            d.grace_armed = false;
            ++d.grace_gen;
        }

        // registry mutex held
        void maybe_arm_grace( const std::shared_ptr< device > & d )
        {
            if( d->state != dstate::listed || ! d->leases.empty() || d->messengers > 0 || ! ( d->anchor || d->captured )
                || d->grace_armed || stopping )
                return;
            d->grace_armed = true;
            uint64_t g = ++d->grace_gen;
            auto self = shared_from_this();
            after_ms( tm.grace_ms, d->q, [self, d, g] { self->grace_expired( d, g ); } );
        }

        void grace_expired( const std::shared_ptr< device > & d, uint64_t g )
        {
            uint64_t anchor;
            bool captured;
            handback_target t;
            {
                std::lock_guard< std::mutex > lock( mu );
                if( g != d->grace_gen || ! d->grace_armed )
                    return;
                d->grace_armed = false;
                if( d->state != dstate::listed || ! d->leases.empty() || d->messengers > 0 )
                    return;
                d->state = dstate::handing_back;
                anchor = d->anchor;
                d->anchor = 0;
                d->active_cfg = -1;
                captured = d->captured;
                t = target( *d );
            }
            if( anchor )
                watched( d, nullptr, [&] { be->close_anchor( anchor ); return 0; } );
            if( ! captured )
            {
                std::lock_guard< std::mutex > lock( mu );
                if( d->state == dstate::handing_back )
                    d->state = dstate::listed;
                return;
            }
            hand_back( d, t, true );
        }

        handback_target target( const device & d )
        {
            handback_target t;
            t.registry_id = d.registry_id;
            t.vid = d.snap.vid();
            t.pid = d.snap.pid();
            t.bus = uint8_t( std::atoi( d.snap.path.c_str() ) );
            auto dash = d.snap.path.find_last_of( '-' );
            t.address = uint16_t( std::atoi( d.snap.path.c_str() + dash + 1 ) );
            return t;
        }

        // device queue (or shutdown); device state is handing_back
        void hand_back( const std::shared_ptr< device > & d, const handback_target & t, bool rearm )
        {
            auto back_to_listed = [&]( bool arm ) {
                std::lock_guard< std::mutex > lock( mu );
                if( d->state == dstate::handing_back )
                    d->state = dstate::listed;
                if( arm && rearm )
                    maybe_arm_grace( d );
            };
            if( be->interfaces_held_elsewhere( t.registry_id ) )
            {
                log_info( "device", "hand-back of %04x:%04x skipped: an interface is open in another process", t.vid, t.pid );
                return back_to_listed( true );
            }
            int r = watched( d, nullptr, [&] { return be->hand_back( t, tm.handback_poll_ms, tm.handback_wait_ms ); } );
            if( r == 0 )
            {
                {
                    std::lock_guard< std::mutex > lock( mu );
                    d->captured = false;
                }
                states.remove( t.registry_id );
                log_info( "device", "device %04x:%04x handed back to macOS", t.vid, t.pid );
                return;
            }
            log_info( "device", "hand-back of %04x:%04x failed (%d)", t.vid, t.pid, r );
            back_to_listed( r == RSUSB_ST_ACCESS );
        }

        // ---------------------------------------------------------------------------------------------------------
        // Watchdog (2.7.10)

        template< class F >
        int watched( const std::shared_ptr< device > & d, std::function< void() > fire, F && f )
        {
            uint64_t id;
            {
                std::lock_guard< std::mutex > lock( mu );
                id = next_watch++;
                watches[id] = { d, now_ms(), std::move( fire ) };
            }
            int r = f();
            std::lock_guard< std::mutex > lock( mu );
            watches.erase( id );
            return r;
        }

        uint64_t watch_begin( const std::shared_ptr< device > & d )
        {
            std::lock_guard< std::mutex > lock( mu );
            watches[next_watch] = { d, now_ms(), nullptr };
            return next_watch++;
        }

        void watch_end( uint64_t id )
        {
            std::lock_guard< std::mutex > lock( mu );
            watches.erase( id );
        }

        void tick()
        {
            std::vector< std::pair< std::shared_ptr< device >, std::function< void() > > > fired;
            std::vector< std::shared_ptr< connection > > cs;
            {
                std::lock_guard< std::mutex > lock( mu );
                uint64_t now = now_ms();
                for( auto & w : watches )
                    if( ! w.second.fired && now - w.second.start > tm.watchdog_ms )
                    {
                        w.second.fired = true;
                        if( auto d = w.second.dev.lock() )
                            fired.emplace_back( d, w.second.fire );
                    }
                for( auto & c : conns )
                    cs.push_back( c.second );
            }
            auto self = shared_from_this();
            for( auto & f : fired )
            {
                auto d = f.first;
                log_fault( "device", "device %04x:%04x token %llu: a device call exceeded the watchdog", d->snap.vid(),
                           d->snap.pid(), (unsigned long long)d->tok );
                device_lost( d, "watchdog" );
                if( f.second )
                    f.second();
                uint64_t rid;
                {
                    std::lock_guard< std::mutex > lock( mu );
                    if( d->forced )
                        continue;
                    d->forced = true;
                    rid = d->registry_id;
                }
                async( aux_q, [self, rid] { self->be->force_reenumerate( rid ); } );
            }
            for( auto & c : cs )
                async( c->q, [self, c] { self->check_backpressure( c ); } );
        }

        // ---------------------------------------------------------------------------------------------------------
        // Connections

        void accept( xpc_connection_t peer )
        {
            auto c = std::make_shared< session >();
            c->peer = peer;
            xpc_retain( peer );
            c->euid = pol->peer_uid( peer );
            c->pid = pol->peer_pid( peer );
            char path[PROC_PIDPATHINFO_MAXSIZE] = {};
            if( c->pid > 0 && proc_pidpath( c->pid, path, sizeof( path ) ) > 0 )
                c->path = path;
            c->q = interactive_queue( "usbd.connection" );
            bool ok = ! stopping && pol->prepare( peer );
            bool over;
            {
                std::lock_guard< std::mutex > lock( mu );
                c->id = next_conn++;
                conns[c->id] = c;
                over = ++new_per_uid[c->euid] > RSUSB_MAX_NEW_PER_UID;
            }
            xpc_connection_set_target_queue( peer, c->q );
            std::weak_ptr< impl > weak = shared_from_this();
            std::weak_ptr< connection > wc = c;
            auto * h = new std::pair< std::weak_ptr< impl >, std::weak_ptr< connection > >( weak, wc );
            // Events are re-queued with dispatch_async on the same serial queue: same order, and the hand-off is visible
            // to ThreadSanitizer (it does not model the serialization of libxpc handler invocations)
            xpc_connection_set_event_handler( peer, ^( xpc_object_t e ) {
                auto s = h->first.lock();
                auto conn = h->second.lock();
                if( s && conn )
                {
                    xpc_retain( e );
                    async( conn->q, [s, conn, e] {
                        s->on_event( conn, e );
                        xpc_release( e );
                    } );
                }
                if( e == XPC_ERROR_CONNECTION_INVALID )
                    delete h;
            } );
            xpc_connection_resume( peer );
            if( ! ok || over )
            {
                log_info( "conn", "connection of pid %{public}d uid %{public}d refused (%{public}s)", c->pid, c->euid,
                          over ? "too many new connections" : "code-signing requirement not set" );
                xpc_connection_cancel( peer );
                return;
            }
            auto self = shared_from_this();
            after_ms( tm.new_timeout_ms, c->q, [self, c] {
                if( c->st == connection::st_t::fresh )
                {
                    log_info( "conn", "connection of pid %{public}d uid %{public}d cancelled: no HELLO", c->pid, c->euid );
                    self->cancel( c );
                }
            } );
        }

        void cancel( const std::shared_ptr< connection > & c )
        {
            if( c->peer )
                xpc_connection_cancel( c->peer );
            else
            {
                auto self = shared_from_this();
                async( c->q, [self, c] { self->connection_end( c ); } );
            }
        }

        void on_event( const std::shared_ptr< connection > & c, xpc_object_t e )
        {
            xpc_type_t t = xpc_get_type( e );
            if( t == XPC_TYPE_ERROR )
            {
                if( e == XPC_ERROR_CONNECTION_INVALID )
                    connection_end( c );
                else if( e == XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT )
                {
                    log_info( "policy", "pid %{public}d uid %{public}d rejected: code-signing requirement (%{public}s)", c->pid,
                              c->euid, c->path.c_str() );
                    cancel( c );
                }
                return;
            }
            if( t != XPC_TYPE_DICTIONARY || c->st == connection::st_t::dead || ! pol->deliver( c->peer ) )
                return;
            handle( c, e );
        }

        void malformed( const std::shared_ptr< connection > & c, const char * what )
        {
            uint32_t sup = 0;
            if( log_rate( c->pid, "malformed", sup ) )
                log_info( "conn", "pid %{public}d uid %{public}d: malformed message (%{public}s), cancelled (%u suppressed)",
                          c->pid, c->euid, what, sup );
            cancel( c );
        }

        // 2.6.8: every RSUSB_BARRIER_EVERY sends a barrier records how far the peer has taken our messages
        void send( const std::shared_ptr< connection > & c, xpc_object_t m )
        {
            if( c->sink )
                c->sink( m );
            else if( c->peer )
                xpc_connection_send_message( c->peer, m );
            else
                return;
            uint64_t v = ++c->sent;
            if( v % tm.barrier_every == 0 && ( c->peer || c->barrier ) )
            {
                c->barriers.emplace_back( v, now_ms() );
                std::weak_ptr< connection > weak = c;
                std::function< void() > ack = [weak, v] {
                    if( auto conn = weak.lock() )
                        async( conn->q, [conn, v] {
                            conn->acked = std::max( conn->acked, v );
                            while( ! conn->barriers.empty() && conn->barriers.front().first <= v )
                                conn->barriers.pop_front();
                        } );
                };
                if( c->barrier )
                    c->barrier( ack );
                else
                {
                    auto * keep = new std::function< void() >( ack );
                    xpc_connection_send_barrier( c->peer, ^{
                        ( *keep )();
                        delete keep;
                    } );
                }
            }
            else if( ! c->peer && ! c->barrier )
                c->acked = v;
            check_backpressure( c );
        }

        void check_backpressure( const std::shared_ptr< connection > & c )
        {
            if( c->st == connection::st_t::dead || ( ! c->peer && ! c->barrier ) )
                return;
            bool stalled = ! c->barriers.empty() && now_ms() - c->barriers.front().second > tm.barrier_stall_ms;
            if( c->sent - c->acked > tm.max_unacked || stalled )
            {
                log_info( "conn", "pid %{public}d uid %{public}d cancelled: not reading its messages", c->pid, c->euid );
                c->barriers.clear();
                c->acked = c->sent;   // report once
                cancel( c );
            }
        }

        xpc_object_t make_reply( const std::shared_ptr< connection > & c, xpc_object_t msg )
        {
            xpc_object_t r = c->peer ? xpc_dictionary_create_reply( msg ) : nullptr;
            if( ! r && c->sink )
                r = xpc_dictionary_create( nullptr, nullptr, 0 );
            return r;
        }

        void reply( const std::shared_ptr< connection > & c, xpc_object_t msg, int st, std::function< void( xpc_object_t ) > fill = nullptr )
        {
            xpc_object_t r = make_reply( c, msg );
            if( ! r )
                return;
            xpc_dictionary_set_int64( r, RSUSB_KEY_STATUS, st );
            if( fill && st == 0 )
                fill( r );
            send( c, r );
            xpc_release( r );
        }

        void deny_log( const std::shared_ptr< connection > & c, const char * what )
        {
            uint32_t sup = 0;
            if( log_rate( c->pid, what, sup ) )
                log_info( "policy", "pid %{public}d uid %{public}d denied: %{public}s (%u suppressed)", c->pid, c->euid, what, sup );
        }

        bool revoked( const std::shared_ptr< connection > & c )
        {
            if( c->euid == 0 )
                return false;
            console_state cs = pol->console();
            if( c->gen != cs.gen && c->euid != cs.uid )
            {
                log_info( "policy", "pid %{public}d uid %{public}d revoked: not the console user", c->pid, c->euid );
                cancel( c );
                return true;
            }
            return false;
        }

        unsigned lease_units( const connection & c ) { return std::max< unsigned >( 1, unsigned( c.leased.size() ) ); }
        unsigned lease_units_locked( const connection & c )
        {
            std::lock_guard< std::mutex > lock( mu );
            return lease_units( c );
        }

        void handle( const std::shared_ptr< connection > & c, xpc_object_t m )
        {
            uint64_t op;
            if( ! get_u64( m, RSUSB_KEY_OP, op ) )
                return malformed( c, "op" );
            if( c->st == connection::st_t::fresh )
            {
                if( op != RSUSB_OP_HELLO )
                    return malformed( c, "op before HELLO" );
                return do_hello( c, m );
            }
            switch( op )
            {
            case RSUSB_OP_ENUMERATE: return do_enumerate( c, m );
            case RSUSB_OP_CLOSE: return do_close( c, m );
            case RSUSB_OP_LEASE: case RSUSB_OP_OPEN: case RSUSB_OP_CONTROL: case RSUSB_OP_BULK: case RSUSB_OP_RESET_EP:
            case RSUSB_OP_REQ_CREATE: case RSUSB_OP_REQ_SUBMIT: case RSUSB_OP_REQ_CANCEL: case RSUSB_OP_REQ_DESTROY:
                break;
            default:
                return malformed( c, op == RSUSB_OP_HELLO ? "second HELLO" : "unknown op" );
            }
            if( revoked( c ) )
                return;
            switch( op )
            {
            case RSUSB_OP_LEASE: return do_lease( c, m );
            case RSUSB_OP_OPEN: return do_open_msg( c, m );
            case RSUSB_OP_CONTROL: return do_control( c, m );
            case RSUSB_OP_BULK: return do_bulk( c, m );
            case RSUSB_OP_RESET_EP: return do_reset_ep( c, m );
            case RSUSB_OP_REQ_CREATE: return do_req_create( c, m );
            case RSUSB_OP_REQ_SUBMIT: return do_req_submit( c, m );
            case RSUSB_OP_REQ_CANCEL: return do_req_cancel( c, m );
            case RSUSB_OP_REQ_DESTROY: return do_req_destroy( c, m );
            }
        }

        void do_hello( const std::shared_ptr< connection > & c, xpc_object_t m )
        {
            uint64_t pv;
            if( ! get_u64( m, RSUSB_KEY_VERSION, pv ) )
                return malformed( c, "pv" );
            // Every protocol version from protocol_min to protocol_max is served and answered with the client's own
            // pv; any other pv gets AUTH_VERSION and the newest version this daemon speaks (protocol header, HELLO)
            bool served = pv >= cfg.protocol_min && pv <= cfg.protocol_max;
            rsusb_auth auth = served ? pol->hello( c->peer ) : RSUSB_AUTH_VERSION;
            if( auth == RSUSB_AUTH_OK )
            {
                console_state cs = pol->console();
                std::lock_guard< std::mutex > lock( mu );
                if( ready >= RSUSB_MAX_CLIENTS )
                    auth = RSUSB_AUTH_LIMIT;
                else
                {
                    ++ready;
                    --new_per_uid[c->euid];
                    c->st = connection::st_t::ready;
                    c->gen = cs.gen;
                    c->pv = pv;
                }
            }
            xpc_object_t r = make_reply( c, m );
            if( r )
            {
                xpc_dictionary_set_int64( r, RSUSB_KEY_STATUS, 0 );
                xpc_dictionary_set_uint64( r, RSUSB_KEY_VERSION, served ? pv : cfg.protocol_max );
                xpc_dictionary_set_uint64( r, RSUSB_KEY_AUTH, auth );
                xpc_dictionary_set_uint64( r, RSUSB_KEY_SERVICE_VERSION, cfg.service_version );
                std::string build = "realsense-usbd " + std::to_string( cfg.service_version );
                xpc_dictionary_set_string( r, RSUSB_KEY_BUILD, build.substr( 0, RSUSB_MAX_BUILD_LEN ).c_str() );
                send( c, r );
                xpc_release( r );
            }
            uint32_t sup = 0;
            if( auth == RSUSB_AUTH_OK )
                log_info( "conn", "pid %{public}d uid %{public}d admitted (protocol %llu, %{public}s)", c->pid, c->euid,
                          (unsigned long long)c->pv, c->path.c_str() );
            else if( log_rate( c->pid, "hello", sup ) )
                log_info( "policy", "pid %{public}d uid %{public}d rejected at HELLO (auth %{public}d, %{public}s, %u suppressed)",
                          c->pid, c->euid, int( auth ), c->path.c_str(), sup );
            if( auth != RSUSB_AUTH_OK )
            {
                if( ! c->peer )
                    return cancel( c );
                auto * keep = new std::shared_ptr< impl >( shared_from_this() );
                auto * kc = new std::shared_ptr< connection >( c );
                xpc_connection_send_barrier( c->peer, ^{
                    ( *keep )->cancel( *kc );
                    delete keep;
                    delete kc;
                } );
            }
        }

        void connection_end( const std::shared_ptr< connection > & c )
        {
            if( c->st == connection::st_t::dead )
                return;
            auto prev = c->st;
            c->st = connection::st_t::dead;
            {
                std::lock_guard< std::mutex > lock( mu );
                c->dead = true;
                c->dead_flag = true;
                if( prev == connection::st_t::fresh )
                    --new_per_uid[c->euid];
                else
                    --ready;
                conns.erase( c->id );
                for( uint64_t tok : c->leased )
                    if( auto d = find( tok ) )
                    {
                        d->leases.erase( c->id );
                        maybe_arm_grace( d );
                    }
                c->leased.clear();
            }
            log_info( "conn", "pid %{public}d uid %{public}d disconnected", c->pid, c->euid );
            auto hs = c->handles;
            for( auto & h : hs )
                if( ! h.second->closing )
                    begin_close( c, h.second, nullptr );
            for( auto & r : c->requests )
                destroy_request( r.second );
            c->requests.clear();
            if( c->peer )
                pol->forget( c->peer );
        }

        void policy_changed( peer_policy::change ch )
        {
            std::vector< std::shared_ptr< connection > > cs;
            {
                std::lock_guard< std::mutex > lock( mu );
                for( auto & c : conns )
                    cs.push_back( c.second );
            }
            if( ch == peer_policy::change::console )
            {
                console_state st = pol->console();
                for( auto & c : cs )
                    if( c->euid != 0 && c->euid != st.uid )
                    {
                        log_info( "policy", "pid %{public}d uid %{public}d revoked: console user changed", c->pid, c->euid );
                        cancel( c );
                    }
            }
            else if( ! pol->camera_allowed() )
            {
                log_info( "policy", "camera disabled by policy: cancelling every connection" );
                for( auto & c : cs )
                    cancel( c );
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // ENUMERATE, LEASE

        void do_enumerate( const std::shared_ptr< connection > & c, xpc_object_t m )
        {
            maybe_refresh();
            std::vector< std::pair< uint64_t, xpc_object_t > > list;
            {
                std::lock_guard< std::mutex > lock( mu );
                for( auto & d : devices )
                    if( usable( d.second->state ) )
                        list.emplace_back( d.second->registry_id, xpc_retain( d.second->xdict ) );
            }
            std::stable_sort( list.begin(), list.end(), []( const std::pair< uint64_t, xpc_object_t > & a, const std::pair< uint64_t, xpc_object_t > & b ) { return a.first < b.first; } );
            reply( c, m, 0, [&]( xpc_object_t r ) {
                xpc_object_t arr = xpc_array_create( nullptr, 0 );
                for( size_t i = 0; i < list.size() && i < RSUSB_MAX_DEVICES; ++i )
                    xpc_array_append_value( arr, list[i].second );
                xpc_dictionary_set_value( r, RSUSB_KEY_DEVICES, arr );
                xpc_release( arr );
            } );
            for( auto & e : list )
                xpc_release( e.second );
        }

        // registry mutex held: 0, NO_DEVICE or NO_MEM
        int take_lease( const std::shared_ptr< connection > & c, const std::shared_ptr< device > & d )
        {
            if( ! usable( d->state ) || c->dead )
                return RSUSB_ST_NO_DEVICE;
            if( ! c->leased.count( d->tok ) )
            {
                if( c->leased.size() >= RSUSB_MAX_DEVICES )
                    return RSUSB_ST_NO_MEM;
                c->leased.insert( d->tok );
                d->leases[c->id] = c;
                log_info( "device", "pid %{public}d uid %{public}d leases %04x:%04x token %llu", c->pid, c->euid,
                          d->snap.vid(), d->snap.pid(), (unsigned long long)d->tok );
            }
            cancel_grace( *d );
            return 0;
        }

        void do_lease( const std::shared_ptr< connection > & c, xpc_object_t m )
        {
            uint64_t tok;
            if( ! get_u64( m, RSUSB_KEY_TOKEN, tok ) )
                return malformed( c, "tok" );
            {
                std::lock_guard< std::mutex > lock( mu );
                auto d = find( tok );
                if( ! d || ! usable( d->state ) )
                    return reply( c, m, RSUSB_ST_NO_DEVICE );
            }
            if( ! pol->camera_allowed() )
            {
                deny_log( c, "camera disabled (LEASE)" );
                return reply( c, m, RSUSB_ST_ACCESS );
            }
            int st;
            {
                std::lock_guard< std::mutex > lock( mu );
                auto d = find( tok );
                st = d ? take_lease( c, d ) : RSUSB_ST_NO_DEVICE;
            }
            reply( c, m, st );
        }

        // ---------------------------------------------------------------------------------------------------------
        // OPEN (2.5.3 - 2.5.5)

        struct open_op
        {
            std::shared_ptr< connection > c;
            std::shared_ptr< device > d;
            uint8_t iface = 0;
            xpc_object_t msg = nullptr;
            once answered;
            ~open_op() { xpc_release( msg ); }
        };

        void do_open_msg( const std::shared_ptr< connection > & c, xpc_object_t m )
        {
            uint64_t tok, iface;
            if( ! get_u64( m, RSUSB_KEY_TOKEN, tok ) || ! get_u64( m, RSUSB_KEY_INTERFACE, iface ) )
                return malformed( c, "OPEN keys" );
            std::shared_ptr< device > d;
            {
                std::lock_guard< std::mutex > lock( mu );
                d = find( tok );
                if( ! d || ! usable( d->state ) )
                    return reply( c, m, RSUSB_ST_NO_DEVICE );
                if( iface > 255 || ! interface_in_any_config( d->snap, unsigned( iface ) ) )
                    return reply( c, m, RSUSB_ST_NOT_FOUND );
                unsigned units = std::max< unsigned >( 1, unsigned( c->leased.size() + ( c->leased.count( tok ) ? 0 : 1 ) ) );
                if( c->handles.size() + c->pending_opens >= RSUSB_QUOTA_HANDLES_PER_DEVICE * units )
                    return reply( c, m, RSUSB_ST_NO_MEM );
            }
            auto op = std::make_shared< open_op >();
            op->c = c;
            op->d = d;
            op->iface = uint8_t( iface );
            op->msg = xpc_retain( m );
            ++c->pending_opens;
            auto self = shared_from_this();
            async( d->q, [self, op] { self->run_open( op ); } );
        }

        std::function< void() > open_fire( const std::shared_ptr< open_op > & op )
        {
            auto self = shared_from_this();
            return [self, op] {
                if( op->answered.claim() )
                    async( op->c->q, [self, op] {
                        --op->c->pending_opens;
                        if( op->c->st != connection::st_t::dead )
                            self->reply( op->c, op->msg, RSUSB_ST_NO_DEVICE );
                    } );
            };
        }

        void open_failed( const std::shared_ptr< open_op > & op, int st )
        {
            auto self = shared_from_this();
            async( op->c->q, [self, op, st] {
                if( ! op->answered.claim() )
                    return;
                --op->c->pending_opens;
                if( op->c->st != connection::st_t::dead )
                    self->reply( op->c, op->msg, st );
            } );
        }

        // device queue
        int open_anchor( const std::shared_ptr< device > & d, const std::shared_ptr< open_op > & op )
        {
            uint64_t h = 0;
            auto fire = open_fire( op );
            int r = watched( d, fire, [&] { return be->open_anchor( d->tok, h ); } );
            if( r )
                return r;
            int cap_if = -1;
            bool video_or_hid = false;
            for( auto & i : d->snap.ifs )
            {
                video_or_hid |= i.alts[0].cls() == 0x0E || i.alts[0].cls() == 0x03;
                if( cap_if < 0 && watched( d, fire, [&] { return be->kernel_driver_active( h, i.number() ); } ) == 1 )
                    cap_if = i.number();
            }
            uint64_t rid;
            {
                std::lock_guard< std::mutex > lock( mu );
                rid = d->registry_id;
                if( cap_if >= 0 && d->state == dstate::listed )
                    d->state = dstate::capturing;
            }
            if( cap_if >= 0 )
            {
                captured_entry e{ rid, d->snap.vid(), d->snap.pid() };
                states.add( e );
                r = watched( d, fire, [&] { return be->capture( h, uint8_t( cap_if ) ); } );
                if( r == RSUSB_ST_ACCESS )
                {
                    be->close_anchor( h );
                    states.remove( rid );
                    std::lock_guard< std::mutex > lock( mu );
                    if( d->state == dstate::capturing )
                        d->state = dstate::listed;
                    log_info( "device", "capture of %04x:%04x refused (ACCESS)", d->snap.vid(), d->snap.pid() );
                    return r;
                }
                if( r == 0 )
                {
                    uint64_t nid = be->registry_id_of( d->tok );
                    if( nid && nid != rid )
                        states.update( rid, { nid, e.vid, e.pid } );
                    std::lock_guard< std::mutex > lock( mu );
                    d->captured = true;
                    if( nid )
                        d->registry_id = nid;
                    log_info( "device", "device %04x:%04x token %llu captured", d->snap.vid(), d->snap.pid(), (unsigned long long)d->tok );
                }
                else
                {
                    states.remove( rid );
                    log_info( "device", "capture of %04x:%04x failed (%d), claiming anyway", d->snap.vid(), d->snap.pid(), r );
                }
            }
            int cfg_index = 0;
            if( watched( d, fire, [&] { return be->active_config( h, cfg_index ); } ) != 0 )
                cfg_index = 0;
            std::lock_guard< std::mutex > lock( mu );
            d->anchor = h;
            d->active_cfg = cfg_index;
            d->capture_on_claim = cap_if < 0 && video_or_hid && ! d->captured;
            if( d->state == dstate::capturing )
                d->state = dstate::listed;
            return 0;
        }

        void run_open( const std::shared_ptr< open_op > & op )
        {
            auto & c = op->c;
            auto & d = op->d;
            if( c->dead_flag )
                return open_failed( op, RSUSB_ST_NO_DEVICE );
            if( ! pol->camera_allowed() )
            {
                deny_log( c, "camera disabled (OPEN)" );
                return open_failed( op, RSUSB_ST_ACCESS );
            }
            bool need_anchor;
            {
                std::lock_guard< std::mutex > lock( mu );
                int st = take_lease( c, d );
                if( st )
                    return open_failed( op, st );
                need_anchor = d->anchor == 0;
            }
            if( need_anchor )
                if( int r = open_anchor( d, op ) )
                    return open_failed( op, r );
            auto m = std::make_shared< messenger >();
            {
                std::lock_guard< std::mutex > lock( mu );
                if( d->state != dstate::listed || c->dead )
                    return open_failed( op, RSUSB_ST_NO_DEVICE );
                m->ifs = claim_set( d->snap, d->active_cfg, op->iface );
                if( m->ifs.empty() )
                    return open_failed( op, RSUSB_ST_NOT_FOUND );
                for( uint8_t i : m->ifs )
                {
                    auto it = d->owners.find( i );
                    if( it == d->owners.end() )
                        continue;
                    if( it->second.conn == c->id && it->second.releasing )
                    {
                        auto self = shared_from_this();
                        d->parked[it->second.m].push_back( [self, op] { self->run_open( op ); } );
                        return;
                    }
                    return open_failed( op, RSUSB_ST_ACCESS );
                }
                for( uint8_t i : m->ifs )
                    d->owners[i] = { m.get(), c->id, false };
                ++d->messengers;
                auto & hc = d->handle_conns[c->id];
                hc.first = c;
                ++hc.second;
            }
            m->dev = d;
            m->conn = c;
            auto fire = open_fire( op );
            int r = watched( d, fire, [&] { return be->open_handle( d->tok, m->bh ); } );
            std::vector< uint8_t > claimed;
            for( size_t k = 0; ! r && k < m->ifs.size(); ++k )
            {
                for( int attempt = 0; attempt < 3; ++attempt )
                {
                    if( attempt )
                        usleep( 10000 );
                    r = watched( d, fire, [&] { return be->claim( m->bh, m->ifs[k] ); } );
                    if( r != RSUSB_ST_BUSY )
                        break;
                }
                if( ! r )
                    claimed.push_back( m->ifs[k] );
            }
            if( r )
            {
                for( uint8_t i : claimed )
                    watched( d, nullptr, [&] { return be->release( m->bh, i ); } );
                if( m->bh )
                    watched( d, nullptr, [&] { be->close_handle( m->bh ); return 0; } );
                m->backend_closed = true;
                release_owner( m );
                maybe_finalize( d );
                return open_failed( op, r );
            }
            {
                std::lock_guard< std::mutex > lock( mu );
                if( d->capture_on_claim && ! d->captured )
                {
                    d->captured = true;
                    d->capture_on_claim = false;
                    states.add( { d->registry_id, d->snap.vid(), d->snap.pid() } );
                }
            }
            auto self = shared_from_this();
            async( c->q, [self, op, m] {
                auto & c = op->c;
                bool mine = op->answered.claim();   // false: the watchdog already answered
                if( mine )
                    --c->pending_opens;
                if( ! mine || c->st == connection::st_t::dead )
                {
                    async( m->dev->q, [self, m] { self->close_backend( m ); } );
                    return;
                }
                m->h = c->next_h++;
                c->handles[m->h] = m;
                uint64_t h = m->h;
                self->reply( c, op->msg, 0, [h]( xpc_object_t r ) { xpc_dictionary_set_uint64( r, RSUSB_KEY_HANDLE, h ); } );
            } );
        }

        // device queue: drops the messenger's interfaces from the owner map and runs OPENs parked on it
        void release_owner( const std::shared_ptr< messenger > & m )
        {
            auto & d = m->dev;
            std::vector< std::function< void() > > parked;
            {
                std::lock_guard< std::mutex > lock( mu );
                for( auto it = d->owners.begin(); it != d->owners.end(); )
                    it = it->second.m == m.get() ? d->owners.erase( it ) : std::next( it );
                auto p = d->parked.find( m.get() );
                if( p != d->parked.end() )
                {
                    parked = std::move( p->second );
                    d->parked.erase( p );
                }
                --d->messengers;
                auto hc = d->handle_conns.find( m->conn->id );
                if( hc != d->handle_conns.end() && --hc->second.second == 0 )
                    d->handle_conns.erase( hc );
                maybe_arm_grace( d );
            }
            for( auto & p : parked )
                p();
        }

        // device queue
        void close_backend( const std::shared_ptr< messenger > & m )
        {
            if( m->backend_closed )
                return;
            m->backend_closed = true;
            auto & d = m->dev;
            for( uint8_t i : m->ifs )
                watched( d, nullptr, [&] { return be->release( m->bh, i ); } );
            watched( d, nullptr, [&] { be->close_handle( m->bh ); return 0; } );
            release_owner( m );
            maybe_finalize( d );
        }

        // ---------------------------------------------------------------------------------------------------------
        // CLOSE (2.5.6) and loss on the connection side (2.8.2)

        std::shared_ptr< messenger > lookup( const std::shared_ptr< connection > & c, xpc_object_t m, bool & ok )
        {
            uint64_t h;
            ok = get_u64( m, RSUSB_KEY_HANDLE, h );
            auto it = ok ? c->handles.find( h ) : c->handles.end();
            return it == c->handles.end() || it->second->closing ? nullptr : it->second;
        }

        void drained( const std::shared_ptr< messenger > & m )
        {
            if( m->inflight == 0 && m->on_drained )
            {
                auto f = std::move( m->on_drained );
                m->on_drained = nullptr;
                f();
            }
        }

        void begin_close( const std::shared_ptr< connection > & c, const std::shared_ptr< messenger > & m, xpc_object_t msg )
        {
            m->closing = true;
            bool was_gone = m->dev->gone;
            {
                std::lock_guard< std::mutex > lock( mu );
                for( uint8_t i : m->ifs )
                {
                    auto it = m->dev->owners.find( i );
                    if( it != m->dev->owners.end() && it->second.m == m.get() )
                        it->second.releasing = true;
                }
            }
            uint64_t wid = watch_begin( m->dev );
            for( sync_op * op : m->ops )
                if( ! op->control || stopping )
                    watched( m->dev, nullptr, [&] { return be->cancel( op->x ); } );
            for( auto & r : c->requests )
                cancel_if_inflight( r.second, m.get() );
            if( msg )
                xpc_retain( msg );
            auto self = shared_from_this();
            m->on_drained = [self, c, m, msg, wid, was_gone] {
                self->watch_end( wid );
                async( m->dev->q, [self, c, m, msg, was_gone] {
                    self->close_backend( m );
                    async( c->q, [self, c, m, msg, was_gone] {
                        for( auto it = c->requests.begin(); it != c->requests.end(); )
                            it = it->second->m == m ? self->erase_request( c, it ) : std::next( it );
                        c->handles.erase( m->h );
                        if( msg )
                        {
                            if( c->st != connection::st_t::dead )
                                self->reply( c, msg, was_gone ? RSUSB_ST_NO_DEVICE : 0 );
                            xpc_release( msg );
                        }
                    } );
                } );
            };
            drained( m );
        }

        void do_close( const std::shared_ptr< connection > & c, xpc_object_t msg )
        {
            bool ok;
            auto m = lookup( c, msg, ok );
            if( ! ok )
                return malformed( c, "h" );
            if( ! m )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            begin_close( c, m, msg );
        }

        void on_device_gone( const std::shared_ptr< connection > & c, const std::shared_ptr< device > & d )
        {
            if( c->st != connection::st_t::dead )
            {
                xpc_object_t e = xpc_dictionary_create( nullptr, nullptr, 0 );
                xpc_dictionary_set_uint64( e, RSUSB_KEY_EVENT, RSUSB_EV_DEVICE_GONE );
                xpc_dictionary_set_uint64( e, RSUSB_KEY_TOKEN, d->tok );
                send( c, e );
                xpc_release( e );
            }
            auto self = shared_from_this();
            for( auto & h : c->handles )
            {
                auto m = h.second;
                if( m->dev != d || m->closing || m->gone_close )
                    continue;
                m->gone_close = true;
                m->on_drained = [self, m] { async( m->dev->q, [self, m] { self->close_backend( m ); } ); };
                drained( m );
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // CONTROL, BULK, RESET_EP (2.6.1 - 2.6.3)

        static int xs_to_status( int xs )
        {
            switch( xs )
            {
            case RSUSB_XS_COMPLETED: return RSUSB_ST_SUCCESS;
            case RSUSB_XS_TIMED_OUT: return RSUSB_ST_TIMEOUT;
            case RSUSB_XS_STALL: return RSUSB_ST_PIPE;
            case RSUSB_XS_NO_DEVICE: return RSUSB_ST_NO_DEVICE;
            case RSUSB_XS_OVERFLOW: return RSUSB_ST_OVERFLOW;
            default: return RSUSB_ST_IO;
            }
        }

        static uint32_t clamp_timeout( uint64_t to ) { return ( to == 0 || to > RSUSB_MAX_TIMEOUT_MS ) ? RSUSB_MAX_TIMEOUT_MS : uint32_t( to ); }

        const std::array< uint8_t, 7 > * find_endpoint( const messenger & m, uint64_t ep )
        {
            if( ep > 0xFF )
                return nullptr;
            int cfg;
            {
                std::lock_guard< std::mutex > lock( mu );
                cfg = m.dev->active_cfg;
            }
            for( uint8_t i : m.ifs )
                if( auto si = find_interface( m.dev->snap, cfg, i ) )
                    for( auto & e : si->eps )
                        if( e[2] == ep )
                            return &e;
            return nullptr;
        }

        // 0 ok, else a status; `p`/`n` the OUT payload
        int check_data( xpc_object_t m, bool out, uint64_t len, const uint8_t *& p, size_t & n, bool & bad_type )
        {
            p = nullptr;
            n = 0;
            int has = get_data( m, RSUSB_KEY_DATA, p, n );
            bad_type = has < 0;
            if( out && len > 0 )
                return has == 1 && n == len ? 0 : RSUSB_ST_INVALID_PARAM;
            return has == 0 ? 0 : RSUSB_ST_INVALID_PARAM;
        }

        bool sync_quota( const std::shared_ptr< connection > & c )
        {
            return c->sync_ops < RSUSB_QUOTA_SYNC_PER_DEVICE * lease_units_locked( *c );
        }

        void submit_sync( const std::shared_ptr< connection > & c, const std::shared_ptr< messenger > & m, sync_op * op,
                          usb_transfer::kind_t kind, uint8_t ep, uint64_t to )
        {
            op->x = be->alloc_transfer();
            op->x->kind = kind;
            op->x->handle = m->bh;
            op->x->endpoint = ep;
            op->x->buffer = op->buf.data();
            op->x->length = uint32_t( op->buf.size() );
            op->x->timeout_ms = clamp_timeout( to );
            auto self = shared_from_this();
            auto d = m->dev;
            op->x->done = [self, c, op, d]( int xs, uint32_t n ) {
                if( xs == RSUSB_XS_NO_DEVICE )
                    self->device_lost( d, "transfer completed with NO_DEVICE" );
                async( c->q, [self, c, op, xs, n] { self->complete_sync( c, op, xs, n ); } );
            };
            ++c->sync_ops;
            ++m->inflight;
            m->ops.insert( op );
            int r = watched( d, nullptr, [&] { return be->submit( op->x ); } );
            if( r )
            {
                --c->sync_ops;
                --m->inflight;
                m->ops.erase( op );
                reply( c, op->msg, r );
                finish_sync( op );
                drained( m );
            }
        }

        void finish_sync( sync_op * op )
        {
            be->free_transfer( op->x );
            xpc_release( op->msg );
            delete op;
        }

        void complete_sync( const std::shared_ptr< connection > & c, sync_op * op, int xs, uint32_t n )
        {
            auto m = op->m;
            --c->sync_ops;
            --m->inflight;
            m->ops.erase( op );
            int st = xs_to_status( xs );
            if( c->st != connection::st_t::dead )
            {
                uint32_t data_len = op->control ? uint32_t( op->buf.size() - 8 ) : uint32_t( op->buf.size() );
                n = std::min( n, data_len );
                const uint8_t * data = op->buf.data() + ( op->control ? 8 : 0 );
                bool in = op->in;
                reply( c, op->msg, st, [n, data, in]( xpc_object_t r ) {
                    xpc_dictionary_set_uint64( r, RSUSB_KEY_TRANSFERRED, n );
                    if( in )
                        xpc_dictionary_set_data( r, RSUSB_KEY_DATA, data, n );
                } );
            }
            finish_sync( op );
            drained( m );
        }

        void hwm_log( const std::shared_ptr< connection > & c, uint8_t opcode )
        {
            uint32_t sup = 0;
            if( log_rate( c->pid, "hwm", sup ) )
                log_info( "policy", "pid %{public}d uid %{public}d: HWM opcode 0x%{public}02x denied (%u suppressed)",
                          c->pid, c->euid, opcode, sup );
        }

        void do_control( const std::shared_ptr< connection > & c, xpc_object_t msg )
        {
            uint64_t h, rt, rq, val, idx, len, to;
            if( ! get_u64( msg, RSUSB_KEY_HANDLE, h ) || ! get_u64( msg, RSUSB_KEY_REQUEST_TYPE, rt )
                || ! get_u64( msg, RSUSB_KEY_REQUEST, rq ) || ! get_u64( msg, RSUSB_KEY_VALUE, val )
                || ! get_u64( msg, RSUSB_KEY_INDEX, idx ) || ! get_u64( msg, RSUSB_KEY_LENGTH, len )
                || ! get_u64( msg, RSUSB_KEY_TIMEOUT, to ) )
                return malformed( c, "CONTROL keys" );
            bool out = rt == RSUSB_RT_CLASS_IF_OUT;
            const uint8_t * p;
            size_t n;
            bool bad_type;
            int data_st = check_data( msg, out, len, p, n, bad_type );
            if( bad_type )
                return malformed( c, "data" );
            if( rt != RSUSB_RT_CLASS_IF_OUT && rt != RSUSB_RT_CLASS_IF_IN )
                return deny_log( c, "control request type" ), reply( c, msg, RSUSB_ST_ACCESS );
            if( rq > 0xFF || val > 0xFFFF || idx > 0xFFFF || len > RSUSB_MAX_CONTROL_LEN || data_st )
                return reply( c, msg, RSUSB_ST_INVALID_PARAM );
            auto it = c->handles.find( h );
            if( it == c->handles.end() || it->second->closing )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            auto m = it->second;
            if( m->dev->gone )
                return reply( c, msg, RSUSB_ST_NO_DEVICE );
            uint8_t iface = uint8_t( idx & 0xFF );
            if( std::find( m->ifs.begin(), m->ifs.end(), iface ) == m->ifs.end() )
                return deny_log( c, "control to an interface not held" ), reply( c, msg, RSUSB_ST_ACCESS );
            if( ! sync_quota( c ) )
                return reply( c, msg, RSUSB_ST_NO_MEM );
            auto op = new sync_op;
            op->m = m;
            op->in = ! out;
            op->control = true;
            op->msg = xpc_retain( msg );
            op->buf.resize( 8 + size_t( len ) );
            uint8_t setup[8] = { uint8_t( rt ), uint8_t( rq ), uint8_t( val ), uint8_t( val >> 8 ), uint8_t( idx ), uint8_t( idx >> 8 ),
                                 uint8_t( len ), uint8_t( len >> 8 ) };
            std::copy( setup, setup + 8, op->buf.begin() );
            if( out && len )
                std::copy( p, p + len, op->buf.begin() + 8 );
            if( out )
            {
                int cfg;
                {
                    std::lock_guard< std::mutex > lock( mu );
                    cfg = m->dev->active_cfg;
                }
                auto si = find_interface( m->dev->snap, cfg, iface );
                bool dfu = si && si->alts[0].cls() == RSUSB_CLASS_APP_SPECIFIC && si->alts[0].subclass() == RSUSB_SUBCLASS_DFU;
                uint8_t opcode = 0;
                if( ! dfu && hwm_denied( hwm, op->buf.data() + 8, size_t( len ), true, uint16_t( val ), uint16_t( idx ), &opcode ) )
                {
                    hwm_log( c, opcode );
                    reply( c, msg, RSUSB_ST_ACCESS );
                    xpc_release( op->msg );
                    delete op;
                    return;
                }
            }
            submit_sync( c, m, op, usb_transfer::control, 0, to );
        }

        void do_bulk( const std::shared_ptr< connection > & c, xpc_object_t msg )
        {
            uint64_t h, ep, len, to;
            if( ! get_u64( msg, RSUSB_KEY_HANDLE, h ) || ! get_u64( msg, RSUSB_KEY_ENDPOINT, ep )
                || ! get_u64( msg, RSUSB_KEY_LENGTH, len ) || ! get_u64( msg, RSUSB_KEY_TIMEOUT, to ) )
                return malformed( c, "BULK keys" );
            bool out = ep <= 0xFF && ! ( ep & 0x80 );
            const uint8_t * p;
            size_t n;
            bool bad_type;
            int data_st = check_data( msg, out, len, p, n, bad_type );
            if( bad_type )
                return malformed( c, "data" );
            if( len > RSUSB_MAX_BULK_LEN || ( ep <= 0xFF && data_st ) )
                return reply( c, msg, RSUSB_ST_INVALID_PARAM );
            auto it = c->handles.find( h );
            if( it == c->handles.end() || it->second->closing )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            auto m = it->second;
            if( m->dev->gone )
                return reply( c, msg, RSUSB_ST_NO_DEVICE );
            auto e = find_endpoint( *m, ep );
            if( ! e )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            uint8_t attr = ( *e )[3];
            if( attr != 2 && attr != 3 )
                return reply( c, msg, RSUSB_ST_INVALID_PARAM );
            if( ! sync_quota( c ) )
                return reply( c, msg, RSUSB_ST_NO_MEM );
            if( out && len && hwm_denied( hwm, p, size_t( len ), false, 0, 0 ) )
            {
                hwm_log( c, p[RSUSB_HWM_OPCODE_OFFSET] );
                return reply( c, msg, RSUSB_ST_ACCESS );
            }
            auto op = new sync_op;
            op->m = m;
            op->in = ! out;
            op->msg = xpc_retain( msg );
            op->buf.resize( size_t( len ) );
            if( out && len )
                std::copy( p, p + len, op->buf.begin() );
            submit_sync( c, m, op, attr == 2 ? usb_transfer::bulk : usb_transfer::interrupt, uint8_t( ep ), to );
        }

        void do_reset_ep( const std::shared_ptr< connection > & c, xpc_object_t msg )
        {
            uint64_t h, ep;
            if( ! get_u64( msg, RSUSB_KEY_HANDLE, h ) || ! get_u64( msg, RSUSB_KEY_ENDPOINT, ep ) )
                return malformed( c, "RESET_EP keys" );
            auto it = c->handles.find( h );
            if( it == c->handles.end() || it->second->closing )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            auto m = it->second;
            if( m->dev->gone )
                return reply( c, msg, RSUSB_ST_NO_DEVICE );
            if( ! find_endpoint( *m, ep ) )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            if( ! sync_quota( c ) )
                return reply( c, msg, RSUSB_ST_NO_MEM );
            ++c->sync_ops;
            ++m->inflight;
            auto done = std::make_shared< once >();
            xpc_retain( msg );
            auto self = shared_from_this();
            auto finish = [self, c, m, msg, done]( int st ) {
                async( c->q, [self, c, m, msg, done, st] {
                    if( ! done->claim() )
                        return;
                    --c->sync_ops;
                    --m->inflight;
                    if( c->st != connection::st_t::dead )
                        self->reply( c, msg, st );
                    xpc_release( msg );
                    self->drained( m );
                } );
            };
            async( m->dev->q, [self, m, ep, finish] {
                int r = m->dev->gone || m->backend_closed
                    ? RSUSB_ST_NO_DEVICE
                    : self->watched( m->dev, [finish] { finish( RSUSB_ST_NO_DEVICE ); }, [&] { return self->be->clear_halt( m->bh, uint8_t( ep ) ); } );
                finish( r );
            } );
        }

        // ---------------------------------------------------------------------------------------------------------
        // Requests (2.6.4 - 2.6.7)

        std::map< uint64_t, std::shared_ptr< request > >::iterator erase_request( const std::shared_ptr< connection > & c,
                                                                                   std::map< uint64_t, std::shared_ptr< request > >::iterator it )
        {
            destroy_request( it->second );
            return c->requests.erase( it );
        }

        void destroy_request( const std::shared_ptr< request > & q )
        {
            q->in_table = false;
            if( q->inflight )
                watched( q->m->dev, nullptr, [&] { return be->cancel( q->x ); } );
        }

        void cancel_if_inflight( const std::shared_ptr< request > & q, messenger * m )
        {
            if( q->m.get() == m && q->inflight )
                watched( q->m->dev, nullptr, [&] { return be->cancel( q->x ); } );
        }

        std::shared_ptr< request > find_request( const std::shared_ptr< connection > & c, xpc_object_t msg, bool & ok )
        {
            uint64_t r;
            ok = get_u64( msg, RSUSB_KEY_REQUEST_ID, r );
            auto it = ok ? c->requests.find( r ) : c->requests.end();
            return it == c->requests.end() ? nullptr : it->second;
        }

        void do_req_create( const std::shared_ptr< connection > & c, xpc_object_t msg )
        {
            uint64_t h, ep, len;
            if( ! get_u64( msg, RSUSB_KEY_HANDLE, h ) || ! get_u64( msg, RSUSB_KEY_ENDPOINT, ep ) || ! get_u64( msg, RSUSB_KEY_LENGTH, len ) )
                return malformed( c, "REQ_CREATE keys" );
            if( len < 1 || len > RSUSB_MAX_REQUEST_LEN )
                return reply( c, msg, RSUSB_ST_INVALID_PARAM );
            auto it = c->handles.find( h );
            if( it == c->handles.end() || it->second->closing )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            auto m = it->second;
            if( m->dev->gone )
                return reply( c, msg, RSUSB_ST_NO_DEVICE );
            auto e = find_endpoint( *m, ep );
            if( ! e )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            if( ! ( ep & 0x80 ) || ( ( *e )[3] != 2 && ( *e )[3] != 3 ) )
                return reply( c, msg, RSUSB_ST_INVALID_PARAM );
            uint64_t page = uint64_t( getpagesize() );
            uint64_t size = ( len + page - 1 ) / page * page;
            unsigned units = lease_units_locked( *c );
            if( c->requests.size() >= RSUSB_QUOTA_REQUESTS_PER_DEVICE * units || c->shm + size > RSUSB_QUOTA_SHM_PER_DEVICE * units )
                return reply( c, msg, RSUSB_ST_NO_MEM );
            uint64_t prev = shm_total.fetch_add( size );
            if( prev + size > shm_limit )
            {
                shm_total -= size;
                return reply( c, msg, RSUSB_ST_NO_MEM );
            }
            ++requests_live;
            c->shm += size;
            auto q = std::make_shared< request >();
            q->owner = this;
            q->c = c;
            q->m = m;
            q->len = uint32_t( len );
            q->map_len = size_t( size );
            void * map = mmap( nullptr, size_t( size ), PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0 );
            if( map == MAP_FAILED )
                return reply( c, msg, RSUSB_ST_NO_MEM );
            q->map = map;
            q->shm = xpc_shmem_create( map, size_t( size ) );
            if( ! q->shm )
                return reply( c, msg, RSUSB_ST_NO_MEM );
            q->x = be->alloc_transfer();
            q->x->kind = ( *e )[3] == 2 ? usb_transfer::bulk : usb_transfer::interrupt;
            q->x->handle = m->bh;
            q->x->endpoint = uint8_t( ep );
            q->x->buffer = static_cast< uint8_t * >( map );
            q->x->length = uint32_t( len );
            q->x->timeout_ms = 0;
            request * raw = q.get();
            auto d = m->dev;
            q->x->done = [this, raw, d]( int xs, uint32_t n ) {
                if( xs == RSUSB_XS_NO_DEVICE )
                    device_lost( d, "transfer completed with NO_DEVICE" );
                auto conn = raw->c;
                async( conn->q, [this, raw, xs, n] { req_done( raw, xs, n ); } );
            };
            q->r = c->next_r++;
            c->requests[q->r] = q;
            uint64_t rid = q->r;
            xpc_object_t shm = q->shm;
            reply( c, msg, 0, [rid, shm]( xpc_object_t r ) {
                xpc_dictionary_set_uint64( r, RSUSB_KEY_REQUEST_ID, rid );
                xpc_dictionary_set_value( r, RSUSB_KEY_SHMEM, shm );
            } );
        }

        void req_done( request * q, int xs, uint32_t n )
        {
            auto keep = std::move( q->self );   // may be the last reference
            auto c = q->c;
            q->inflight = false;
            c->inflight -= q->len;
            inflight_total -= q->len;
            --q->m->inflight;
            if( q->in_table && c->st != connection::st_t::dead )
            {
                xpc_object_t e = xpc_dictionary_create( nullptr, nullptr, 0 );
                xpc_dictionary_set_uint64( e, RSUSB_KEY_EVENT, RSUSB_EV_REQ_DONE );
                xpc_dictionary_set_uint64( e, RSUSB_KEY_REQUEST_ID, q->r );
                xpc_dictionary_set_uint64( e, RSUSB_KEY_TRANSFER_STATUS, uint64_t( xs ) );
                xpc_dictionary_set_uint64( e, RSUSB_KEY_TRANSFERRED, std::min( n, q->len ) );
                send( c, e );
                xpc_release( e );
            }
            auto m = q->m;
            keep.reset();
            drained( m );
        }

        void do_req_submit( const std::shared_ptr< connection > & c, xpc_object_t msg )
        {
            bool ok;
            auto r = find_request( c, msg, ok );
            if( ! ok )
                return malformed( c, "r" );
            if( ! r || r->m->closing )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            auto & q = r;
            if( q->m->dev->gone )
                return reply( c, msg, RSUSB_ST_NO_DEVICE );
            if( q->inflight )
                return reply( c, msg, RSUSB_ST_BUSY );
            unsigned units = lease_units_locked( *c );
            if( c->inflight + q->len > RSUSB_QUOTA_INFLIGHT_PER_DEVICE * units )
                return reply( c, msg, RSUSB_ST_NO_MEM );
            if( inflight_total.fetch_add( q->len ) + q->len > RSUSB_MAX_INFLIGHT_TOTAL )
            {
                inflight_total -= q->len;
                return reply( c, msg, RSUSB_ST_NO_MEM );
            }
            q->inflight = true;
            q->self = r;
            c->inflight += q->len;
            ++q->m->inflight;
            int st = watched( q->m->dev, nullptr, [&] { return be->submit( q->x ); } );
            if( st )
            {
                q->inflight = false;
                q->self.reset();
                c->inflight -= q->len;
                inflight_total -= q->len;
                --q->m->inflight;
            }
            reply( c, msg, st );
            if( st )
                drained( q->m );
        }

        void do_req_cancel( const std::shared_ptr< connection > & c, xpc_object_t msg )
        {
            bool ok;
            auto r = find_request( c, msg, ok );
            if( ! ok )
                return malformed( c, "r" );
            if( ! r )
                return reply( c, msg, RSUSB_ST_NOT_FOUND );
            auto & q = r;
            if( q->m->dev->gone )
                return reply( c, msg, RSUSB_ST_NO_DEVICE );
            int st = q->inflight ? watched( q->m->dev, nullptr, [&] { return be->cancel( q->x ); } ) : int( RSUSB_ST_NOT_FOUND );
            reply( c, msg, st );
        }

        void do_req_destroy( const std::shared_ptr< connection > & c, xpc_object_t msg )
        {
            uint64_t r;
            if( ! get_u64( msg, RSUSB_KEY_REQUEST_ID, r ) )
                return malformed( c, "r" );
            auto it = c->requests.find( r );
            if( it != c->requests.end() )
                erase_request( c, it );
        }

        // ---------------------------------------------------------------------------------------------------------
        // Sweep, shutdown, stats

        void sweep()
        {
            sync( reg_q, [this] { refresh(); } );
            std::vector< captured_entry > recorded;
            states.read( recorded );
            auto live = be->live_devices();
            auto selected = sweep_select( recorded, live );
            states.set_entries( selected );   // only recorded devices still captured are kept
            for( auto & e : selected )
            {
                std::shared_ptr< device > d;
                handback_target t;
                t.registry_id = e.registry_id;
                t.vid = e.vid;
                t.pid = e.pid;
                for( auto & l : live )
                    if( l.registry_id == e.registry_id )
                        t.bus = uint8_t( l.location >> 24 ), t.address = l.address;
                {
                    std::lock_guard< std::mutex > lock( mu );
                    for( auto & x : devices )
                        if( x.second->registry_id == e.registry_id && x.second->state == dstate::listed )
                        {
                            d = x.second;
                            d->captured = true;
                            if( ! d->anchor && d->leases.empty() && ! d->messengers )
                                d->state = dstate::handing_back;
                        }
                }
                if( d )
                {
                    bool busy;
                    {
                        std::lock_guard< std::mutex > lock( mu );
                        busy = d->state != dstate::handing_back;   // already in use by a client: its grace hands back
                    }
                    if( ! busy )
                        sync( d->q, [&] { hand_back( d, t, true ); } );
                }
                else if( ! be->interfaces_held_elsewhere( t.registry_id )
                         && be->hand_back( t, tm.handback_poll_ms, tm.handback_wait_ms ) == 0 )
                    states.remove( t.registry_id );
                log_info( "device", "startup sweep: %04x:%04x processed", e.vid, e.pid );
            }
            std::lock_guard< std::mutex > lock( mu );
            sweep_done = true;
        }

        void shutdown()
        {
            std::vector< std::shared_ptr< connection > > cs;
            {
                std::lock_guard< std::mutex > lock( mu );
                stopping = true;
                for( auto & c : conns )
                    cs.push_back( c.second );
            }
            if( listener )
                xpc_connection_cancel( listener );
            auto self = shared_from_this();
            for( auto & c : cs )
            {
                cancel( c );
                async( c->q, [self, c] {   // control transfers already in flight are cancelled too
                    for( auto & h : c->handles )
                        for( sync_op * op : h.second->ops )
                            self->be->cancel( op->x );
                } );
            }
            for( int i = 0; i < 150; ++i )
            {
                {
                    std::lock_guard< std::mutex > lock( mu );
                    int open = 0;
                    for( auto & d : devices )
                        open += d.second->messengers;
                    if( ! open && conns.empty() )
                        break;
                }
                usleep( 20000 );
            }
            std::vector< std::shared_ptr< device > > ds;
            {
                std::lock_guard< std::mutex > lock( mu );
                for( auto & d : devices )
                    ds.push_back( d.second );
            }
            for( auto & d : ds )
            {
                uint64_t anchor;
                bool captured;
                handback_target t;
                {
                    std::lock_guard< std::mutex > lock( mu );
                    cancel_grace( *d );
                    if( d->state != dstate::listed )
                        continue;
                    d->state = dstate::handing_back;
                    anchor = d->anchor;
                    d->anchor = 0;
                    captured = d->captured;
                    t = target( *d );
                }
                if( anchor )
                    be->close_anchor( anchor );
                if( captured )
                    hand_back( d, t, false );
            }
            be->stop();
        }

        bool idle()
        {
            std::lock_guard< std::mutex > lock( mu );
            if( ! conns.empty() || ! sweep_done || ! watches.empty() )
                return false;
            for( auto & d : devices )
                if( d.second->messengers || d.second->anchor || d.second->grace_armed
                    || d.second->state == dstate::capturing || d.second->state == dstate::handing_back )
                    return false;
            return true;
        }

        core_stats stats()
        {
            core_stats s;
            std::lock_guard< std::mutex > lock( mu );
            s.connections = unsigned( conns.size() );
            s.ready = ready;
            for( auto & e : devices )
            {
                auto & d = *e.second;
                s.devices += d.state == dstate::listed;
                s.anchors += d.anchor != 0;
                s.messengers += unsigned( d.messengers );
                s.claimed += unsigned( d.owners.size() );
                s.leases += unsigned( d.leases.size() );
                s.grace_timers += d.grace_armed;
                s.captured += d.captured;
            }
            s.shm_bytes = shm_total;
            s.inflight_bytes = inflight_total;
            s.requests = unsigned( requests_live );
            return s;
        }
    };

    namespace
    {
        request::~request()
        {
            if( x )
                owner->be->free_transfer( x );
            if( shm )
                xpc_release( shm );
            if( map )
                munmap( map, map_len );
            c->shm -= map_len;
            owner->shm_total -= map_len;
            --owner->requests_live;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------

    usbd_core::usbd_core( std::unique_ptr< usb_backend > backend, std::unique_ptr< peer_policy > policy, const config & cfg,
                          const timing & t, const paths & p )
        : _impl( std::make_shared< impl >( std::move( backend ), std::move( policy ), cfg, t, p ) )
    {
    }

    usbd_core::~usbd_core() = default;

    void usbd_core::start( bool bg ) { _impl->start( bg ); }

    void usbd_core::attach_listener( xpc_connection_t listener )
    {
        _impl->listener = listener;
        xpc_retain( listener );
        std::weak_ptr< impl > weak = _impl;
        auto * h = new std::weak_ptr< impl >( weak );
        xpc_connection_set_event_handler( listener, ^( xpc_object_t peer ) {
            auto s = h->lock();
            if( s && xpc_get_type( peer ) == XPC_TYPE_CONNECTION )
                s->accept( static_cast< xpc_connection_t >( peer ) );
        } );
        xpc_connection_resume( listener );
    }

    void usbd_core::shutdown() { _impl->shutdown(); }
    bool usbd_core::idle() { return _impl->idle(); }
    core_stats usbd_core::stats() { return _impl->stats(); }
    void usbd_core::sweep() { _impl->sweep(); }
    usb_backend & usbd_core::backend() { return *_impl->be; }
    peer_policy & usbd_core::policy() { return *_impl->pol; }

    std::shared_ptr< session > usbd_core::open_session( std::function< void( xpc_object_t ) > sink,
                                                        std::function< void( std::function< void() > ) > barrier )
    {
        auto c = std::make_shared< session >();
        c->sink = std::move( sink );
        c->barrier = std::move( barrier );
        c->euid = _impl->pol->peer_uid( nullptr );
        c->q = impl::interactive_queue( "usbd.session" );
        std::lock_guard< std::mutex > lock( _impl->mu );
        c->id = _impl->next_conn++;
        _impl->conns[c->id] = c;
        ++_impl->new_per_uid[c->euid];
        return c;
    }

    void usbd_core::session_message( const std::shared_ptr< session > & s, xpc_object_t msg )
    {
        auto i = _impl;
        std::shared_ptr< connection > c = s;
        sync( s->q, [i, c, msg] {
            if( c->st != connection::st_t::dead )
                i->handle( c, msg );
        } );
    }

    void usbd_core::close_session( const std::shared_ptr< session > & s )
    {
        auto i = _impl;
        std::shared_ptr< connection > c = s;
        sync( s->q, [i, c] { i->connection_end( c ); } );
    }
}
