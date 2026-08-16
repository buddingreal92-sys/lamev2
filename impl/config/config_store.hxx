#pragma once

#include <Windows.h>
#include <ShlObj.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>
#include <algorithm>

namespace config {

    struct settings_t {
        bool aim_enabled = true;
        bool aim_ignore_sliders = false;
        bool aim_tablet_mode = true;
        float aim_strength = 62.f;
        float aim_radius = 2.55f;
        float aim_smoothing_ms = 78.f;
        float aim_max_correction = 0.78f;
        bool aim_adaptive = false;
        float aim_adaptation_strength = 60.f;


        bool relax_enabled = true;
        float relax_ur = 70.f;
        int relax_tap_style = 1;
        int relax_primary_key = 0;
        int relax_singletap_speed_bpm = 390;
        int relax_burst_tolerance = 2;
        float relax_stamina = 90.f;
        float relax_k1_hold_center = 46.f;
        float relax_k1_hold_spread = 10.f;
        float relax_k2_hold_center = 46.f;
        float relax_k2_hold_spread = 10.f;
        float relax_hold_floor = 14.f;
        float relax_hold_ceiling = 110.f;
        int relax_manual_offset_ms = 0;
        bool relax_timing_variation = true;
        int relax_early_variation_ms = -14;
        int relax_late_variation_ms = 14;
        int relax_timing_drift_ms = 3;

        bool replay_enabled = false;
        std::string replay_path_utf8;
        bool replay_parse_buttons = true;

        bool autobot_enabled = false;
        float autobot_aim_spread = 0.15f;
        float autobot_curve_strength = 0.33f;
        float autobot_drift_amount = 1.5f;
        float autobot_momentum = 0.85f;
        float autobot_slider_laziness = 0.15f;
        float autobot_spinner_rpm = 400.f;

        bool tap_enabled = false;
        int tap_assist_window = 125;
        int tap_randomization = 15;
        bool tap_ignore_sliders = false;

        int custom_left_key = 'Z';
        int custom_right_key = 'X';
        int menu_keybind = VK_DELETE;
        bool stream_proof = false;
        std::string songs_path_utf8;

    };

    inline std::filesystem::path configs_dir( ) {
        wchar_t appdata[ MAX_PATH ]{};
        if ( SUCCEEDED( SHGetFolderPathW( nullptr, CSIDL_APPDATA, nullptr, 0, appdata ) ) ) {
            std::filesystem::path dir = std::filesystem::path( appdata ) / L"lame" / L"configs";
            std::error_code ec;
            std::filesystem::create_directories( dir, ec );
            return dir;
        }
        std::filesystem::path dir = std::filesystem::current_path( ) / "configs";
        std::error_code ec;
        std::filesystem::create_directories( dir, ec );
        return dir;
    }

    inline std::string sanitize_name( std::string name ) {
        name.erase( std::remove_if( name.begin( ), name.end( ),
            []( char c ) {
                return c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
                       c == '<' || c == '>' || c == '|' || c < 32;
            } ),
            name.end( ) );
        while ( !name.empty( ) && std::isspace( static_cast<unsigned char>( name.back( ) ) ) )
            name.pop_back( );
        size_t start = 0;
        while ( start < name.size( ) && std::isspace( static_cast<unsigned char>( name[ start ] ) ) )
            ++start;
        return name.substr( start );
    }

    inline std::filesystem::path profile_path( const std::string& name ) {
        return configs_dir( ) / ( sanitize_name( name ) + ".cfg" );
    }

    inline void write_line( std::ostream& out, const char* key, bool v ) {
        out << key << '=' << ( v ? '1' : '0' ) << '\n';
    }

    inline void write_line( std::ostream& out, const char* key, int v ) {
        out << key << '=' << v << '\n';
    }

    inline void write_line( std::ostream& out, const char* key, float v ) {
        out << key << '=' << v << '\n';
    }

    inline void write_line( std::ostream& out, const char* key, const std::string& v ) {
        out << key << '=';
        for ( char c : v ) {
            if ( c == '\n' || c == '\r' )
                continue;
            if ( c == '\\' )
                out << "\\\\";
            else if ( c == '=' )
                out << "\\e";
            else
                out << c;
        }
        out << '\n';
    }

