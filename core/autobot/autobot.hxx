#pragma once

#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <impl/util/playfield.hxx>
#include <core/relax/relax.hxx>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace autobot {

    constexpr float k_pi = 3.14159265358979323846f;

    struct point_t { float x = 0.f; float y = 0.f; };
    inline point_t operator+( point_t a, point_t b ) { return { a.x + b.x, a.y + b.y }; }
    inline point_t operator-( point_t a, point_t b ) { return { a.x - b.x, a.y - b.y }; }
    inline point_t operator*( point_t a, float s ) { return { a.x * s, a.y * s }; }
    inline point_t operator/( point_t a, float s ) { return s != 0.f ? point_t{ a.x / s, a.y / s } : point_t{}; }
    inline point_t& operator+=( point_t& a, point_t b ) { a.x += b.x; a.y += b.y; return a; }
    inline float dot( point_t a, point_t b ) { return a.x * b.x + a.y * b.y; }
    inline float length_sq( point_t a ) { return dot( a, a ); }
    inline float length( point_t a ) { return std::sqrt( length_sq( a ) ); }
    inline point_t normalized( point_t a ) { const float n = length( a ); return n > 0.0001f ? a / n : point_t{}; }
    inline point_t limit( point_t a, float maximum ) { const float n = length( a ); return n > maximum && n > 0.0001f ? a * ( maximum / n ) : a; }
    inline float saturate( float v ) { return std::clamp( v, 0.f, 1.f ); }
    inline float smoothstep( float a, float b, float x ) {
        if ( b <= a ) return x >= b ? 1.f : 0.f;
        const float t = saturate( ( x - a ) / ( b - a ) );
        return t * t * ( 3.f - 2.f * t );
    }

    enum class movement_state_t : uint8_t {
        idle, acquire, travel, arrival, slider_entry, slider_follow,
        slider_exit, spinner_entry, spinner_sustain, spinner_exit, break_idle
    };

    enum class destination_owner_t : uint8_t {
        none, gameplay, slider, spinner, startup, break_idle, break_dance, recovery
    };

    enum class decorative_state_t : uint8_t { none, moving, staging, pause };
    enum class destination_source_t : uint8_t {
        none, gameplay_trajectory, slider_follow, spinner_orbit, startup_choreography,
        break_choreography, break_rest, recovery, external_resync
    };
    enum class source_change_reason_t : uint8_t {
        none, target_change, material_schedule_change, state_change, choreography_start,
        choreography_segment, acquisition_deadline, recovery_required, external_resync,
        invalid_trajectory
    };
    enum class replan_reason_t : uint8_t {
        none, target_change, material_schedule_change, state_change, recovery_required,
        slider_change, spinner_change, invalid_trajectory, choreography_segment
    };

    struct diagnostics_t {
        uint64_t objects_completed = 0, movement_replans = 0, teleport_discontinuities = 0;
        uint64_t internal_trajectory_discontinuities = 0, output_cap_events = 0, external_resync_events = 0;
        uint64_t target_change_replans = 0, timing_update_replans = 0, slider_state_replans = 0;
        uint64_t recovery_replans = 0, invalid_data_replans = 0, fidget_segments = 0;
        uint64_t motion_samples = 0, target_offset_samples = 0;
        double speed_sum = 0.0, acceleration_sum = 0.0, curvature_sum = 0.0, target_offset_sum = 0.0;
        double desired_speed_sum = 0.0, tracking_error_sum = 0.0, trajectory_lag_ms_sum = 0.0;
        float desired_speed_peak = 0.f, tracking_error_peak = 0.f, trajectory_lag_ms_peak = 0.f;
        float peak_speed = 0.f, peak_acceleration = 0.f, max_frame_displacement = 0.f;
        uint64_t k1_count = 0, k2_count = 0, singletap_count = 0, alternate_count = 0;
        uint64_t timing_samples = 0;
        double timing_sum = 0.0, timing_sq_sum = 0.0;
        uint64_t slider_samples = 0;
        double slider_error_sum = 0.0;
        float slider_error_peak = 0.f;
        uint64_t spinner_samples = 0;
        double spinner_rpm_sum = 0.0, spinner_radius_sum = 0.0;
        float spinner_rpm_peak = 0.f;
        float spinner_radius_min = std::numeric_limits<float>::max( );
        float spinner_radius_max = 0.f, spinner_entry_speed = 0.f, spinner_exit_speed = 0.f;
        double idle_distance = 0.0;
        uint64_t arrival_samples = 0, arrival_inside_full = 0, arrival_inside_075 = 0, arrival_inside_050 = 0;
        double arrival_error_sum = 0.0;
        float arrival_error_peak = 0.f;
        int last_arrival_target_screen_x = 0, last_arrival_target_screen_y = 0;
        int last_arrival_object_screen_x = 0, last_arrival_object_screen_y = 0;
        int last_arrival_cursor_screen_x = 0, last_arrival_cursor_screen_y = 0;
        int last_arrival_press_time = 0;
        float last_arrival_hit_radius_screen = 0.f, last_arrival_error_screen = 0.f;
        float projection_self_check_error = 0.f, spinner_target_rpm = 0.f;
        int startup_pattern = 0, break_pattern = 0;
        decorative_state_t decorative_state = decorative_state_t::none;
        uint64_t startup_segments_completed = 0, break_dance_loops_completed = 0;
        uint64_t decorative_interruptions = 0, gameplay_owner_violations = 0;
        uint64_t unexpected_offroute_replans = 0;
        double decorative_distance = 0.0;
        int decorative_remaining_ms = 0, decorative_return_ms = 0;
        destination_source_t current_source = destination_source_t::none;
        destination_source_t previous_source = destination_source_t::none;
        source_change_reason_t source_change_reason = source_change_reason_t::none;
        replan_reason_t last_replan_reason = replan_reason_t::none;
        int source_object_index = -1;
        uint64_t trajectory_id = 0, target_point_mutations = 0;
        point_t internal_desired_delta{}, final_requested_delta{}, observed_cursor_delta{};
        uint64_t startup_primitives_generated = 0, break_primitives_generated = 0;
        float requested_accuracy = 100.f, session_accuracy = 100.f, accuracy_bias = 0.f;
        double accuracy_score_sum = 0.0;
        uint64_t accuracy_samples = 0;
        int accuracy_state = 0;
        uint64_t controlled_100 = 0;
        float predicted_accuracy = 100.f, accuracy_debt = 0.f;
        uint64_t owner_transition_replans = 0;
    };

    // One captured context around an abnormal (off-trajectory) movement sample.
    // Filled by a small bounded ring buffer so a live random-jump can be inspected
    // after the fact without any per-frame logging.
    struct bad_delta_trace_t {
        uint64_t frame = 0;
        int control_time = 0;
        float dt = 0.f;
        int object_index = -1, trajectory_target = -1;
        uint64_t trajectory_id = 0;
        destination_owner_t owner = destination_owner_t::none;
        destination_owner_t previous_owner = destination_owner_t::none;
        destination_source_t source = destination_source_t::none;
        replan_reason_t replan = replan_reason_t::none;
        point_t position_before{}, desired{}, velocity{}, acceleration{};
        float displacement = 0.f, kinematic_bound = 0.f;
        int external_mismatch_streak = 0;
        bool external_resync = false;
    };

    class c_autobot {
    public:
        bool enabled = false;
        float aim_spread = 0.10f;
        float curve_strength = 0.36f;
        float drift_amount = 1.05f;
        float momentum = 0.82f;
        float slider_laziness = 0.08f;
        float spinner_rpm = 420.f;
        bool startup_motion = true;
        bool break_motion = true;
        bool energetic_dances = true;
        bool gameplay_flow = true;
        float startup_energy = 0.82f;
        float break_energy = 0.68f;
        float target_accuracy = 98.5f;

        [[nodiscard]] diagnostics_t diagnostics( ) const { return m_diag; }
        [[nodiscard]] movement_state_t movement_state( ) const { return m_motion.state; }
        [[nodiscard]] destination_owner_t destination_owner( ) const { return m_destination_owner; }
        [[nodiscard]] uint64_t bad_delta_count( ) const { return m_trace_count; }
        [[nodiscard]] bool last_bad_delta( bad_delta_trace_t& out ) const {
            if ( m_trace_count == 0 ) return false;
            out = m_trace[ ( m_trace_head + k_trace_capacity - 1 ) % k_trace_capacity ];
            return true;
        }

        void on_leave_play( const osu::game_snapshot_t& ) {
            m_in_play = false;
            m_paused = false;
            m_motion = {};
            m_slider_cache.clear( );
            m_hit_points.clear( );
            m_hit_point_shadow.clear( );
            m_accuracy_edge_bias.clear( );
            m_hit_point_valid.clear( );
            m_map_object_count = 0;
            m_last_game_time = 0;
            m_last_control_time = 0;
            m_last_completed_index = -1;
            m_diag_scheduled_through = -1;
            m_arrival_recorded_through = -1;
            m_last_slider_index = -1;
            m_active_slider_ball_valid = false;
            m_choreography = {};
            m_destination_owner = destination_owner_t::none;
            m_prev_frame_owner = destination_owner_t::none;
            m_external_mismatch_streak=0;m_last_observed_valid=false;
        }

        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map,
                     const relax::c_relax& timing, bool user_control = false ) {
            if ( !enabled || game.cur_state != osu::game_state_t::play || !map.loaded || map.objects.empty( ) ) {
                if ( m_in_play ) on_leave_play( game );
                return;
            }
            const HWND hwnd = input::target_window( );
            RECT window{};
            if ( !hwnd || !playfield::get_playfield_rect( hwnd, window ) ) return;
            if ( user_control ) {
                m_paused = true;
                m_motion.last_real_update = std::chrono::steady_clock::now( );
                return;
            }

            const int previous_game_time = m_last_game_time;
            const bool new_session = !m_in_play || m_map_object_count != map.objects.size( ) ||
                game.cur_time < m_last_game_time - 200;
            m_hr_flip = ( game.cur_mod_state & 16 ) != 0 && !map.hr;
            if ( new_session ) begin_session( game, map, window );
            else if ( m_paused ) resume_from_actual_cursor( window );
            m_in_play = true;
            m_paused = false;
            if ( !m_motion.initialized && !initialize_from_actual_cursor( window ) ) return;

            const auto now = std::chrono::steady_clock::now( );
            float real_dt = 1.f / 120.f;
            if ( m_motion.last_real_update.time_since_epoch( ).count( ) != 0 )
                real_dt = std::chrono::duration<float>( now - m_motion.last_real_update ).count( );
            m_motion.last_real_update = now;
            const float dt = std::clamp( real_dt, 0.001f, 0.025f );
            if ( !new_session && previous_game_time >= 0 && game.cur_time > previous_game_time ) {
                const float measured_rate = static_cast<float>( game.cur_time - previous_game_time ) /
                    ( std::max(real_dt,0.001f) * 1000.f );
                if ( measured_rate >= 0.5f && measured_rate <= 2.f )
                    m_game_rate += ( measured_rate - m_game_rate ) * 0.08f;
            }
            m_external_resync_this_frame=false;
            sync_external_cursor( window, dt );

            update_schedule_diagnostics( map, timing );
            const int control_time=std::max(game.cur_time,
                static_cast<int>(std::floor(timing.prepared_game_time())));
            ++m_frame_counter; m_frame_control_time=control_time; m_frame_dt=dt;
            const int previous_control_time=new_session?control_time-1:m_last_control_time;
            point_t desired_position = m_motion.position, desired_velocity = m_motion.velocity;
            point_t desired_acceleration = m_motion.acceleration;
            float max_speed = 1800.f, max_acceleration = 14000.f;
            m_active_slider_ball_valid = false;

            const int active = find_active_long_object( control_time, map, timing );
            int authoritative_target = -1;
            bool gameplay_must_own = active >= 0;
            if ( active >= 0 ) {
                stop_choreography( m_choreography.active );
                const auto& obj = map.objects[ static_cast<size_t>( active ) ];
                if ( is_spinner_object( obj ) ) {
                    m_destination_owner=destination_owner_t::spinner;
                    set_destination_source(destination_source_t::spinner_orbit,
                        source_change_reason_t::state_change,active);
                    update_spinner( control_time, active, obj, dt, desired_position,
                        desired_velocity, max_speed, max_acceleration, map, timing );
                }
                else {
                    m_destination_owner=destination_owner_t::slider;
                    set_destination_source(destination_source_t::slider_follow,
                        source_change_reason_t::state_change,active);
                    update_slider( control_time, active, obj, map, desired_position,
                        desired_velocity, max_speed, max_acceleration );
                }
            }
            else {
                m_spinner.active = false;
                const int target_index = find_next_object( control_time, map, timing );
                authoritative_target=target_index;
                if ( target_index >= 0 ) {
                    const auto& obj = map.objects[ static_cast<size_t>( target_index ) ];
                    const int arrival = scheduled_press_time( target_index, obj, timing );
                    const point_t target = target_point( target_index, map );
                    const float time_left = static_cast<float>( arrival - control_time );
                    const float distance = length( target - m_motion.position );
                    const float acceleration_time = 2000.f * std::sqrt( distance /
                        ( 18000.f * m_style.acceleration ) );
                    const float travel_time = distance * 1000.f / 1500.f;
                    const float lead = std::clamp( std::max( acceleration_time, travel_time ) + 130.f, 220.f, 1400.f );
                    const int return_time=static_cast<int>(std::ceil(lead));
                    const bool startup=m_last_completed_index<0&&target_index==0&&startup_motion;
                    // Only treat a genuinely large gap as a break.  Ordinary slow
                    // sections (a ~2s spacing between notes) previously tripped this
                    // and let the cursor wander off to a decorative staging point mid
                    // map, which reads as an unexplained jump.  A real break is seconds
                    // long, so require a wider gap plus a comfortable return budget.
                    const bool long_break=m_last_completed_index>=0&&break_motion&&time_left>2600.f;
                    const bool choreography_allowed=(startup||long_break)&&time_left>return_time+520.f;
                    gameplay_must_own=!choreography_allowed;
                    const bool choreography_running=choreography_allowed&&update_choreography(control_time,now,
                        startup,target_index,target,hit_radius(map),static_cast<int>(time_left),return_time,
                        desired_position,desired_velocity,desired_acceleration);
                    if ( !choreography_running ) {
                        stop_choreography( m_choreography.active );
                        m_destination_owner=(time_left<=0.f||m_external_resync_this_frame)?
                            destination_owner_t::recovery:destination_owner_t::gameplay;
                        set_destination_source(m_destination_owner==destination_owner_t::recovery?
                            destination_source_t::recovery:destination_source_t::gameplay_trajectory,
                            m_external_resync_this_frame?source_change_reason_t::external_resync:
                            time_left<=0.f?source_change_reason_t::recovery_required:
                            source_change_reason_t::target_change,target_index);
                        // A slider/spinner/choreography frame drives the cursor by means
                        // other than the gameplay quintic, so any quintic left over from
                        // priming holds a stale p0/start time.  Re-anchor on the first
                        // gameplay frame after such an owner so the plan starts from the
                        // ACTUAL current position/velocity/acceleration, never a stale one.
                        switch ( m_prev_frame_owner ) {
                        case destination_owner_t::slider:
                        case destination_owner_t::spinner:
                        case destination_owner_t::startup:
                        case destination_owner_t::break_idle:
                        case destination_owner_t::break_dance:
                            if ( m_motion.trajectory.valid ) {
                                m_motion.trajectory.valid = false;
                                m_diag.owner_transition_replans++;
                            }
                            break;
                        default: break;
                        }
                        ensure_object_trajectory( control_time, now, target_index, arrival, target, map, timing );
                        evaluate_trajectory( now, desired_position, desired_velocity, desired_acceleration );
                        m_motion.state = time_left <= 35.f ? movement_state_t::arrival :
                            time_left > lead * 0.72f ? movement_state_t::acquire :
                            is_slider_object( obj ) ? movement_state_t::slider_entry :
                            is_spinner_object( obj ) ? movement_state_t::spinner_entry : movement_state_t::travel;
                        const float available = std::max( time_left * 0.001f, 0.035f );
                        const float required = distance / available;
                        max_speed = std::clamp( required * 2.1f + 500.f, 1200.f, 6200.f );
                        max_acceleration = std::clamp( required * 8.f + 9000.f, 12000.f, 52000.f );
                    }
                }
                else {
                    stop_choreography( false );m_destination_owner=destination_owner_t::break_idle;
                    set_destination_source(destination_source_t::break_rest,
                        source_change_reason_t::state_change,-1);
                    update_idle( control_time, true, desired_position, desired_velocity, desired_acceleration );
                }
            }
            const bool decorative_owner=m_destination_owner==destination_owner_t::startup||
                m_destination_owner==destination_owner_t::break_idle||m_destination_owner==destination_owner_t::break_dance;
            if(gameplay_must_own&&decorative_owner)
                m_diag.gameplay_owner_violations++;
            if(m_destination_owner==destination_owner_t::gameplay&&authoritative_target>=0&&
                (!m_motion.trajectory.valid||m_motion.trajectory.target_index!=authoritative_target))
                m_diag.unexpected_offroute_replans++;
            apply_motion_sample( desired_position, desired_velocity, desired_acceleration, dt );
            emit_cursor( window );
            record_slider_error( window );
            record_arrivals( previous_control_time, control_time, map, timing, window );
            update_completed_objects( control_time, map, timing );
            if(active<0&&m_destination_owner==destination_owner_t::gameplay)
                prime_next_trajectory(control_time,now,m_last_completed_index+1,map,timing);
            else if(active>=0&&map.objects[static_cast<size_t>(active)].end_time-control_time<=8)
                prime_next_trajectory(control_time,now,active+1,map,timing);
            m_last_game_time = game.cur_time;
            m_last_control_time = control_time;
            m_prev_frame_owner = m_destination_owner;
        }

    private:
        struct trajectory_t {
            bool valid = false;
            point_t p0{}, p1{}, v0{}, v1{}, a0{}, a1{};
            int start_time = 0, end_time = 0, target_index = -1;
            std::chrono::steady_clock::time_point start_real{}, end_real{};
            uint64_t id = 0;
        };
        struct movement_t {
            bool initialized = false;
            point_t position{}, velocity{}, acceleration{}, previous_velocity{};
            float last_frame_displacement = 0.f;
            trajectory_t trajectory{};
            movement_state_t state = movement_state_t::idle;
            int current_target = -1, next_target = -1;
            std::chrono::steady_clock::time_point last_real_update{};
            POINT last_output_screen{};
            bool last_output_valid = false;
        };
        struct session_style_t {
            float directness = 0.85f, curvature = 1.f, acceleration = 1.f;
            float momentum_retention = 0.85f, arrival_depth = 0.55f;
            float overshoot = 0.02f, idle_energy = 0.5f, slider_tightness = 0.85f, phase = 0.f;
        };
        struct spinner_state_t {
            bool active = false;
            int object_index = -1;
            point_t center{};
            float angle = 0.f, angular_velocity = 0.f, radius = 70.f;
            float radius_velocity = 0.f, direction = 1.f;
        };
        struct cached_slider_t {
            int start_time = -1;
            std::vector<point_t> path;
            std::vector<float> distances;
            float total_distance = 0.f;
        };
        struct decorative_waypoint_t {
            point_t position{};
            int duration_ms = 300;
            bool loop_end = false;
            bool pause = false;
            bool primitive_end = false;
        };
        enum class primitive_t : uint8_t {
            short_flick, micro_arc, partial_loop, small_loop, hook, diagonal_sweep,
            reverse_sweep, s_curve, local_reposition, quick_circle, pause
        };
        struct choreography_t {
            bool active = false, startup = false, energetic = false;
            int pattern = -1, waypoint = 0, segment_end_time = 0;
            point_t staging{}, target{}, local_anchor{};
            float hit_radius = 32.f, energy = 0.5f;
            int last_primitive = -1, primitives = 0;
            std::vector<decorative_waypoint_t> points;
        };

        movement_t m_motion{};
        session_style_t m_style{};
        spinner_state_t m_spinner{};
        choreography_t m_choreography{};
        diagnostics_t m_diag{};
        std::vector<cached_slider_t> m_slider_cache;
        std::vector<point_t> m_hit_points;
        std::vector<point_t> m_hit_point_shadow;
        std::vector<uint8_t> m_hit_point_valid;
        std::vector<float> m_accuracy_edge_bias;
        bool m_in_play = false, m_paused = false, m_hr_flip = false;
        destination_owner_t m_destination_owner = destination_owner_t::none;
        size_t m_map_object_count = 0;
        int m_last_game_time = 0, m_last_control_time = 0;
        int m_last_completed_index = -1, m_diag_scheduled_through = -1;
        int m_arrival_recorded_through = -1, m_last_slider_index = -1;
        uint32_t m_session_seed = 0x9e3779b9u;
        point_t m_idle_anchor{};
        int m_idle_segment_end = 0;
        float m_game_rate = 1.f;
        point_t m_active_slider_ball{};
        bool m_active_slider_ball_valid = false;
        bool m_external_resync_this_frame = false;
        int m_external_mismatch_streak = 0;
        uint64_t m_next_trajectory_id = 1;
        POINT m_last_observed_screen{};
        bool m_last_observed_valid = false;
        int m_last_startup_pattern = -1, m_last_break_pattern = -1;
        destination_owner_t m_prev_frame_owner = destination_owner_t::none;
        uint64_t m_frame_counter = 0;
        int m_frame_control_time = 0;
        float m_frame_dt = 0.f;

        static constexpr size_t k_trace_capacity = 24;
        std::array<bad_delta_trace_t, k_trace_capacity> m_trace{};
        size_t m_trace_head = 0;
        uint64_t m_trace_count = 0;

        void record_bad_delta( point_t before, point_t desired, float displacement, float bound ) {
            bad_delta_trace_t t{};
            t.frame = m_frame_counter;
            t.control_time = m_frame_control_time;
            t.dt = m_frame_dt;
            t.object_index = m_diag.source_object_index;
            t.trajectory_target = m_motion.trajectory.target_index;
            t.trajectory_id = m_motion.trajectory.id;
            t.owner = m_destination_owner;
            t.previous_owner = m_prev_frame_owner;
            t.source = m_diag.current_source;
            t.replan = m_diag.last_replan_reason;
            t.position_before = before;
            t.desired = desired;
            t.velocity = m_motion.velocity;
            t.acceleration = m_motion.acceleration;
            t.displacement = displacement;
            t.kinematic_bound = bound;
            t.external_mismatch_streak = m_external_mismatch_streak;
            t.external_resync = m_external_resync_this_frame;
            m_trace[ m_trace_head ] = t;
            m_trace_head = ( m_trace_head + 1 ) % k_trace_capacity;
            ++m_trace_count;
        }

        static bool is_slider_object( const osu::hit_object_t& obj ) {
            return ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) != 0;
        }
        static bool is_spinner_object( const osu::hit_object_t& obj ) {
            if ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::spinner ) ) return true;
            if ( !is_slider_object( obj ) || !obj.slider_curve_str.empty( ) || obj.slider_length > 0.f ) return false;
            const float dx = obj.x - 256.f, dy = obj.y - 192.f;
            return obj.end_time - obj.start_time >= 400 && dx * dx + dy * dy < 48.f * 48.f;
        }
        uint32_t next_random( ) {
            m_session_seed ^= m_session_seed << 13; m_session_seed ^= m_session_seed >> 17;
            m_session_seed ^= m_session_seed << 5; return m_session_seed;
        }
        float random_unit( ) { return static_cast<float>( next_random( ) & 0x00ffffffu ) / 16777216.f; }

        void begin_session( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map, const RECT& window ) {
            m_in_play = true; m_paused = false; m_motion = {}; m_spinner = {}; m_choreography = {}; m_diag = {};
            m_destination_owner=destination_owner_t::none;
            m_map_object_count = map.objects.size( ); m_slider_cache.assign( m_map_object_count, {} );
            m_hit_points.assign( m_map_object_count, {} ); m_hit_point_valid.assign( m_map_object_count, 0 );
            m_hit_point_shadow.assign( m_map_object_count, {} );
            m_accuracy_edge_bias.assign(m_map_object_count,0.f);
            m_last_completed_index = -1; m_diag_scheduled_through = -1; m_arrival_recorded_through = -1;
            m_last_slider_index = -1; m_game_rate = std::clamp( game.speed_mult, 0.5f, 2.f );
            m_active_slider_ball_valid = false;
            m_next_trajectory_id=1;m_external_mismatch_streak=0;m_last_observed_valid=false;
            m_prev_frame_owner=destination_owner_t::none;m_trace_head=0;m_trace_count=0;m_frame_counter=0;
            m_session_seed = static_cast<uint32_t>( std::chrono::steady_clock::now( ).time_since_epoch( ).count( ) ) ^
                static_cast<uint32_t>( game.map_id * 2654435761u );
            m_style.directness = 0.80f + random_unit( ) * 0.16f;
            m_style.curvature = 0.75f + random_unit( ) * 0.50f;
            m_style.acceleration = 0.90f + random_unit( ) * 0.20f;
            m_style.momentum_retention = 0.82f + random_unit( ) * 0.14f;
            m_style.arrival_depth = 0.45f + random_unit( ) * 0.20f;
            m_style.overshoot = random_unit( ) * 0.035f;
            m_style.idle_energy = 0.35f + random_unit( ) * 0.45f;
            m_style.slider_tightness = 0.82f + random_unit( ) * 0.16f;
            m_style.phase = random_unit( ) * 2.f * k_pi;
            verify_projection( window );
            initialize_from_actual_cursor( window ); m_idle_anchor = m_motion.position; m_idle_segment_end = game.cur_time;
        }
        bool initialize_from_actual_cursor( const RECT& window ) {
            POINT cursor{}; if ( !input::get_cursor_pos( &cursor ) ) return false;
            float x = 0.f, y = 0.f;
            if ( !playfield::screen_to_playfield( cursor.x, cursor.y, window, x, y ) ) return false;
            m_motion.position = { x, y }; m_motion.velocity = {}; m_motion.acceleration = {};
            m_motion.previous_velocity = {}; m_motion.trajectory = {}; m_motion.initialized = true;
            m_motion.last_output_screen = cursor; m_motion.last_output_valid = true;
            m_motion.last_real_update = std::chrono::steady_clock::now( ); return true;
        }
        void resume_from_actual_cursor( const RECT& window ) {
            if ( initialize_from_actual_cursor( window ) ) m_diag.external_resync_events++;
            m_motion.trajectory = {};
            m_motion.current_target = -1; m_spinner.active = false; m_idle_anchor = m_motion.position;
        }
        void verify_projection( const RECT& window ) {
            static constexpr point_t samples[] = { {0.f,0.f}, {256.f,192.f}, {512.f,384.f} };
            float maximum = 0.f;
            const int width = window.right-window.left, height = window.bottom-window.top;
            for ( const auto sample : samples ) {
                const auto canonical = playfield::playfield_to_screen( sample.x, sample.y, window );
                float local_x=0.f, local_y=0.f;
                playfield::project_osu_to_window( sample.x, sample.y, width, height, local_x, local_y );
                const float check_x=static_cast<float>(window.left)+local_x;
                const float check_y=static_cast<float>(window.top)+local_y;
                maximum=std::max(maximum,std::abs(static_cast<float>(canonical.x)-check_x));
                maximum=std::max(maximum,std::abs(static_cast<float>(canonical.y)-check_y));
            }
            m_diag.projection_self_check_error=maximum;
        }
        void set_destination_source(destination_source_t source,source_change_reason_t reason,int object_index){
            if(m_diag.current_source!=source){
                m_diag.previous_source=m_diag.current_source;
                m_diag.current_source=source;
                m_diag.source_change_reason=reason;
            }
            m_diag.source_object_index=object_index;
        }
        void note_replan(replan_reason_t reason){
            m_diag.last_replan_reason=reason;
            m_diag.movement_replans++;
        }
        void sync_external_cursor( const RECT& window, float dt ) {
            if ( !m_motion.last_output_valid ) return;
            POINT actual{}; if ( !input::get_cursor_pos( &actual ) ) return;
            if(m_last_observed_valid)
                m_diag.observed_cursor_delta={static_cast<float>(actual.x-m_last_observed_screen.x),
                    static_cast<float>(actual.y-m_last_observed_screen.y)};
            m_last_observed_screen=actual;m_last_observed_valid=true;
            const float dx=static_cast<float>(actual.x-m_motion.last_output_screen.x);
            const float dy=static_cast<float>(actual.y-m_motion.last_output_screen.y);
            if ( std::sqrt(dx*dx+dy*dy) <= 6.f ) {m_external_mismatch_streak=0;return;}
            // One stale GetCursorPos sample or a delayed absolute tablet report must
            // not tear down a valid committed trajectory.  Require a persistent
            // mismatch before rebasing the controller on the observed cursor.
            if(++m_external_mismatch_streak<3)return;
            float x=0.f,y=0.f;
            if(!playfield::screen_to_playfield(actual.x,actual.y,window,x,y))return;
            const point_t observed{x,y};
            const point_t observed_velocity=limit((observed-m_motion.position)/std::max(dt,0.001f),6500.f);
            m_motion.acceleration=limit((observed_velocity-m_motion.velocity)/std::max(dt,0.001f),60000.f);
            m_motion.velocity=observed_velocity;
            m_motion.position=observed;
            m_motion.trajectory={};
            m_motion.last_output_screen=actual;
            m_external_mismatch_streak=0;
            m_external_resync_this_frame=true;
            m_diag.external_resync_events++;
            m_diag.invalid_data_replans++;
            note_replan(replan_reason_t::invalid_trajectory);
            set_destination_source(destination_source_t::external_resync,
                source_change_reason_t::external_resync,-1);
        }
        point_t object_center( const osu::hit_object_t& obj ) const { return { obj.x, m_hr_flip ? 384.f - obj.y : obj.y }; }
        float hit_radius( const osu::beatmap_data_t& map ) const {
            float cs = map.cs; if ( m_hr_flip ) cs = std::min( cs * 1.3f, 10.f );
            return std::max( 8.f, 54.4f - 4.48f * cs );
        }
        point_t target_point( int index, const osu::beatmap_data_t& map ) {
            if ( index < 0 || index >= static_cast<int>( map.objects.size( ) ) ) return m_motion.position;
            if ( m_hit_point_valid[ static_cast<size_t>( index ) ] ) {
                const size_t slot=static_cast<size_t>(index);
                if(length_sq(m_hit_points[slot]-m_hit_point_shadow[slot])>0.0001f){
                    m_diag.target_point_mutations++;
                    m_hit_points[slot]=m_hit_point_shadow[slot];
                }
                return m_hit_point_shadow[slot];
            }
            const auto& obj = map.objects[ static_cast<size_t>( index ) ]; point_t center = object_center( obj );
            if ( is_spinner_object( obj ) ) {
                point_t radial = normalized( m_motion.position - center ); if ( length_sq( radial ) < 0.1f ) radial = { 1.f, 0.f };
                m_hit_points[ static_cast<size_t>( index ) ] = center + radial * 70.f;
                m_hit_point_shadow[static_cast<size_t>(index)]=m_hit_points[static_cast<size_t>(index)];
                m_hit_point_valid[ static_cast<size_t>( index ) ] = 1; return m_hit_points[ static_cast<size_t>( index ) ];
            }
            if ( is_slider_object( obj ) ) {
                m_hit_points[ static_cast<size_t>( index ) ] = center;
                m_hit_point_shadow[static_cast<size_t>(index)]=center;
                m_hit_point_valid[ static_cast<size_t>( index ) ] = 1;
                return center;
            }
            const uint32_t h = static_cast<uint32_t>( index + 1 ) * 747796405u ^ m_session_seed;
            const float u = static_cast<float>( h & 0xffffu ) / 65535.f;
            const float v = static_cast<float>( ( h >> 16 ) & 0xffffu ) / 65535.f;
            const float accuracy_edge=index<static_cast<int>(m_accuracy_edge_bias.size())?
                m_accuracy_edge_bias[static_cast<size_t>(index)]:0.f;
            const float safe_fraction = accuracy_edge>0.f?accuracy_edge:
                ( 0.08f + 0.38f * std::clamp( aim_spread, 0.f, 1.f ) ) *
                ( 0.82f + 0.32f * m_style.arrival_depth );
            const float radius = hit_radius( map ) * (accuracy_edge>0.f?
                accuracy_edge*(0.92f+0.08f*std::sqrt(u)):safe_fraction*std::sqrt( u ));
            const float angle = v * 2.f * k_pi + m_style.phase;
            const point_t selected = center + point_t{ std::cos( angle ), std::sin( angle ) } * radius;
            m_hit_points[ static_cast<size_t>( index ) ] = selected;
            m_hit_point_shadow[static_cast<size_t>(index)]=selected;
            m_hit_point_valid[ static_cast<size_t>( index ) ] = 1;
            m_diag.target_offset_samples++; m_diag.target_offset_sum += radius; return selected;
        }
        static int scheduled_press_time( int index, const osu::hit_object_t& obj, const relax::c_relax& timing ) {
            relax::c_relax::scheduled_object_t s{};
            return timing.scheduled_object( static_cast<size_t>( index ), s ) ? s.press_time : obj.start_time;
        }
        int find_next_object( int game_time, const osu::beatmap_data_t& map, const relax::c_relax& timing ) const {
            const size_t start = static_cast<size_t>( std::max( m_last_completed_index + 1, 0 ) );
            (void)game_time;(void)timing;
            return start<map.objects.size()?static_cast<int>(start):-1;
        }
        int find_active_long_object( int game_time, const osu::beatmap_data_t& map, const relax::c_relax& timing ) const {
            const size_t start = static_cast<size_t>( std::max( m_last_completed_index - 1, 0 ) );
            for ( size_t i = start; i < map.objects.size( ); ++i ) {
                const auto& obj = map.objects[ i ];
                if ( !is_slider_object( obj ) && !is_spinner_object( obj ) ) continue;
                const int press = scheduled_press_time( static_cast<int>( i ), obj, timing );
                if ( game_time >= press && game_time <= obj.end_time + 8 ) return static_cast<int>( i );
                if ( press > game_time ) break;
            } return -1;
        }
        void prime_next_trajectory(int game_time,std::chrono::steady_clock::time_point now,int index,
                                   const osu::beatmap_data_t& map,const relax::c_relax& timing){
            if(index<0||index>=static_cast<int>(map.objects.size()))return;
            const auto& obj=map.objects[static_cast<size_t>(index)];
            const int arrival=scheduled_press_time(index,obj,timing);
            if(arrival<=game_time)return;
            const point_t target=target_point(index,map);
            const float distance=length(target-m_motion.position);
            const float lead=std::clamp(std::max(2000.f*std::sqrt(distance/(18000.f*m_style.acceleration)),
                distance*1000.f/1500.f)+130.f,220.f,1400.f);
            if(arrival-game_time<=lead)
                ensure_object_trajectory(game_time,now,index,arrival,target,map,timing);
        }
        point_t outgoing_velocity( int index, point_t target, const osu::beatmap_data_t& map, const relax::c_relax& timing ) {
            if(!gameplay_flow)return{};
            const int next = index + 1; if ( next >= static_cast<int>( map.objects.size( ) ) ) return {};
            const point_t next_target = target_point( next, map );
            const int a = scheduled_press_time( index, map.objects[ static_cast<size_t>( index ) ], timing );
            const int b = scheduled_press_time( next, map.objects[ static_cast<size_t>( next ) ], timing );
            const float seconds = std::max( ( b - a ) * 0.001f, 0.035f );
            const point_t first_delta = next_target - target;
            const float first_distance = length( first_delta );
            point_t direction = normalized( first_delta );
            float geometry_gain = 1.f;

            // A cached 8-object window shapes the departure tangent without causing
            // per-frame target-point rerolls or scans.  Near stacks settle, compact
            // streams remain direct, and sharp reversals shed incompatible momentum.
            const float radius = hit_radius( map );
            if ( first_distance < radius * 0.45f ) geometry_gain = 0.18f;
            for ( int look = 2; look <= 8 && index + look < static_cast<int>( map.objects.size( ) ); ++look ) {
                const point_t a_point = target_point( index + look - 1, map );
                const point_t b_point = target_point( index + look, map );
                const point_t future_direction = normalized( b_point - a_point );
                const float alignment = dot( direction, future_direction );
                const float weight = 0.16f / static_cast<float>( look - 1 );
                if ( alignment > -0.15f )
                    direction = normalized( direction + future_direction * weight );
                if ( look == 2 && alignment < -0.35f )
                    geometry_gain *= 0.38f;
                const int interval = map.objects[ static_cast<size_t>( index + look ) ].start_time -
                    map.objects[ static_cast<size_t>( index + look - 1 ) ].start_time;
                if ( interval < 125 && length( b_point - a_point ) < radius * 2.2f )
                    geometry_gain = std::min( geometry_gain, 0.72f );
            }
            const float carry = std::clamp( momentum, 0.f, 0.95f ) * m_style.momentum_retention;
            return limit( direction * ( first_distance / seconds ) * carry * geometry_gain, 2200.f );
        }
        void ensure_object_trajectory( int game_time, std::chrono::steady_clock::time_point now,
                                       int target_index, int arrival_time, point_t target,
                                       const osu::beatmap_data_t& map, const relax::c_relax& timing ) {
            trajectory_t& tr = m_motion.trajectory;
            if ( tr.valid && tr.target_index == target_index && tr.end_time == arrival_time ) return;
            const bool timing_change = tr.valid && tr.target_index == target_index && tr.end_time != arrival_time;
            const bool recovery = arrival_time <= game_time;
            const float seconds = std::max( ( arrival_time - game_time ) * 0.001f /
                std::max( m_game_rate, 0.5f ), 0.018f );
            const float required = length( target - m_motion.position ) / seconds;
            // A new quintic segment inherits the exact current derivatives.  Style
            // changes the rest of the curve, never the t=0 velocity/acceleration.
            point_t start_v = m_motion.velocity;
            const point_t direct = normalized( target - m_motion.position );
            const float reliability_pressure=saturate((required-850.f)/1800.f);
            point_t end_v{};
            const auto& target_object = map.objects[ static_cast<size_t>( target_index ) ];
            if ( is_spinner_object( target_object ) ) {
                const point_t radial = normalized( target - object_center( target_object ) );
                const point_t tangent{ -radial.y, radial.x };
                float direction_sign = dot( start_v, tangent ) >= 0.f ? 1.f : -1.f;
                if ( length_sq( start_v ) < 100.f )
                    direction_sign = ( ( m_session_seed ^ static_cast<uint32_t>( target_index ) ) & 1u ) ? 1.f : -1.f;
                end_v = tangent * direction_sign * 520.f;
            }
            else {
                end_v = outgoing_velocity( target_index, target, map, timing );
                end_v = end_v * ( 0.72f + 0.28f * std::clamp( curve_strength, 0.f, 1.f ) * m_style.curvature );
                end_v = end_v * (0.86f+0.14f*m_style.directness)*(1.f-0.18f*reliability_pressure);
            }
            if ( required > 1150.f && !is_spinner_object( target_object ) )
                end_v += direct * required * m_style.overshoot * std::clamp( curve_strength, 0.f, 1.f )*
                    (1.f-reliability_pressure);
            if ( required > 1800.f ) end_v = end_v * 0.82f;
            tr = {};
            tr.valid=true;tr.p0=m_motion.position;tr.p1=target;tr.v0=start_v;tr.v1=end_v;
            tr.a0=m_motion.acceleration;tr.a1={};tr.start_time=game_time;
            // Keep the immutable scheduled deadline as the identity key even when
            // a late recovery needs a short real-time completion window.  Using the
            // recovery end here caused the same target to replan every frame.
            tr.end_time=arrival_time;tr.target_index=target_index;
            tr.id=m_next_trajectory_id++;
            tr.start_real=now;tr.end_real=now+std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(seconds));
            m_motion.current_target = target_index;
            m_motion.next_target = target_index + 1 < static_cast<int>( map.objects.size( ) ) ? target_index + 1 : -1;
            m_diag.trajectory_id=tr.id;
            if(timing_change){m_diag.timing_update_replans++;note_replan(replan_reason_t::material_schedule_change);}
            else if(recovery){m_diag.recovery_replans++;note_replan(replan_reason_t::recovery_required);}
            else {m_diag.target_change_replans++;note_replan(replan_reason_t::target_change);}
        }
        void evaluate_trajectory( std::chrono::steady_clock::time_point now, point_t& position,
                                  point_t& velocity, point_t& acceleration ) const {
            const trajectory_t& tr = m_motion.trajectory;
            if ( !tr.valid || tr.end_real <= tr.start_real ) {
                position=m_motion.position;velocity=m_motion.velocity;acceleration=m_motion.acceleration;return;
            }
            const float duration=std::chrono::duration<float>(tr.end_real-tr.start_real).count();
            const float t=saturate(std::chrono::duration<float>(now-tr.start_real).count()/duration);
            const point_t c0=tr.p0,c1=tr.v0*duration,c2=tr.a0*(0.5f*duration*duration);
            const point_t delta=tr.p1-c0-c1-c2;
            const point_t velocity_delta=tr.v1*duration-c1-c2*2.f;
            const point_t acceleration_delta=tr.a1*(duration*duration)-c2*2.f;
            const point_t c3=delta*10.f-velocity_delta*4.f+acceleration_delta*0.5f;
            const point_t c4=delta*-15.f+velocity_delta*7.f-acceleration_delta;
            const point_t c5=delta*6.f-velocity_delta*3.f+acceleration_delta*0.5f;
            const float t2=t*t,t3=t2*t,t4=t3*t,t5=t4*t;
            position=c0+c1*t+c2*t2+c3*t3+c4*t4+c5*t5;
            velocity=(c1+c2*(2.f*t)+c3*(3.f*t2)+c4*(4.f*t3)+c5*(5.f*t4))/duration;
            acceleration=(c2*2.f+c3*(6.f*t)+c4*(12.f*t2)+c5*(20.f*t3))/(duration*duration);
        }
        static point_t safe_playfield_point(point_t p){
            p.x=std::clamp(p.x,34.f,478.f);p.y=std::clamp(p.y,30.f,354.f);return p;
        }
        void add_decorative_point(point_t p,int duration,bool loop_end=false,bool pause=false,bool primitive_end=false){
            m_choreography.points.push_back({safe_playfield_point(p),std::clamp(duration,60,700),loop_end,pause,primitive_end});
        }
        int choose_pattern(int first,int last,int previous){
            int selected=first+static_cast<int>(random_unit()*static_cast<float>(last-first+1));
            selected=std::clamp(selected,first,last);
            if(selected==previous&&last>first)selected=selected==last?first:selected+1;
            return selected;
        }
        point_t make_staging_point(point_t target,float radius,bool startup){
            const float distance=radius*((startup?2.7f:2.4f)+random_unit()*(startup?3.7f:3.2f));
            const float angle=random_unit()*2.f*k_pi;
            return safe_playfield_point(target+point_t{std::cos(angle),std::sin(angle)}*distance);
        }
        void generate_restless_primitive(){
            if(m_choreography.waypoint>=static_cast<int>(m_choreography.points.size())){
                m_choreography.points.clear();m_choreography.waypoint=0;
            }
            const float e=std::clamp(m_choreography.energy,0.f,1.f);
            if(m_choreography.primitives>0&&m_choreography.primitives%static_cast<int>(3+random_unit()*4.f)==0){
                const float a=random_unit()*2.f*k_pi;
                const float r=m_choreography.hit_radius*(2.0f+random_unit()*(2.2f+e*2.0f));
                m_choreography.local_anchor=safe_playfield_point(m_choreography.staging+
                    point_t{std::cos(a),std::sin(a)}*r);
            }
            int choice=static_cast<int>(random_unit()*11.f);
            if(choice==m_choreography.last_primitive)choice=(choice+1+static_cast<int>(random_unit()*9.f))%11;
            m_choreography.last_primitive=choice;m_choreography.primitives++;
            const primitive_t primitive=static_cast<primitive_t>(choice);
            const float angle=random_unit()*2.f*k_pi,sign=random_unit()<.5f?-1.f:1.f;
            const point_t direction{std::cos(angle),std::sin(angle)},perp{-direction.y,direction.x};
            const point_t start=m_motion.position,anchor=m_choreography.local_anchor;
            const float local=12.f+e*30.f+random_unit()*(16.f+e*34.f);
            const bool broad=m_choreography.energetic&&random_unit()<(0.07f+0.16f*e);
            const float scale=broad?1.9f+random_unit()*1.6f:1.f;
            const int fast=static_cast<int>(70.f+random_unit()*(180.f-75.f*e));
            switch(primitive){
            case primitive_t::short_flick:
                add_decorative_point(start+direction*local*scale,fast,false,false,true);break;
            case primitive_t::micro_arc:
                add_decorative_point(start+direction*local*.55f+perp*sign*local*.30f,fast);
                add_decorative_point(start+direction*local+perp*sign*local*.10f,fast,false,false,true);break;
            case primitive_t::partial_loop:
            case primitive_t::small_loop:{
                const int count=primitive==primitive_t::small_loop?5:3;const float r=local*(primitive==primitive_t::small_loop?.55f:.82f)*scale;
                const point_t center=safe_playfield_point(start+perp*sign*r);
                const float phase=std::atan2(start.y-center.y,start.x-center.x);
                for(int i=1;i<=count;++i){const float a=phase+sign*(primitive==primitive_t::small_loop?2.f*k_pi:1.35f*k_pi)*i/count;
                    add_decorative_point(center+point_t{std::cos(a)*r,std::sin(a)*r*.82f},fast,false,false,i==count);}break;}
            case primitive_t::hook:
                add_decorative_point(start+direction*local*.65f,fast);
                add_decorative_point(start+direction*local+perp*sign*local*.48f,fast,false,false,true);break;
            case primitive_t::diagonal_sweep:
                add_decorative_point(start+direction*local*1.8f*scale,static_cast<int>(140+random_unit()*260),false,false,true);break;
            case primitive_t::reverse_sweep:
                add_decorative_point(start+direction*local*1.35f,fast);
                add_decorative_point(start-direction*local*.55f+perp*sign*local*.35f,fast+60,false,false,true);break;
            case primitive_t::s_curve:
                add_decorative_point(start+direction*local*.35f+perp*sign*local*.45f,fast);
                add_decorative_point(start+direction*local*.72f-perp*sign*local*.42f,fast);
                add_decorative_point(start+direction*local*1.15f,fast,false,false,true);break;
            case primitive_t::local_reposition:
                add_decorative_point(anchor+direction*(random_unit()*local*.35f),static_cast<int>(180+random_unit()*400),false,false,true);break;
            case primitive_t::quick_circle:{
                const float r=local*.5f;const point_t center=safe_playfield_point(start+perp*sign*r);
                const float phase=std::atan2(start.y-center.y,start.x-center.x);
                for(int i=1;i<=6;++i){const float a=phase+sign*2.f*k_pi*i/6.f;
                    add_decorative_point(center+point_t{std::cos(a)*r,std::sin(a)*r},60+static_cast<int>(random_unit()*45.f),i==6,false,i==6);}break;}
            case primitive_t::pause:
                add_decorative_point(start,static_cast<int>(65+random_unit()*180.f),false,true,true);break;
            }
            if(m_choreography.startup)m_diag.startup_primitives_generated++;
            else m_diag.break_primitives_generated++;
        }
        void begin_choreography(bool startup,point_t target,float radius,int available_ms,int return_ms){
            m_choreography={};m_choreography.active=true;m_choreography.startup=startup;
            m_choreography.points.reserve(12);m_choreography.target=target;m_choreography.hit_radius=radius;
            m_choreography.staging=make_staging_point(target,radius,startup);
            m_choreography.local_anchor=m_choreography.staging;
            m_choreography.energy=startup?startup_energy:break_energy;
            if(startup){
                const bool allow_energy=energetic_dances&&available_ms>return_ms+900;
                m_choreography.pattern=allow_energy?choose_pattern(0,2,m_last_startup_pattern):choose_pattern(3,7,m_last_startup_pattern);
                m_choreography.energetic=m_choreography.pattern<=2;m_last_startup_pattern=m_choreography.pattern;
                m_diag.startup_pattern=m_choreography.pattern+1;
            }else{
                const bool allow_energy=energetic_dances&&available_ms>return_ms+1800&&random_unit()<(0.25f+0.45f*break_energy);
                m_choreography.pattern=allow_energy?choose_pattern(2,3,m_last_break_pattern):choose_pattern(0,1,m_last_break_pattern);
                m_choreography.energetic=m_choreography.pattern>=2;m_last_break_pattern=m_choreography.pattern;
                m_diag.break_pattern=m_choreography.pattern+1;
            }
            generate_restless_primitive();
            set_destination_source(startup?destination_source_t::startup_choreography:
                destination_source_t::break_choreography,source_change_reason_t::choreography_start,-1);
        }
        void stop_choreography(bool interrupted){
            if(!m_choreography.active)return;
            if(interrupted&&m_choreography.waypoint<static_cast<int>(m_choreography.points.size()))m_diag.decorative_interruptions++;
            m_choreography.active=false;m_diag.decorative_state=decorative_state_t::none;
            m_diag.decorative_remaining_ms=0;m_diag.decorative_return_ms=0;
        }
        void start_decorative_segment(int game_time,std::chrono::steady_clock::time_point now){
            if(m_choreography.waypoint>=static_cast<int>(m_choreography.points.size()))generate_restless_primitive();
            const auto& point=m_choreography.points[static_cast<size_t>(m_choreography.waypoint)];
            const int duration=point.duration_ms;trajectory_t tr{};tr.valid=true;tr.p0=m_motion.position;tr.p1=point.position;
            tr.v0=m_motion.velocity;tr.a0=m_motion.acceleration;
            point_t next_direction=normalized(point.position-m_motion.position);
            if(m_choreography.waypoint+1<static_cast<int>(m_choreography.points.size()))
                next_direction=normalized(m_choreography.points[static_cast<size_t>(m_choreography.waypoint+1)].position-point.position);
            tr.v1=point.pause?point_t{}:next_direction*(length(point.position-m_motion.position)/std::max(duration*.001f,.09f)*.62f);
            tr.start_time=game_time;tr.end_time=game_time+duration;tr.target_index=-100-m_choreography.waypoint;
            tr.id=m_next_trajectory_id++;
            tr.start_real=now;tr.end_real=now+std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float,std::milli>(duration/std::max(m_game_rate,.5f)));
            m_motion.trajectory=tr;m_choreography.segment_end_time=game_time+duration;
            m_diag.trajectory_id=tr.id;note_replan(replan_reason_t::choreography_segment);
            set_destination_source(m_choreography.startup?destination_source_t::startup_choreography:
                destination_source_t::break_choreography,source_change_reason_t::choreography_segment,-1);
            m_diag.decorative_state=point.pause?decorative_state_t::pause:
                m_choreography.waypoint+1>=static_cast<int>(m_choreography.points.size())?decorative_state_t::staging:decorative_state_t::moving;
        }
        bool update_choreography(int game_time,std::chrono::steady_clock::time_point now,bool startup,int target_index,
                                 point_t target,float radius,int time_left,int return_ms,point_t& position,
                                 point_t& velocity,point_t& acceleration){
            (void)target_index;
            if(!m_choreography.active||m_choreography.startup!=startup)
                begin_choreography(startup,target,radius,time_left,return_ms);
            const int safety=startup?260:320;
            m_diag.decorative_return_ms=return_ms;
            if(time_left<=return_ms+safety){stop_choreography(true);return false;}
            if(m_choreography.segment_end_time>0&&game_time>=m_choreography.segment_end_time){
                const auto& completed=m_choreography.points[static_cast<size_t>(m_choreography.waypoint)];
                if(startup)m_diag.startup_segments_completed++;
                else if(completed.loop_end)m_diag.break_dance_loops_completed++;
                m_choreography.waypoint++;m_choreography.segment_end_time=0;
            }
            if(m_choreography.segment_end_time==0){
                if(m_choreography.waypoint>=static_cast<int>(m_choreography.points.size())){
                    const int minimum_budget=80+static_cast<int>(220.f*(1.f-m_choreography.energy));
                    if(time_left<=return_ms+safety+minimum_budget){stop_choreography(false);return false;}
                    generate_restless_primitive();
                }
                start_decorative_segment(game_time,now);
            }
            int remaining=std::max(m_choreography.segment_end_time-game_time,0);
            for(size_t i=static_cast<size_t>(m_choreography.waypoint+1);i<m_choreography.points.size();++i)
                remaining+=m_choreography.points[i].duration_ms;
            m_diag.decorative_remaining_ms=remaining;
            m_destination_owner=startup?destination_owner_t::startup:
                m_choreography.energetic?destination_owner_t::break_dance:destination_owner_t::break_idle;
            evaluate_trajectory(now,position,velocity,acceleration);
            m_motion.state=startup?movement_state_t::idle:movement_state_t::break_idle;
            return true;
        }
        void update_idle( int game_time, bool after_map, point_t& desired_position,
                          point_t& desired_velocity, point_t& desired_acceleration ) {
            const bool was_idle = m_motion.state == movement_state_t::idle ||
                m_motion.state == movement_state_t::break_idle;
            if ( !was_idle ) {
                m_idle_anchor = m_motion.position;
                m_idle_segment_end = game_time;
                m_motion.trajectory.valid = false;
            }
            if ( !m_motion.trajectory.valid || m_motion.trajectory.target_index != -2 || game_time >= m_idle_segment_end ) {
                const bool startup=m_last_completed_index<0&&!after_map;
                const float calm = after_map ? 0.42f : 1.f;
                const float energy = std::clamp( drift_amount, 0.f, 5.f ) * m_style.idle_energy * calm;
                const float angle = random_unit( ) * 2.f * k_pi;
                const float radius=(startup?10.f:4.f)+energy*((startup?15.f:7.f)+random_unit()*(startup?26.f:12.f));
                const bool pause=random_unit()<(startup?0.10f:0.24f);
                point_t destination=pause?m_motion.position:
                    m_idle_anchor+point_t{std::cos(angle),std::sin(angle)}*radius;
                destination.x=std::clamp(destination.x,32.f,480.f); destination.y=std::clamp(destination.y,28.f,356.f);
                const int duration=pause?static_cast<int>(90.f+random_unit()*130.f):
                    static_cast<int>((startup?130.f:300.f)+random_unit()*(startup?300.f:520.f));
                m_idle_segment_end=game_time+duration;
                trajectory_t tr{};tr.valid=true;tr.p0=m_motion.position;tr.p1=destination;
                tr.v0=m_motion.velocity*0.55f;const point_t radial=normalized(destination-m_motion.position);
                tr.v1=pause?point_t{}:point_t{-radial.y,radial.x}*(startup?90.f+energy*80.f:35.f);
                tr.a0=m_motion.acceleration;tr.start_time=game_time;tr.end_time=m_idle_segment_end;tr.target_index=-2;
                tr.id=m_next_trajectory_id++;
                tr.start_real=m_motion.last_real_update;tr.end_real=tr.start_real+
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<float,std::milli>(duration/std::max(m_game_rate,0.5f)));
                m_motion.trajectory=tr;m_diag.trajectory_id=tr.id;
                note_replan(replan_reason_t::state_change);m_diag.fidget_segments++;
            }
            evaluate_trajectory(m_motion.last_real_update,desired_position,desired_velocity,desired_acceleration);
            m_motion.state=after_map?movement_state_t::break_idle:movement_state_t::idle;
        }

        void update_slider( int game_time, int index, const osu::hit_object_t& obj, const osu::beatmap_data_t& map,
                            point_t& desired_position, point_t& desired_velocity, float& max_speed, float& max_acceleration ) {
            const point_t ball=evaluate_slider(index,obj,game_time), future=evaluate_slider(index,obj,std::min(game_time+8,obj.end_time));
            desired_velocity=(future-ball)/0.008f;
            const float lazy=std::clamp(slider_laziness,0.f,1.f) * (2.f-m_style.slider_tightness);
            desired_position=ball+(m_motion.position-ball)*(lazy*0.045f);
            m_active_slider_ball=ball;m_active_slider_ball_valid=true;
            if(m_last_slider_index!=index){m_last_slider_index=index;m_diag.slider_state_replans++;
                m_diag.trajectory_id=m_next_trajectory_id++;note_replan(replan_reason_t::slider_change);}
            if(obj.end_time-game_time<=120 && index+1<static_cast<int>(map.objects.size())) {
                const point_t exit_dir=normalized(target_point(index+1,map)-ball);
                const float blend=1.f-smoothstep(0.f,120.f,static_cast<float>(obj.end_time-game_time));
                desired_position+=exit_dir*hit_radius(map)*0.10f*lazy*blend;
                const float exit_speed=std::max(length(desired_velocity),480.f);
                desired_velocity=desired_velocity*(1.f-0.20f*blend)+exit_dir*exit_speed*(0.20f*blend);
                m_motion.state=movement_state_t::slider_exit;
            } else m_motion.state=movement_state_t::slider_follow;
            max_speed=4200.f; max_acceleration=36000.f*(1.f-lazy*0.18f);
        }
        void begin_spinner( int index, const osu::hit_object_t& obj ) {
            m_spinner.active=true; m_spinner.object_index=index; m_spinner.center=object_center(obj);
            point_t radial=m_motion.position-m_spinner.center; m_spinner.radius=std::clamp(length(radial),52.f,82.f);
            if(length_sq(radial)<1.f) radial={1.f,0.f}; m_spinner.angle=std::atan2(radial.y,radial.x);
            const point_t tangent{-std::sin(m_spinner.angle),std::cos(m_spinner.angle)};
            m_spinner.direction=dot(m_motion.velocity,tangent)>=0.f?1.f:-1.f;
            if(length_sq(m_motion.velocity)<100.f) m_spinner.direction=random_unit()>=0.5f?1.f:-1.f;
            const float incoming_angular_velocity = dot( m_motion.velocity, tangent ) /
                std::max( m_spinner.radius, 1.f );
            m_spinner.angular_velocity = std::abs( incoming_angular_velocity ) * m_spinner.direction;
            m_spinner.radius_velocity=0.f; m_diag.spinner_entry_speed=length(m_motion.velocity);
            m_diag.trajectory_id=m_next_trajectory_id++;note_replan(replan_reason_t::spinner_change);
        }
        void update_spinner( int game_time,int index,const osu::hit_object_t& obj,float dt,
                             point_t& desired_position,point_t& desired_velocity,float& max_speed,float& max_acceleration,
                             const osu::beatmap_data_t& map,const relax::c_relax& timing ) {
            if(!m_spinner.active||m_spinner.object_index!=index) begin_spinner(index,obj);
            const float duration=std::max(static_cast<float>(obj.end_time-obj.start_time),1.f);
            const float progress=saturate(static_cast<float>(game_time-obj.start_time)/duration);
            const float entry=smoothstep(0.f,0.16f,progress), exit=1.f-smoothstep(0.82f,1.f,progress);
            const float rpm_drift=1.f+0.025f*std::sin(m_style.phase+game_time*0.0013f);
            m_diag.spinner_target_rpm=std::clamp(spinner_rpm,200.f,477.f)*rpm_drift;
            const float target=std::clamp(spinner_rpm,200.f,477.f)*2.f*k_pi/60.f*rpm_drift*entry*(0.72f+0.28f*exit);
            const float angular_accel=std::clamp((target-std::abs(m_spinner.angular_velocity))*8.f,-42.f,42.f);
            const float angular_speed=std::max(0.f,std::abs(m_spinner.angular_velocity)+angular_accel*dt);
            m_spinner.angular_velocity=angular_speed*m_spinner.direction; m_spinner.angle+=m_spinner.angular_velocity*dt;
            const float target_radius=68.f+3.f*std::sin(m_style.phase*0.7f+game_time*0.0009f);
            const float radius_accel=(target_radius-m_spinner.radius)*20.f-m_spinner.radius_velocity*8.f;
            m_spinner.radius_velocity+=std::clamp(radius_accel,-180.f,180.f)*dt;
            m_spinner.radius=std::clamp(m_spinner.radius+m_spinner.radius_velocity*dt,52.f,82.f);
            const float ellipse=0.96f+0.025f*std::sin(m_style.phase+game_time*0.0007f);
            desired_position=m_spinner.center+point_t{std::cos(m_spinner.angle)*m_spinner.radius,
                std::sin(m_spinner.angle)*m_spinner.radius*ellipse};
            desired_velocity={-std::sin(m_spinner.angle)*m_spinner.radius*m_spinner.angular_velocity,
                std::cos(m_spinner.angle)*m_spinner.radius*ellipse*m_spinner.angular_velocity};
            const float exit_blend=smoothstep(0.82f,1.f,progress);
            if(exit_blend>0.f&&index+1<static_cast<int>(map.objects.size())){
                const point_t next=target_point(index+1,map),exit_direction=normalized(next-desired_position);
                const int next_time=scheduled_press_time(index+1,map.objects[static_cast<size_t>(index+1)],timing);
                const float available=std::max((next_time-game_time)*0.001f,0.08f);
                const float exit_speed=std::clamp(length(next-desired_position)/available,420.f,2600.f);
                desired_position+=exit_direction*(10.f*exit_blend);
                desired_velocity=desired_velocity*(1.f-0.55f*exit_blend)+exit_direction*exit_speed*(0.55f*exit_blend);
            }
            m_motion.state=progress<0.16f?movement_state_t::spinner_entry:
                progress>0.82f?movement_state_t::spinner_exit:movement_state_t::spinner_sustain;
            max_speed=5200.f; max_acceleration=48000.f;
            const float rpm=std::abs(m_spinner.angular_velocity)*60.f/(2.f*k_pi);
            m_diag.spinner_samples++; m_diag.spinner_rpm_sum+=rpm; m_diag.spinner_rpm_peak=std::max(m_diag.spinner_rpm_peak,rpm);
            m_diag.spinner_radius_min=std::min(m_diag.spinner_radius_min,m_spinner.radius);
            m_diag.spinner_radius_max=std::max(m_diag.spinner_radius_max,m_spinner.radius);
            m_diag.spinner_radius_sum+=m_spinner.radius;
            if(progress>0.98f) m_diag.spinner_exit_speed=length(m_motion.velocity);
        }
        void apply_motion_sample( point_t desired_position,point_t desired_velocity,
                                  point_t desired_acceleration,float dt ) {
            const point_t old_position=m_motion.position, old_velocity=m_motion.velocity;
            const point_t old_acceleration=m_motion.acceleration;
            const bool dynamic_path=m_motion.state==movement_state_t::slider_follow||
                m_motion.state==movement_state_t::slider_exit||m_motion.state==movement_state_t::spinner_entry||
                m_motion.state==movement_state_t::spinner_sustain||m_motion.state==movement_state_t::spinner_exit;
            if(!std::isfinite(desired_position.x)||!std::isfinite(desired_position.y)||
               !std::isfinite(desired_velocity.x)||!std::isfinite(desired_velocity.y)){
                m_diag.internal_trajectory_discontinuities++;
                m_diag.teleport_discontinuities=m_diag.internal_trajectory_discontinuities;
                return;
            }
            m_diag.internal_desired_delta=desired_position-old_position;
            const point_t sampled_velocity=dynamic_path?
                (desired_position-old_position)/std::max(dt,0.001f):desired_velocity;
            if(dynamic_path)desired_acceleration=(sampled_velocity-old_velocity)/std::max(dt,0.001f);
            m_motion.position=desired_position;m_motion.velocity=sampled_velocity;m_motion.acceleration=desired_acceleration;
            const float displacement=length(m_motion.position-old_position);
            m_motion.last_frame_displacement=displacement;
            const float kinematic_bound=std::max(length(old_velocity),length(sampled_velocity))*dt+
                0.5f*std::max(length(old_acceleration),length(desired_acceleration))*dt*dt+2.f;
            if(displacement>kinematic_bound+0.01f){
                m_diag.internal_trajectory_discontinuities++;
                record_bad_delta(old_position,desired_position,displacement,kinematic_bound);
            }
            m_diag.teleport_discontinuities=m_diag.internal_trajectory_discontinuities;
            m_diag.max_frame_displacement=std::max(m_diag.max_frame_displacement,displacement);
            const float speed=length(m_motion.velocity), a=length(m_motion.acceleration);
            const float desired_speed=length(desired_velocity);
            m_diag.motion_samples++; m_diag.speed_sum+=speed; m_diag.acceleration_sum+=a;
            m_diag.desired_speed_sum+=desired_speed;m_diag.desired_speed_peak=std::max(m_diag.desired_speed_peak,desired_speed);
            const float tracking_error=length(m_motion.position-desired_position);
            m_diag.tracking_error_sum+=tracking_error;m_diag.tracking_error_peak=std::max(m_diag.tracking_error_peak,tracking_error);
            const float lag_ms=desired_speed>1.f?tracking_error/desired_speed*1000.f:0.f;
            m_diag.trajectory_lag_ms_sum+=lag_ms;m_diag.trajectory_lag_ms_peak=std::max(m_diag.trajectory_lag_ms_peak,lag_ms);
            m_diag.peak_speed=std::max(m_diag.peak_speed,speed); m_diag.peak_acceleration=std::max(m_diag.peak_acceleration,a);
            if(length_sq(old_velocity)>1.f&&length_sq(m_motion.velocity)>1.f) {
                const float cosine=std::clamp(dot(old_velocity,m_motion.velocity)/(length(old_velocity)*speed),-1.f,1.f);
                m_diag.curvature_sum+=std::acos(cosine);
            }
            if(m_motion.state==movement_state_t::idle||m_motion.state==movement_state_t::break_idle) m_diag.idle_distance+=displacement;
            if(m_destination_owner==destination_owner_t::startup||m_destination_owner==destination_owner_t::break_idle||
               m_destination_owner==destination_owner_t::break_dance)m_diag.decorative_distance+=displacement;
            m_motion.previous_velocity=old_velocity;
        }
        void emit_cursor( const RECT& window ) {
            const auto p=playfield::playfield_to_screen(m_motion.position.x,m_motion.position.y,window);
            // Absolute injection avoids Windows relative-pointer acceleration while
            // every requested point still comes from the continuous trajectory.
            // There is intentionally no output displacement cap.
            if(m_motion.last_output_valid)m_diag.final_requested_delta={
                static_cast<float>(p.x-m_motion.last_output_screen.x),
                static_cast<float>(p.y-m_motion.last_output_screen.y)};
            if(input::move_absolute_virtual_desktop(p.x,p.y)){
                m_motion.last_output_screen={p.x,p.y};m_motion.last_output_valid=true;
            }
            else {m_motion.trajectory.valid=false;m_diag.invalid_data_replans++;note_replan(replan_reason_t::invalid_trajectory);}
        }
        void record_slider_error( const RECT& window ) {
            if(!m_active_slider_ball_valid)return;
            point_t observed=m_motion.position;POINT cursor{};float x=0.f,y=0.f;
            if(input::get_cursor_pos(&cursor)&&playfield::screen_to_playfield(cursor.x,cursor.y,window,x,y))observed={x,y};
            const float error=length(observed-m_active_slider_ball);
            m_diag.slider_samples++;m_diag.slider_error_sum+=error;
            m_diag.slider_error_peak=std::max(m_diag.slider_error_peak,error);
        }
        void record_arrivals( int previous_time,int current_time,const osu::beatmap_data_t& map,
                              const relax::c_relax& timing,const RECT& window ) {
            while(m_arrival_recorded_through+1<static_cast<int>(map.objects.size())){
                const int index=m_arrival_recorded_through+1;
                const auto& obj=map.objects[static_cast<size_t>(index)];
                const int press=scheduled_press_time(index,obj,timing);
                if(press>current_time)break;
                m_arrival_recorded_through=index;
                if(press<previous_time||is_spinner_object(obj))continue;
                const point_t selected=target_point(index,map),center=object_center(obj);
                const auto target_screen=playfield::playfield_to_screen(selected.x,selected.y,window);
                const auto center_screen=playfield::playfield_to_screen(center.x,center.y,window);
                const auto planned_screen=playfield::playfield_to_screen(m_motion.position.x,m_motion.position.y,window);
                POINT observed{planned_screen.x,planned_screen.y};input::get_cursor_pos(&observed);
                const playfield::screen_point_t cursor_screen{observed.x,observed.y};
                const float ex=static_cast<float>(cursor_screen.x-target_screen.x),ey=static_cast<float>(cursor_screen.y-target_screen.y);
                const float error=std::sqrt(ex*ex+ey*ey);
                const float scale=std::max(static_cast<float>(window.bottom-window.top)*0.8f/384.f,0.25f);
                const float radius=hit_radius(map)*scale;
                m_diag.arrival_samples++;m_diag.arrival_error_sum+=error;m_diag.arrival_error_peak=std::max(m_diag.arrival_error_peak,error);
                if(error<=radius)m_diag.arrival_inside_full++;
                if(error<=radius*0.75f)m_diag.arrival_inside_075++;
                if(error<=radius*0.50f)m_diag.arrival_inside_050++;
                m_diag.last_arrival_object_screen_x=center_screen.x;m_diag.last_arrival_object_screen_y=center_screen.y;
                m_diag.last_arrival_target_screen_x=target_screen.x;m_diag.last_arrival_target_screen_y=target_screen.y;
                m_diag.last_arrival_cursor_screen_x=cursor_screen.x;m_diag.last_arrival_cursor_screen_y=cursor_screen.y;
                m_diag.last_arrival_press_time=press;m_diag.last_arrival_hit_radius_screen=radius;
                m_diag.last_arrival_error_screen=error;
            }
        }
        void update_completed_objects( int gt,const osu::beatmap_data_t& map,const relax::c_relax& timing ) {
            while(m_last_completed_index+1<static_cast<int>(map.objects.size())) {
                const int next=m_last_completed_index+1;
                if(gt<scheduled_press_time(next,map.objects[static_cast<size_t>(next)],timing)) break;
                m_last_completed_index=next; m_diag.objects_completed++;
            }
        }
        void update_schedule_diagnostics( const osu::beatmap_data_t& map,const relax::c_relax& timing ) {
            m_diag.requested_accuracy=target_accuracy;
            for(int i=m_diag_scheduled_through+1;i<static_cast<int>(map.objects.size());++i) {
                relax::c_relax::scheduled_object_t s{}; if(!timing.scheduled_object(static_cast<size_t>(i),s)) break;
                if(i<static_cast<int>(m_accuracy_edge_bias.size()))
                    m_accuracy_edge_bias[static_cast<size_t>(i)]=s.accuracy_edge_bias;
                s.key_index==0?m_diag.k1_count++:m_diag.k2_count++;
                if(s.tap_state==relax::tap_style_state_t::singletap)m_diag.singletap_count++;else m_diag.alternate_count++;
                const double offset=static_cast<double>(s.press_time-map.objects[static_cast<size_t>(i)].start_time);
                m_diag.accuracy_score_sum+=s.simulated_grade==300?1.0:s.simulated_grade==100?1.0/3.0:0.0;
                m_diag.accuracy_samples++;
                m_diag.timing_samples++;m_diag.timing_sum+=offset;m_diag.timing_sq_sum+=offset*offset;m_diag_scheduled_through=i;
            }
            m_diag.session_accuracy=m_diag.accuracy_samples?static_cast<float>(m_diag.accuracy_score_sum/
                static_cast<double>(m_diag.accuracy_samples)*100.0):100.f;
            m_diag.accuracy_bias=m_diag.session_accuracy-target_accuracy;
            m_diag.accuracy_state=m_diag.accuracy_bias>0.20f?1:m_diag.accuracy_bias<-0.20f?-1:0;
            const auto acc=timing.accuracy_telemetry();
            m_diag.controlled_100=acc.controlled_100;
            m_diag.predicted_accuracy=acc.predicted;
            m_diag.accuracy_debt=acc.debt;
        }

        static point_t evaluate_bezier( const std::vector<point_t>& controls,float t ) {
            if(controls.empty())return{}; if(controls.size()==2)return controls[0]+(controls[1]-controls[0])*t;
            std::vector<point_t> work=controls;
            for(size_t step=1;step<work.size();++step)for(size_t i=0;i+step<work.size();++i)work[i]=work[i]+(work[i+1]-work[i])*t;
            return work[0];
        }
        static bool same_point( point_t a, point_t b ) { return length_sq( a - b ) < 0.0001f; }
        static void append_bezier_segment( std::vector<point_t>& path,
                                           const std::vector<point_t>& controls ) {
            if ( controls.size( ) < 2 ) return;
            const int samples = std::clamp( static_cast<int>( controls.size( ) * 18 ), 18, 96 );
            for ( int i = path.empty( ) ? 0 : 1; i <= samples; ++i )
                path.push_back( evaluate_bezier( controls, static_cast<float>( i ) / samples ) );
        }
        static bool append_perfect_curve( std::vector<point_t>& path,
                                          const std::vector<point_t>& controls ) {
            if ( controls.size( ) != 3 ) return false;
            const point_t a=controls[0], b=controls[1], c=controls[2];
            const float d=2.f*(a.x*(b.y-c.y)+b.x*(c.y-a.y)+c.x*(a.y-b.y));
            if(std::abs(d)<0.001f)return false;
            const float aa=length_sq(a),bb=length_sq(b),cc=length_sq(c);
            const point_t center{(aa*(b.y-c.y)+bb*(c.y-a.y)+cc*(a.y-b.y))/d,
                (aa*(c.x-b.x)+bb*(a.x-c.x)+cc*(b.x-a.x))/d};
            const float start=std::atan2(a.y-center.y,a.x-center.x);
            const float middle=std::atan2(b.y-center.y,b.x-center.x);
            const float finish=std::atan2(c.y-center.y,c.x-center.x);
            auto positive=[](float x){while(x<0.f)x+=2.f*k_pi;while(x>=2.f*k_pi)x-=2.f*k_pi;return x;};
            const float ccw_finish=positive(finish-start),ccw_middle=positive(middle-start);
            const float sweep=ccw_middle<=ccw_finish?ccw_finish:ccw_finish-2.f*k_pi;
            const float radius=length(a-center);const int samples=std::clamp(static_cast<int>(std::abs(sweep)*radius/5.f),18,128);
            for(int i=0;i<=samples;++i){const float angle=start+sweep*static_cast<float>(i)/samples;
                path.push_back(center+point_t{std::cos(angle),std::sin(angle)}*radius);}return true;
        }
        static void append_catmull( std::vector<point_t>& path,const std::vector<point_t>& controls ) {
            if(controls.size()<2)return;
            for(size_t segment=0;segment+1<controls.size();++segment){
                const point_t p0=segment>0?controls[segment-1]:controls[segment];
                const point_t p1=controls[segment],p2=controls[segment+1];
                const point_t p3=segment+2<controls.size()?controls[segment+2]:p2;
                constexpr int samples=20;
                for(int i=(path.empty()?0:1);i<=samples;++i){const float t=static_cast<float>(i)/samples,t2=t*t,t3=t2*t;
                    path.push_back((p1*2.f+(p2-p0)*t+(p0*2.f-p1*5.f+p2*4.f-p3)*t2+
                        (p0*-1.f+p1*3.f-p2*3.f+p3)*t3)*0.5f);}
            }
        }
        std::vector<point_t> generate_slider_path( const osu::hit_object_t& obj ) const {
            std::vector<point_t> controls;controls.push_back(object_center(obj));char type='L';
            if(!obj.slider_curve_str.empty()) {
                std::stringstream stream(obj.slider_curve_str);std::string item;bool first=true;
                while(std::getline(stream,item,'|')) {
                    if(first){if(!item.empty())type=item[0];first=false;continue;}
                    const size_t colon=item.find(':');if(colon==std::string::npos)continue;
                    try{point_t p{std::stof(item.substr(0,colon)),std::stof(item.substr(colon+1))};if(m_hr_flip)p.y=384.f-p.y;controls.push_back(p);}catch(...){ }
                }
            }
            if(controls.size()<2||type=='L')return controls;
            std::vector<point_t> path;path.reserve(128);
            if(type=='P'&&append_perfect_curve(path,controls))return path;
            if(type=='C'){append_catmull(path,controls);return path;}
            size_t segment_start=0;
            for(size_t i=1;i+1<controls.size();++i)if(same_point(controls[i],controls[i+1])){
                append_bezier_segment(path,std::vector<point_t>(controls.begin()+segment_start,controls.begin()+i+1));
                segment_start=i+1;
            }
            append_bezier_segment(path,std::vector<point_t>(controls.begin()+segment_start,controls.end()));
            return path;
        }
        const cached_slider_t& slider_path( int index,const osu::hit_object_t& obj ) {
            if(index<0||index>=static_cast<int>(m_slider_cache.size())){
                static cached_slider_t empty{};return empty;
            }
            auto& slot=m_slider_cache[static_cast<size_t>(index)];
            if(slot.start_time==obj.start_time)return slot;
            cached_slider_t c{};c.start_time=obj.start_time;c.path=generate_slider_path(obj);
            if(c.path.size()>=2&&obj.slider_length>0.f){
                std::vector<point_t> trimmed;trimmed.push_back(c.path.front());float covered=0.f;
                for(size_t i=1;i<c.path.size()&&covered<obj.slider_length;++i){const float segment=length(c.path[i]-c.path[i-1]);
                    if(covered+segment>=obj.slider_length){const float u=segment>0.f?(obj.slider_length-covered)/segment:0.f;
                        trimmed.push_back(c.path[i-1]+(c.path[i]-c.path[i-1])*u);covered=obj.slider_length;break;}
                    trimmed.push_back(c.path[i]);covered+=segment;}
                if(covered<obj.slider_length&&trimmed.size()>=2){const point_t direction=normalized(trimmed.back()-trimmed[trimmed.size()-2]);
                    trimmed.push_back(trimmed.back()+direction*(obj.slider_length-covered));}
                c.path=std::move(trimmed);
            }
            c.distances.push_back(0.f);
            for(size_t i=1;i<c.path.size();++i){c.total_distance+=length(c.path[i]-c.path[i-1]);c.distances.push_back(c.total_distance);}
            slot=std::move(c);return slot;
        }
        point_t evaluate_slider( int index,const osu::hit_object_t& obj,int gt ) {
            const auto& c=slider_path(index,obj);if(c.path.size()<2||c.total_distance<=0.f)return object_center(obj);
            const float duration=static_cast<float>(std::max(obj.end_time-obj.start_time,1));
            const float t=saturate(static_cast<float>(gt-obj.start_time)/duration);const int spans=std::max(obj.slider_repeat,1);
            const float span_progress=t*spans;int span=static_cast<int>(span_progress);float local=span_progress-span;
            if(span>=spans)local=spans%2==1?1.f:0.f;else if(span%2==1)local=1.f-local;
            const float target=local*c.total_distance;
            for(size_t i=1;i<c.path.size();++i)if(target<=c.distances[i]){
                const float segment=c.distances[i]-c.distances[i-1];const float u=segment>0.f?(target-c.distances[i-1])/segment:0.f;
                return c.path[i-1]+(c.path[i]-c.path[i-1])*u;
            }return c.path.back();
        }
    };
}
