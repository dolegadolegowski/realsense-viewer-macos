// License: Apache 2.0. See LICENSE file in root directory.
// Hardware self-test for RealSense D400 cameras on macOS (Apple Silicon).
// Exercises the code paths used by RealSense Viewer: enumeration, hardware monitor, options,
// depth / infrared / color streaming, IMU (HID) streaming and repeated sensor power cycles.

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

#include <unistd.h>
#include <cstdio>

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
}  // namespace

int main( int argc, char ** argv )
try
{
    double seconds = argc > 1 ? std::atof( argv[1] ) : 5.0;
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