    inline std::string unescape_value( std::string v ) {
        std::string out;
        out.reserve( v.size( ) );
        for ( size_t i = 0; i < v.size( ); ++i ) {
            if ( v[ i ] == '\\' && i + 1 < v.size( ) ) {
                if ( v[ i + 1 ] == '\\' ) {
                    out.push_back( '\\' );
                    ++i;
                }
                else if ( v[ i + 1 ] == 'e' ) {
                    out.push_back( '=' );
                    ++i;
                }
                else {
                    out.push_back( v[ i ] );
                }
            }
            else {
                out.push_back( v[ i ] );
            }
        }
        return out;
    }

    inline bool save_profile( const std::string& name, const settings_t& s ) {
        const auto path = profile_path( name );
        if ( path.empty( ) )
            return false;

        std::ofstream out( path, std::ios::trunc );
        if ( !out )
            return false;

        write_line( out, "profile_name", name );
        write_line( out, "aim.enabled", s.aim_enabled );
        write_line( out, "aim.ignore_sliders", s.aim_ignore_sliders );
        write_line( out, "aim.tablet_mode", s.aim_tablet_mode );
        write_line( out, "aim.strength", s.aim_strength );
        write_line( out, "aim.radius", s.aim_radius );
        write_line( out, "aim.smoothing_ms", s.aim_smoothing_ms );
        write_line( out, "aim.max_correction", s.aim_max_correction );
        write_line( out, "aim.adaptive", s.aim_adaptive );
        write_line( out, "aim.adaptation_strength", s.aim_adaptation_strength );


        write_line( out, "relax.enabled", s.relax_enabled );
        write_line( out, "relax.ur", s.relax_ur );
        write_line( out, "relax.tap_style", s.relax_tap_style );
        write_line( out, "relax.primary_key", s.relax_primary_key );
        write_line( out, "relax.singletap_speed_bpm", s.relax_singletap_speed_bpm );
        write_line( out, "relax.burst_tolerance", s.relax_burst_tolerance );
        write_line( out, "relax.stamina", s.relax_stamina );
        write_line( out, "relax.k1_hold_center", s.relax_k1_hold_center );
        write_line( out, "relax.k1_hold_spread", s.relax_k1_hold_spread );
        write_line( out, "relax.k2_hold_center", s.relax_k2_hold_center );
        write_line( out, "relax.k2_hold_spread", s.relax_k2_hold_spread );
        write_line( out, "relax.hold_floor", s.relax_hold_floor );
        write_line( out, "relax.hold_ceiling", s.relax_hold_ceiling );
        write_line( out, "relax.manual_offset_ms", s.relax_manual_offset_ms );
        write_line( out, "relax.timing_variation", s.relax_timing_variation );
        write_line( out, "relax.early_variation_ms", s.relax_early_variation_ms );
        write_line( out, "relax.late_variation_ms", s.relax_late_variation_ms );
        write_line( out, "relax.timing_drift_ms", s.relax_timing_drift_ms );

        write_line( out, "replay.enabled", s.replay_enabled );
        write_line( out, "replay.path", s.replay_path_utf8 );
        write_line( out, "replay.parse_buttons", s.replay_parse_buttons );

        write_line( out, "autobot.enabled", s.autobot_enabled );
        write_line( out, "autobot.aim_spread", s.autobot_aim_spread );
        write_line( out, "autobot.curve_strength", s.autobot_curve_strength );
        write_line( out, "autobot.drift_amount", s.autobot_drift_amount );
        write_line( out, "autobot.momentum", s.autobot_momentum );
        write_line( out, "autobot.slider_laziness", s.autobot_slider_laziness );
        write_line( out, "autobot.spinner_rpm", s.autobot_spinner_rpm );

        write_line( out, "tap.enabled", s.tap_enabled );
        write_line( out, "tap.assist_window", s.tap_assist_window );
        write_line( out, "tap.randomization", s.tap_randomization );
        write_line( out, "tap.ignore_sliders", s.tap_ignore_sliders );


        write_line( out, "keys.left", s.custom_left_key );
        write_line( out, "keys.right", s.custom_right_key );
        write_line( out, "keys.menu", s.menu_keybind );
        write_line( out, "system.stream_proof", s.stream_proof );
        write_line( out, "system.songs_path", s.songs_path_utf8 );
        return true;
    }

