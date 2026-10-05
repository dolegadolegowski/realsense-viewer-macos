// License: Apache 2.0. See LICENSE file in root directory.
// Hardware self-test for RealSense D400 cameras on macOS (Apple Silicon).
// Exercises the code paths used by RealSense Viewer: enumeration, hardware monitor, options,
// depth / infrared / color streaming, IMU (HID) streaming and repeated sensor power cycles.
// With --stress it also hammers the paths that used to hang or crash: start/stop cycles, recording toggled while
// streaming, presets and options changed while streaming, a long run of all sensors, context teardown and a
// hardware reset.

#include <librealsense2/rs.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include <unistd.h>
#include <mach/mach.h>

namespace
{
int g_failures = 0;

void report( bool ok, const std::string & what )
{
    std::printf( "[%s] %s\n", ok ? " OK " : "FAIL", what.c_str() );
    std::fflush( stdout );
    if( ! ok )
        ++g_failures;
}

std::string info( const rs2::device & dev, rs2_camera_info i )
{
    return dev.supports( i ) ? dev.get_info( i ) : "n/a";
}

struct counter
{
    std::mutex m;
    std::map< std::string, int > frames;
    void add( const rs2::frame & f )
    {
        std::lock_guard< std::mutex > l( m );
        frames[f.get_profile().stream_name()]++;
    }
};

// Streams the given profiles on a sensor for `seconds` and returns frames per stream.
std::map< std::string, int > stream_sensor( rs2::sensor & s, const std::vector< rs2::stream_profile > & profiles, double seconds )
{
    counter c;
    s.open( profiles );
    s.start( [&]( rs2::frame f ) {
        if( auto fs = f.as< rs2::frameset >() )
            for( auto && sub : fs )
                c.add( sub );
        else
            c.add( f );
    } );
    std::this_thread::sleep_for( std::chrono::duration< double >( seconds ) );
    s.stop();
    s.close();
    return c.frames;
}

rs2::stream_profile find_profile( const rs2::sensor & s, rs2_stream stream, int index, rs2_format fmt, int w, int h, int fps )
{
    for( auto && p : s.get_stream_profiles() )
    {
        if( p.stream_type() != stream || p.format() != fmt || p.fps() != fps )
            continue;
        if( index >= 0 && p.stream_index() != index )
            continue;
        if( auto vp = p.as< rs2::video_stream_profile >() )
        {
            if( vp.width() == w && vp.height() == h )
                return p;
        }
        else if( w == 0 )
            return p;
    }
    return {};
}

void check_rate( const std::map< std::string, int > & frames, const std::string & stream, double seconds, int fps, double min_ratio )
{
    auto it = frames.find( stream );
    int n = it == frames.end() ? 0 : it->second;
    double rate = n / seconds;
    char buf[256];
    std::snprintf( buf, sizeof( buf ), "%-8s %5d frames in %.0fs = %6.1f fps (expected ~%d)", stream.c_str(), n, seconds, rate, fps );
    report( rate >= fps * min_ratio, buf );
}

double seconds_since( std::chrono::steady_clock::time_point t0 )
{
    return std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count();
}

double resident_mb()
{
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    task_info( mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count );
    return info.resident_size / 1048576.0;
}

void run_stress( rs2::context & ctx, rs2::device & dev, rs2::sensor & depth_sensor, rs2::sensor & color_sensor,
                 rs2::sensor & motion_sensor )
{
    std::printf( "\nStress tests\n" );
    auto dp = find_profile( depth_sensor, RS2_STREAM_DEPTH, -1, RS2_FORMAT_Z16, 848, 480, 30 );
    auto cp = find_profile( color_sensor, RS2_STREAM_COLOR, -1, RS2_FORMAT_RGB8, 1280, 720, 30 );

    // Start/stop cycles: every start must deliver frames and every stop must return promptly
    {
        int cycles = 20, without_frames = 0;
        double slowest_stop = 0;
        for( int i = 0; i < cycles; ++i )
        {
            std::atomic< int > n{ 0 };
            depth_sensor.open( dp );
            depth_sensor.start( [&]( rs2::frame ) { n++; } );
            std::this_thread::sleep_for( std::chrono::milliseconds( 700 ) );
            auto t0 = std::chrono::steady_clock::now();
            depth_sensor.stop();
            depth_sensor.close();
            slowest_stop = std::max( slowest_stop, seconds_since( t0 ) );
            without_frames += n == 0;
        }
        char buf[200];
        std::snprintf( buf, sizeof( buf ), "%d depth start/stop cycles (%d without frames, slowest stop %.2f s)", cycles,
                       without_frames, slowest_stop );
        report( without_frames == 0 && slowest_stop < 2.0, buf );
    }

    // Recording toggled while depth and color stream: the recorder swaps the live frame callbacks every time
    {
        char dir_template[] = "/tmp/rs-stress-XXXXXX";
        std::string dir = mkdtemp( dir_template ) ? dir_template : "/tmp";
        std::atomic< int > depth_frames{ 0 }, color_frames{ 0 };
        depth_sensor.open( dp );
        color_sensor.open( cp );
        depth_sensor.start( [&]( rs2::frame ) { depth_frames++; } );
        color_sensor.start( [&]( rs2::frame ) { color_frames++; } );
        int toggles = 30;
        for( int i = 0; i < toggles; ++i )
        {
            std::string file = dir + "/toggle-" + std::to_string( i ) + ".db3";
            {
                rs2::recorder rec( file, dev );
                std::this_thread::sleep_for( std::chrono::milliseconds( 150 + 50 * ( i % 4 ) ) );
            }
            std::remove( file.c_str() );
        }
        int d0 = depth_frames, c0 = color_frames;
        std::this_thread::sleep_for( std::chrono::seconds( 2 ) );
        int d = depth_frames - d0, c = color_frames - c0;
        depth_sensor.stop();
        color_sensor.stop();
        depth_sensor.close();
        color_sensor.close();
        rmdir( dir.c_str() );
        // The recorder registered for notifications, which turns on error polling: it would keep powering this device
        // object's depth sensor while the pipeline below streams through another one
        if( depth_sensor.supports( RS2_OPTION_ERROR_POLLING_ENABLED ) )
            depth_sensor.set_option( RS2_OPTION_ERROR_POLLING_ENABLED, 0 );
        report( d > 45 && c > 45, std::to_string( toggles ) + " recording start/stops while streaming, streams still alive (depth "
                                      + std::to_string( d ) + ", color " + std::to_string( c ) + " frames in 2 s)" );
    }

    // Presets and options changed while all video streams run, as when clicking through the viewer's controls
    if( depth_sensor.supports( RS2_OPTION_VISUAL_PRESET ) )
    {
        std::atomic< int > depth_frames{ 0 }, color_frames{ 0 };
        depth_sensor.open( dp );
        color_sensor.open( cp );
        depth_sensor.start( [&]( rs2::frame ) { depth_frames++; } );
        color_sensor.start( [&]( rs2::frame ) { color_frames++; } );
        auto range = depth_sensor.get_option_range( RS2_OPTION_VISUAL_PRESET );
        float original = depth_sensor.get_option( RS2_OPTION_VISUAL_PRESET );
        int errors = 0;
        std::string first_error;
        auto t0 = std::chrono::steady_clock::now();
        for( int round = 0; round < 3; ++round )
            for( float v = range.min; v <= range.max; v += range.step )
            {
                try
                {
                    depth_sensor.set_option( RS2_OPTION_VISUAL_PRESET, v );
                    if( depth_sensor.supports( RS2_OPTION_LASER_POWER ) )
                        depth_sensor.set_option( RS2_OPTION_LASER_POWER, 30.f * ( 1 + ( (int)v % 5 ) ) );
                    if( color_sensor.supports( RS2_OPTION_EXPOSURE ) )
                        color_sensor.get_option( RS2_OPTION_EXPOSURE );
                }
                catch( const std::exception & e )
                {
                    if( errors++ == 0 )
                        first_error = e.what();
                }
            }
        double took = seconds_since( t0 );
        depth_sensor.set_option( RS2_OPTION_VISUAL_PRESET, original );
        int d0 = depth_frames, c0 = color_frames;
        std::this_thread::sleep_for( std::chrono::seconds( 2 ) );
        int d = depth_frames - d0, c = color_frames - c0;
        depth_sensor.stop();
        color_sensor.stop();
        depth_sensor.close();
        color_sensor.close();
        char buf[300];
        std::snprintf( buf, sizeof( buf ), "presets/options x3 while streaming in %.1f s (%d errors%s%s), then depth %d, color %d frames in 2 s",
                       took, errors, errors ? ": " : "", first_error.c_str(), d, c );
        report( errors == 0 && d > 45 && c > 45, buf );
    }

    // Long run of every sensor at once: steady frame rates and no memory growth
    {
        double run = 60;
        rs2::pipeline pipe( ctx );
        rs2::config cfg;
        cfg.enable_device( info( dev, RS2_CAMERA_INFO_SERIAL_NUMBER ) );
        cfg.enable_stream( RS2_STREAM_DEPTH, 848, 480, RS2_FORMAT_Z16, 30 );
        cfg.enable_stream( RS2_STREAM_INFRARED, 1, 848, 480, RS2_FORMAT_Y8, 30 );
        cfg.enable_stream( RS2_STREAM_COLOR, 1280, 720, RS2_FORMAT_RGB8, 30 );
        if( motion_sensor )
        {
            cfg.enable_stream( RS2_STREAM_ACCEL );
            cfg.enable_stream( RS2_STREAM_GYRO );
        }
        counter c;
        pipe.start( cfg, [&]( rs2::frame f ) {
            if( auto fs = f.as< rs2::frameset >() )
                for( auto && sub : fs )
                    c.add( sub );
            else
                c.add( f );
        } );
        std::this_thread::sleep_for( std::chrono::seconds( 10 ) );  // let allocations settle before measuring
        double mb0 = resident_mb();
        std::map< std::string, int > f0;
        {
            std::lock_guard< std::mutex > l( c.m );
            f0 = c.frames;
        }
        std::this_thread::sleep_for( std::chrono::duration< double >( run ) );
        std::map< std::string, int > frames;
        {
            std::lock_guard< std::mutex > l( c.m );
            for( auto & kv : c.frames )
                frames[kv.first] = kv.second - f0[kv.first];
        }
        double mb1 = resident_mb();
        pipe.stop();
        std::printf( "  all sensors for %.0f s:\n", run );
        check_rate( frames, "Depth", run, 30, 0.9 );
        check_rate( frames, "Infrared 1", run, 30, 0.9 );
        check_rate( frames, "Color", run, 30, 0.9 );
        if( motion_sensor )
            report( frames["Accel"] > run * 50 && frames["Gyro"] > run * 100,
                    "IMU over the whole run (accel " + std::to_string( frames["Accel"] ) + ", gyro " + std::to_string( frames["Gyro"] ) + ")" );
        char buf[200];
        std::snprintf( buf, sizeof( buf ), "memory over the run %.1f -> %.1f MB", mb0, mb1 );
        report( mb1 - mb0 < 50, buf );
    }

    // Context teardown used to wait out the device watcher's 2 s poll
    {
        auto t0 = std::chrono::steady_clock::now();
        {
            rs2::context c2;
            c2.query_devices();
        }
        char buf[100];
        std::snprintf( buf, sizeof( buf ), "context create, query and destroy in %.2f s", seconds_since( t0 ) );
        report( seconds_since( t0 ) < 1.5, buf );
    }

    // Hardware reset: the camera drops off USB and comes back, and must stream again
    {
        std::string serial = info( dev, RS2_CAMERA_INFO_SERIAL_NUMBER );
        dev.hardware_reset();
        auto t0 = std::chrono::steady_clock::now();
        bool gone = false;
        rs2::device again;
        while( seconds_since( t0 ) < 20 && ! again )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
            rs2::device found;
            try
            {
                for( auto && d : ctx.query_devices() )
                    if( info( d, RS2_CAMERA_INFO_SERIAL_NUMBER ) == serial )
                        found = d;
            }
            catch( const rs2::error & )
            {
                // still listed while it drops off USB, so creating it fails
            }
            gone = gone || ! found;
            if( gone && found )
                again = found;
        }
        report( (bool)again, "hardware reset: camera back after " + std::to_string( (int)seconds_since( t0 ) ) + " s" );
        if( again )
        {
            rs2::sensor ds;
            for( auto && s : again.query_sensors() )
                if( s.is< rs2::depth_sensor >() )
                    ds = s;
            auto p = find_profile( ds, RS2_STREAM_DEPTH, -1, RS2_FORMAT_Z16, 848, 480, 30 );
            check_rate( stream_sensor( ds, { p }, 3.0 ), "Depth", 3.0, 30, 0.8 );
        }
    }
}
}  // namespace