    inline bool parse_bool( const std::string& v, bool& out ) {
        if ( v == "1" || v == "true" || v == "True" || v == "yes" ) {
            out = true;
            return true;
        }
        if ( v == "0" || v == "false" || v == "False" || v == "no" ) {
            out = false;
            return true;
        }
        return false;
    }

    inline bool load_profile( const std::string& name, settings_t& s ) {
        const auto path = profile_path( name );
        std::ifstream in( path );
        if ( !in )
            return false;

        s = settings_t{};
        std::string line;
        while ( std::getline( in, line ) ) {
            if ( line.empty( ) || line[ 0 ] == '#' )
                continue;
            const auto eq = line.find( '=' );
            if ( eq == std::string::npos )
                continue;

            const std::string key = line.substr( 0, eq );
            const std::string val = unescape_value( line.substr( eq + 1 ) );

            auto parse_int = [ & ]( int& dst ) {
                try {
                    dst = std::stoi( val );
                }
                catch ( ... ) {
                }
            };
            auto parse_float = [ & ]( float& dst ) {
                try {
                    dst = std::stof( val );
                }
                catch ( ... ) {
                }
            };

            if ( key == "aim.enabled" )
                parse_bool( val, s.aim_enabled );
            else if ( key == "aim.ignore_sliders" )
                parse_bool( val, s.aim_ignore_sliders );
            else if ( key == "aim.tablet_mode" )
                parse_bool( val, s.aim_tablet_mode );
            else if ( key == "aim.strength_x" || key == "aim.gain_x" ) {
                float v = 7.5f;
                parse_float( v );
                if ( v <= 1.0f ) v = std::clamp( v * 15.0f, 1.0f, 15.0f );
                s.aim_strength = std::clamp( v * ( 100.f / 15.f ), 5.f, 100.f );
            }
            else if ( key == "aim.strength_y" || key == "aim.gain_y" ) {
                float v = 7.5f;
                parse_float( v );
                if ( v <= 1.0f ) v = std::clamp( v * 15.0f, 1.0f, 15.0f );
                s.aim_strength = std::clamp( v * ( 100.f / 15.f ), 5.f, 100.f );
            }
            else if ( key == "aim.strength" ) {
                float v = 35.f;
                parse_float( v );
                if ( v <= 1.f )
                    v *= 100.f;
                else if ( v <= 15.f )
                    v *= ( 100.f / 15.f );
                s.aim_strength = std::clamp( v, 5.f, 100.f );
            }
            else if ( key == "aim.radius" )
                parse_float( s.aim_radius );
            else if ( key == "aim.smoothing_ms" )
                parse_float( s.aim_smoothing_ms );
            else if ( key == "aim.max_correction" )
                parse_float( s.aim_max_correction );
            else if ( key == "aim.adaptive" )
                parse_bool( val, s.aim_adaptive );
            else if ( key == "aim.adaptation_strength" )
                parse_float( s.aim_adaptation_strength );
            else if ( key == "aim.lerp" || key == "aim.smoothness" || key == "aim.attraction_rate" ) {
                float legacy_lerp = 0.24f;
                parse_float( legacy_lerp );
                s.aim_smoothing_ms = std::clamp( 135.f - legacy_lerp * 200.f, 45.f, 140.f );
            }

            else if ( key == "relax.enabled" )
                parse_bool( val, s.relax_enabled );
            else if ( key == "relax.ur" || key == "relax.ur_target" || key == "relax.hit_window_ms" )
                parse_float( s.relax_ur );
            else if ( key == "relax.tap_style" )
                parse_int( s.relax_tap_style );
            else if ( key == "relax.primary_key" )
                parse_int( s.relax_primary_key );
            else if ( key == "relax.singletap_speed_bpm" || key == "relax.singletap_bpm_cap" )
                parse_int( s.relax_singletap_speed_bpm );
            else if ( key == "relax.burst_tolerance" )
                parse_int( s.relax_burst_tolerance );
            else if ( key == "relax.stamina" )
                parse_float( s.relax_stamina );
            else if ( key == "relax.k1_hold_center" )
                parse_float( s.relax_k1_hold_center );
            else if ( key == "relax.k1_hold_spread" )
                parse_float( s.relax_k1_hold_spread );
            else if ( key == "relax.k2_hold_center" )
                parse_float( s.relax_k2_hold_center );
            else if ( key == "relax.k2_hold_spread" )
                parse_float( s.relax_k2_hold_spread );
            else if ( key == "relax.hold_floor" )
                parse_float( s.relax_hold_floor );
            else if ( key == "relax.hold_ceiling" )
                parse_float( s.relax_hold_ceiling );
            else if ( key == "relax.manual_offset_ms" )
                parse_int( s.relax_manual_offset_ms );
            else if ( key == "relax.timing_variation" )
                parse_bool( val, s.relax_timing_variation );
            else if ( key == "relax.early_variation_ms" )
                parse_int( s.relax_early_variation_ms );
            else if ( key == "relax.late_variation_ms" )
                parse_int( s.relax_late_variation_ms );
            else if ( key == "relax.timing_drift_ms" )
                parse_int( s.relax_timing_drift_ms );
            else if ( key == "replay.enabled" )
                parse_bool( val, s.replay_enabled );
            else if ( key == "replay.path" )
                s.replay_path_utf8 = val;
            else if ( key == "replay.parse_buttons" )
                parse_bool( val, s.replay_parse_buttons );
            else if ( key == "autobot.enabled" )
                parse_bool( val, s.autobot_enabled );
            else if ( key == "autobot.aim_spread" )
                parse_float( s.autobot_aim_spread );
            else if ( key == "autobot.curve_strength" )
                parse_float( s.autobot_curve_strength );
            else if ( key == "autobot.drift_amount" )
                parse_float( s.autobot_drift_amount );
            else if ( key == "autobot.momentum" )
                parse_float( s.autobot_momentum );
            else if ( key == "autobot.slider_laziness" )
                parse_float( s.autobot_slider_laziness );
            else if ( key == "autobot.spinner_rpm" )
                parse_float( s.autobot_spinner_rpm );
            else if ( key == "tap.enabled" )
                parse_bool( val, s.tap_enabled );
            else if ( key == "tap.assist_window" )
                parse_int( s.tap_assist_window );
            else if ( key == "tap.randomization" )
                parse_int( s.tap_randomization );
            else if ( key == "tap.ignore_sliders" )
                parse_bool( val, s.tap_ignore_sliders );
            else if ( key == "keys.left" )
                parse_int( s.custom_left_key );
            else if ( key == "keys.right" )
                parse_int( s.custom_right_key );
            else if ( key == "keys.menu" )
                parse_int( s.menu_keybind );
            else if ( key == "system.stream_proof" )
                parse_bool( val, s.stream_proof );
            else if ( key == "system.songs_path" )
                s.songs_path_utf8 = val;
        }
        s.aim_strength = std::clamp( s.aim_strength, 5.f, 100.f );
        s.aim_radius = std::clamp( s.aim_radius, 1.1f, 4.f );
        s.aim_smoothing_ms = std::clamp( s.aim_smoothing_ms, 35.f, 180.f );
        s.aim_max_correction = std::clamp( s.aim_max_correction, 0.1f, 1.25f );
        s.aim_adaptation_strength = std::clamp( s.aim_adaptation_strength, 0.f, 100.f );
        s.relax_tap_style = std::clamp( s.relax_tap_style, 0, 1 );
        s.relax_primary_key = std::clamp( s.relax_primary_key, 0, 1 );
        s.relax_singletap_speed_bpm = std::clamp( s.relax_singletap_speed_bpm, 100, 400 );
        s.relax_burst_tolerance = std::clamp( s.relax_burst_tolerance, 0, 2 );
        s.relax_stamina = std::clamp( s.relax_stamina, 0.f, 100.f );
        s.relax_early_variation_ms = std::clamp( s.relax_early_variation_ms, -25, 0 );
        s.relax_late_variation_ms = std::clamp( s.relax_late_variation_ms, 0, 25 );
        s.relax_timing_drift_ms = std::clamp( s.relax_timing_drift_ms, 0, 8 );
        return true;
    }

    inline std::vector<std::string> list_profiles( ) {
        std::vector<std::string> names;
        std::error_code ec;
        for ( const auto& entry : std::filesystem::directory_iterator( configs_dir( ), ec ) ) {
            if ( !entry.is_regular_file( ) )
                continue;
            if ( entry.path( ).extension( ) != ".cfg" )
                continue;
            names.push_back( entry.path( ).stem( ).string( ) );
        }
        std::sort( names.begin( ), names.end( ) );
        return names;
    }

}