int main( int argc, char ** argv )
try
{
    bool stress = false;
    double seconds = 5.0;
    for( int i = 1; i < argc; ++i )
        if( std::string( argv[i] ) == "--stress" )
            stress = true;
        else
            seconds = std::atof( argv[i] );
    std::printf( "RealSense macOS self-test (librealsense %s, euid=%d)\n", RS2_API_FULL_VERSION_STR, (int)geteuid() );
    if( geteuid() != 0 )
        std::printf( "WARNING: not running as root - macOS needs administrator privileges for USB camera access\n" );

    rs2::log_to_console( RS2_LOG_SEVERITY_WARN );
    rs2::context ctx;
    auto list = ctx.query_devices();
    report( list.size() > 0, "device enumeration (" + std::to_string( list.size() ) + " device(s))" );
    if( list.size() == 0 )
        return 1;

    rs2::device dev = list[0];
    std::printf( "  Name: %s\n  Serial: %s\n  Firmware: %s (recommended %s)\n  USB: %s\n  Product line: %s\n",
                 info( dev, RS2_CAMERA_INFO_NAME ).c_str(), info( dev, RS2_CAMERA_INFO_SERIAL_NUMBER ).c_str(),
                 info( dev, RS2_CAMERA_INFO_FIRMWARE_VERSION ).c_str(),
                 info( dev, RS2_CAMERA_INFO_RECOMMENDED_FIRMWARE_VERSION ).c_str(),
                 info( dev, RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR ).c_str(),
                 info( dev, RS2_CAMERA_INFO_PRODUCT_LINE ).c_str() );
    report( dev.supports( RS2_CAMERA_INFO_FIRMWARE_VERSION ), "hardware monitor (firmware version query)" );

    // Hardware monitor round trip through a raw debug command (GVD)
    if( auto dbg = dev.as< rs2::debug_protocol >() )
    {
        auto cmd = dbg.build_command( 0x10 /* GVD */ );
        auto res = dbg.send_and_receive_raw_data( cmd );
        report( res.size() > 4, "debug protocol raw command (GVD, " + std::to_string( res.size() ) + " bytes)" );
    }

    rs2::sensor depth_sensor, color_sensor, motion_sensor;
    for( auto && s : dev.query_sensors() )
    {
        std::printf( "  Sensor: %s (%zu profiles)\n", s.get_info( RS2_CAMERA_INFO_NAME ), s.get_stream_profiles().size() );
        if( s.is< rs2::depth_sensor >() )
            depth_sensor = s;
        else if( s.is< rs2::color_sensor >() )
            color_sensor = s;
        else if( s.is< rs2::motion_sensor >() )
            motion_sensor = s;
    }
    report( (bool)depth_sensor, "stereo module present" );
    report( (bool)color_sensor, "RGB camera present" );
    report( (bool)motion_sensor, "motion module (IMU) present" );

    // Parallel access to all sensors, like the viewer does when it builds its device panel
    {
        std::atomic< int > errors{ 0 };
        std::string first_error;
        std::mutex error_mutex;
        std::vector< std::thread > threads;
        for( auto s : dev.query_sensors() )
            threads.emplace_back( [&, s]() mutable {
                for( int i = 0; i < 10; ++i )
                {
                    try
                    {
                        s.get_stream_profiles();
                        for( auto opt : s.get_supported_options() )
                            if( ! s.is_option_read_only( opt ) )
                                s.get_option( opt );
                    }
                    catch( const std::exception & e )
                    {
                        std::lock_guard< std::mutex > l( error_mutex );
                        if( errors++ == 0 )
                            first_error = e.what();
                    }
                }
            } );
        for( auto & t : threads )
            t.join();
        report( errors == 0, "parallel profile/option queries on all sensors" + ( errors ? " (" + first_error + ")" : std::string() ) );
    }

    // Visual presets (the viewer's "Preset" list) - each is a batch of advanced-mode commands
    if( depth_sensor && depth_sensor.supports( RS2_OPTION_VISUAL_PRESET ) )
    {
        auto range = depth_sensor.get_option_range( RS2_OPTION_VISUAL_PRESET );
        float original = depth_sensor.get_option( RS2_OPTION_VISUAL_PRESET );
        bool ok = true;
        long long slowest = 0;
        std::string failed;
        for( float v = range.min; v <= range.max; v += range.step )
        {
            auto t0 = std::chrono::steady_clock::now();
            try
            {
                depth_sensor.set_option( RS2_OPTION_VISUAL_PRESET, v );
            }
            catch( const std::exception & e )
            {
                ok = false;
                failed += std::string( " " ) + depth_sensor.get_option_value_description( RS2_OPTION_VISUAL_PRESET, v ) + ": " + e.what();
            }
            auto ms = std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now() - t0 ).count();
            slowest = std::max( slowest, (long long)ms );
        }
        depth_sensor.set_option( RS2_OPTION_VISUAL_PRESET, original );
        report( ok && slowest < 10000,
                "visual presets " + std::to_string( (int)range.min ) + ".." + std::to_string( (int)range.max ) + " (slowest " + std::to_string( slowest ) + " ms)" + failed );
    }

    // Options (UVC extension units + processing units)
    if( depth_sensor && depth_sensor.supports( RS2_OPTION_LASER_POWER ) )
    {
        auto r = depth_sensor.get_option_range( RS2_OPTION_LASER_POWER );
        float old = depth_sensor.get_option( RS2_OPTION_LASER_POWER );
        float target = std::fabs( old - 150.f ) < 1.f ? 120.f : 150.f;
        depth_sensor.set_option( RS2_OPTION_LASER_POWER, target );
        float now = depth_sensor.get_option( RS2_OPTION_LASER_POWER );
        depth_sensor.set_option( RS2_OPTION_LASER_POWER, old );
        report( std::fabs( now - target ) < r.step + 0.5f, "depth option set/get (laser power)" );
    }
    if( color_sensor && color_sensor.supports( RS2_OPTION_BRIGHTNESS ) )
    {
        float old = color_sensor.get_option( RS2_OPTION_BRIGHTNESS );
        color_sensor.set_option( RS2_OPTION_BRIGHTNESS, old + 1 );
        float now = color_sensor.get_option( RS2_OPTION_BRIGHTNESS );
        color_sensor.set_option( RS2_OPTION_BRIGHTNESS, old );
        report( std::fabs( now - ( old + 1 ) ) < 0.5f, "color option set/get (brightness)" );
    }

    // Stereo module: depth + both infrared streams
    if( depth_sensor )
    {
        std::vector< rs2::stream_profile > profiles = {
            find_profile( depth_sensor, RS2_STREAM_DEPTH, -1, RS2_FORMAT_Z16, 848, 480, 30 ),
            find_profile( depth_sensor, RS2_STREAM_INFRARED, 1, RS2_FORMAT_Y8, 848, 480, 30 ),
            find_profile( depth_sensor, RS2_STREAM_INFRARED, 2, RS2_FORMAT_Y8, 848, 480, 30 ) };
        bool found = profiles[0] && profiles[1] && profiles[2];
        report( found, "depth+IR 848x480@30 profiles found" );
        if( found )
        {
            auto f = stream_sensor( depth_sensor, profiles, seconds );
            check_rate( f, "Depth", seconds, 30, 0.8 );
            check_rate( f, "Infrared 1", seconds, 30, 0.8 );
            check_rate( f, "Infrared 2", seconds, 30, 0.8 );
        }
    }

    // RGB camera
    if( color_sensor )
    {
        auto p = find_profile( color_sensor, RS2_STREAM_COLOR, -1, RS2_FORMAT_RGB8, 1280, 720, 30 );
        report( (bool)p, "color 1280x720@30 RGB8 profile found" );
        if( p )
            check_rate( stream_sensor( color_sensor, { p }, seconds ), "Color", seconds, 30, 0.8 );
    }

    // IMU (HID): accel + gyro
    if( motion_sensor )
    {
        std::vector< rs2::stream_profile > profiles;
        int accel_fps = 0, gyro_fps = 0;
        for( auto && p : motion_sensor.get_stream_profiles() )
        {
            if( p.stream_type() == RS2_STREAM_ACCEL && p.fps() > accel_fps ) accel_fps = p.fps();
            if( p.stream_type() == RS2_STREAM_GYRO && p.fps() > gyro_fps ) gyro_fps = p.fps();
        }
        for( auto && p : motion_sensor.get_stream_profiles() )
            if( ( p.stream_type() == RS2_STREAM_ACCEL && p.fps() == accel_fps ) || ( p.stream_type() == RS2_STREAM_GYRO && p.fps() == gyro_fps ) )
                profiles.push_back( p );
        report( profiles.size() == 2, "IMU accel@" + std::to_string( accel_fps ) + " + gyro@" + std::to_string( gyro_fps ) + " profiles found" );
        if( profiles.size() == 2 )
        {
            auto f = stream_sensor( motion_sensor, profiles, seconds );
            check_rate( f, "Accel", seconds, accel_fps, 0.7 );
            check_rate( f, "Gyro", seconds, gyro_fps, 0.7 );
        }
    }

    // All sensors at once through the pipeline (what the viewer's default configuration does)
    {
        rs2::pipeline pipe( ctx );
        rs2::config cfg;
        cfg.enable_device( info( dev, RS2_CAMERA_INFO_SERIAL_NUMBER ) );
        cfg.enable_stream( RS2_STREAM_DEPTH, 848, 480, RS2_FORMAT_Z16, 30 );
        cfg.enable_stream( RS2_STREAM_COLOR, 1280, 720, RS2_FORMAT_RGB8, 30 );
        if( motion_sensor )
        {
            cfg.enable_stream( RS2_STREAM_ACCEL );
            cfg.enable_stream( RS2_STREAM_GYRO );
        }
        counter c;
        pipe.start( cfg, [&]( rs2::frame f ) {
            if( auto fs = f.as< rs2::frameset >() )
                for( auto && sub : fs )
                    c.add( sub );
            else
                c.add( f );
        } );
        std::this_thread::sleep_for( std::chrono::duration< double >( seconds ) );
        pipe.stop();
        std::printf( "  pipeline (depth+color%s, all sensors concurrently):\n", motion_sensor ? "+IMU" : "" );
        check_rate( c.frames, "Depth", seconds, 30, 0.8 );
        check_rate( c.frames, "Color", seconds, 30, 0.8 );
        if( motion_sensor )
        {
            report( c.frames["Accel"] > 0 && c.frames["Gyro"] > 0,
                    "IMU frames alongside video (accel " + std::to_string( c.frames["Accel"] ) + ", gyro " + std::to_string( c.frames["Gyro"] ) + ")" );
        }
    }

    // Recording to a file and playing it back (Viewer "Record" button / "Add Source > Load Recorded Sequence")
    {
        char dir_template[] = "/tmp/rs-selftest-XXXXXX";
        std::string dir = mkdtemp( dir_template ) ? dir_template : "/tmp";
        std::string file = dir + "/selftest.db3";
        {
            rs2::pipeline pipe( ctx );
            rs2::config cfg;
            cfg.enable_device( info( dev, RS2_CAMERA_INFO_SERIAL_NUMBER ) );
            cfg.enable_stream( RS2_STREAM_DEPTH, 640, 480, RS2_FORMAT_Z16, 30 );
            cfg.enable_stream( RS2_STREAM_COLOR, 640, 480, RS2_FORMAT_RGB8, 30 );
            cfg.enable_record_to_file( file );
            pipe.start( cfg );
            for( int i = 0; i < 60; ++i )
                pipe.wait_for_frames();
            pipe.stop();
        }
        int played = 0;
        {
            rs2::pipeline pipe( ctx );
            rs2::config cfg;
            cfg.enable_device_from_file( file, false );
            auto profile = pipe.start( cfg );
            profile.get_device().as< rs2::playback >().set_real_time( false );
            rs2::frameset fs;
            while( pipe.try_wait_for_frames( &fs, 2000 ) )
                played++;
            pipe.stop();
        }
        report( played > 30, "record to " + file + " and play back (" + std::to_string( played ) + " framesets)" );
        std::remove( file.c_str() );
        rmdir( dir.c_str() );
    }

    // Repeated power cycles of individual sensors - must not disturb the other running sensor
    if( depth_sensor && color_sensor )
    {
        auto dp = find_profile( depth_sensor, RS2_STREAM_DEPTH, -1, RS2_FORMAT_Z16, 640, 480, 30 );
        auto cp = find_profile( color_sensor, RS2_STREAM_COLOR, -1, RS2_FORMAT_RGB8, 640, 480, 30 );
        std::atomic< int > depth_frames{ 0 };
        depth_sensor.open( dp );
        depth_sensor.start( [&]( rs2::frame ) { depth_frames++; } );
        bool ok = true;
        std::string details;
        for( int i = 0; i < 3; ++i )
        {
            int n = stream_sensor( color_sensor, { cp }, 2.0 )["Color"];
            details += " color=" + std::to_string( n );
            ok = ok && n > 30;
        }
        if( motion_sensor )
        {
            std::vector< rs2::stream_profile > one = { motion_sensor.get_stream_profiles().front() };
            for( int i = 0; i < 2; ++i )
            {
                auto f = stream_sensor( motion_sensor, one, 1.0 );
                int n = f.empty() ? 0 : f.begin()->second;
                details += " imu=" + std::to_string( n );
                ok = ok && n > 0;
            }
        }
        // starting/stopping RGB makes the firmware briefly restart the depth stream, so measure over 2 seconds
        int before = depth_frames;
        std::this_thread::sleep_for( std::chrono::seconds( 2 ) );
        int after = depth_frames - before;
        details += " depth(2s)=" + std::to_string( after );
        bool depth_alive = after > 45;
        depth_sensor.stop();
        depth_sensor.close();
        report( ok && depth_alive, "start/stop cycles of color+IMU while depth keeps streaming (" + details.substr( 1 ) + ")" );
    }

    if( stress && depth_sensor && color_sensor )
        run_stress( ctx, dev, depth_sensor, color_sensor, motion_sensor );

    std::printf( "\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures );
    return g_failures ? 1 : 0;
}
catch( const rs2::error & e )
{
    std::printf( "[FAIL] librealsense error calling %s(%s): %s\n", e.get_failed_function().c_str(), e.get_failed_args().c_str(), e.what() );
    return 2;
}
catch( const std::exception & e )
{
    std::printf( "[FAIL] %s\n", e.what() );
    return 2;
}
