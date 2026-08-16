#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <core/aim_assist/a_core.hxx>
#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <impl/util/playfield.hxx>
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <climits>
#include <chrono>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>
#include <memory>

namespace aim_assist {

    struct aim_snapshot_t {
        std::vector<assist::waypoint_t> targets;
        RECT window{};
        float speed_mult = 1.f;
        bool hr = false;
        bool ez = false;
        float cs = 5.f;
        float adaptive_difficulty = 0.f;
        float effective_strength = 62.f;
        float effective_smoothing_ms = 78.f;
        float effective_max_correction = 0.78f;
        float effective_anticipation_ms = 520.f;
        float adaptive_boost = 0.f;
        float large_jump_demand = 0.f;
    };

    struct aim_verification_t {
        uint64_t aim_reports = 0;
        uint64_t target_bearing_reports = 0;
        uint64_t relevant_approach_reports = 0;
        uint64_t engaged_reports = 0;
        uint64_t non_zero_corrections = 0;
        uint64_t requested_non_zero_samples = 0;
        uint64_t observed_non_zero_samples = 0;
        double requested_correction_sum = 0.0;
        double observed_output_delta_sum = 0.0;
        float peak_requested_correction = 0.f;
        float peak_observed_output_delta = 0.f;
        float adaptive_difficulty = 0.f;
        float effective_strength = 62.f;
        float effective_smoothing_ms = 78.f;
        float effective_max_correction = 0.78f;
        float effective_anticipation_ms = 520.f;
        uint64_t rejected_safe_trajectory = 0;
        uint64_t rejected_distance = 0;
        uint64_t rejected_direction = 0;
        uint64_t rejected_timing = 0;
        uint64_t corrected_reports = 0;
        uint64_t max_correction_clamp_hits = 0;
        float safe_destination_multiplier = 0.f;
        uint64_t relevant_predicted_miss_samples = 0;
        uint64_t corrected_predicted_miss_samples = 0;
        uint64_t corrected_observed_samples = 0;
        double relevant_predicted_miss_sum = 0.0;
        double corrected_predicted_miss_sum = 0.0;
        double corrected_observed_sum = 0.0;
        float peak_corrected_predicted_miss = 0.f;

        double adaptive_sample_seconds = 0.0;
        double adaptive_difficulty_time_sum = 0.0;
        float adaptive_difficulty_peak = 0.f;
        double difficulty_bucket_seconds[ 4 ]{};
        double effective_strength_time_sum = 0.0;
        double effective_smoothing_time_sum = 0.0;
        double effective_max_correction_time_sum = 0.0;
        float effective_strength_min = 0.f;
        float effective_strength_max = 0.f;
        float effective_smoothing_min = 0.f;
        float effective_smoothing_max = 0.f;
        float effective_max_correction_min = 0.f;
        float effective_max_correction_max = 0.f;
        float effective_anticipation_min = 0.f;
        float effective_anticipation_max = 0.f;
        uint64_t adaptive_hard_pattern_activations = 0;
        uint64_t corrected_by_difficulty[ 4 ]{};
        uint64_t high_difficulty_rescue_activations = 0;
        uint64_t large_jump_rescue_activations = 0;
        uint64_t low_medium_correction_samples = 0;
        uint64_t high_extreme_correction_samples = 0;
        double low_medium_correction_sum = 0.0;
        double high_extreme_correction_sum = 0.0;
        uint64_t first_rescue_miss_samples = 0;
        uint64_t final_correction_miss_samples = 0;
        double first_rescue_miss_sum = 0.0;
        double final_correction_miss_sum = 0.0;
    };

    class c_aimbot {
    public:
        bool  enabled          = true;
        bool  ignore_sliders   = false;
        bool  tablet_mode      = true;
        float assist_strength  = 62.f;
        float assist_radius    = 2.55f;
        float smoothing_ms     = 78.f;
        float max_correction   = 0.78f;
        bool  adaptive_aim     = false;
        float adaptation_strength = 60.f;
        void start() {
            clear_motion_state();
        }

        void stop() {
            clear_motion_state();
        }

        void on_leave_play( ) {
            m_in_play.store( false );
            m_user_blocked.store( false );
            clear_motion_state( );
            {
                std::lock_guard<std::mutex> slock( m_snap_mutex );
                m_shared_snap = std::make_shared<aim_snapshot_t>( );
            }
        }

        void set_user_input_blocked( bool blocked ) {
            const bool was = m_user_blocked.exchange( blocked );
            if ( blocked && !was ) {
                clear_motion_state( );
            }
        }

        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            if ( m_user_blocked.load( ) ) {
                m_in_play.store( false );
                return;
            }

            const bool in_play = enabled
                                 && game.cur_state == osu::game_state_t::play
                                 && map.loaded
                                 && !map.objects.empty( );
            m_in_play.store( in_play );

            if ( !in_play ) {
                {
                    std::lock_guard<std::mutex> slock( m_snap_mutex );
                    m_shared_snap = std::make_shared<aim_snapshot_t>( );
                }
                return;
            }

            const HWND hwnd = input::target_window( );
            RECT window{};
            if ( !hwnd || !playfield::get_playfield_rect( hwnd, window ) ) {
                {
                    std::lock_guard<std::mutex> slock( m_snap_mutex );
                    m_shared_snap = std::make_shared<aim_snapshot_t>( );
                }
                return;
            }

            const int win_w = window.right - window.left;
            const int win_h = window.bottom - window.top;

            if ( window.left != m_window.left || window.top != m_window.top || 
                 window.right != m_window.right || window.bottom != m_window.bottom ) {
                m_window = window;
                input::invalidate_virtual_desktop( );
            }

            const bool hr_active = ( game.cur_mod_state & 16 ) != 0;
            const bool ez_active = ( game.cur_mod_state & 2 ) != 0;

            float effective_cs = map.cs;
            if ( hr_active && !map.hr )
                effective_cs = std::min( effective_cs * 1.3f, 10.f );
            else if ( ez_active && !map.ez )
                effective_cs = effective_cs * 0.5f;

            auto snap = std::make_shared<aim_snapshot_t>( );
            snap->window = window;
            snap->speed_mult = game.speed_mult;
            snap->hr = hr_active;
            snap->ez = ez_active;
            snap->cs = effective_cs;
            snap->targets.reserve( 16 );

            for ( size_t index = 0; index < map.objects.size( ); ++index ) {
                const auto& obj = map.objects[ index ];
                if ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::spinner ) )
                    continue;
                
                if ( game.cur_time >= obj.start_time )
                    continue;

                assist::waypoint_t t{};
                t.x = obj.x;
                t.y = obj.y;

                if ( hr_active && !map.hr )
                    t.y = 384.f - obj.y;

                t.start_time = obj.start_time;
                t.end_time = obj.end_time;
                t.game_time = game.cur_time;
                t.hit_radius = hit_radius_screen( effective_cs, win_w, win_h );
                t.alive = true;
                t.is_slider = ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) != 0;
                snap->targets.push_back( t );
            }

            const float osu_hit_radius = std::max( 4.f, 54.4f - 4.48f * effective_cs );
            const local_difficulty_t local_difficulty = compute_local_difficulty(
                snap->targets, osu_hit_radius, game.speed_mult );
            const adaptive_parameters_t adaptive = update_adaptive_parameters(
                local_difficulty.difficulty, local_difficulty.large_jump_demand );
            snap->adaptive_difficulty = adaptive.difficulty;
            snap->effective_strength = adaptive.strength;
            snap->effective_smoothing_ms = adaptive.smoothing_ms;
            snap->effective_max_correction = adaptive.max_correction;
            snap->effective_anticipation_ms = adaptive.anticipation_ms;
            snap->adaptive_boost = adaptive.boost;
            snap->large_jump_demand = adaptive.large_jump_demand;

            {
                std::lock_guard<std::mutex> slock( m_snap_mutex );
                m_shared_snap = snap;
            }
        }

        std::optional<POINT> apply_hook_move( POINT raw, const MSLLHOOKSTRUCT& ) {
            if ( !enabled ) {
                return std::nullopt;
            }
            if ( m_user_blocked.load( ) ) {
                return std::nullopt;
            }
            if ( !m_in_play.load( ) ) {
                return std::nullopt;
            }

            float cursor_x = static_cast<float>( raw.x );
            float cursor_y = static_cast<float>( raw.y );

            if ( !tablet_mode ) {
                cursor_x -= m_prev_injected_x;
                cursor_y -= m_prev_injected_y;
            }

            POINT assisted{};
            if ( try_apply_absolute( cursor_x, cursor_y, &assisted ) ) {
                return assisted;
            }

            return std::nullopt;
        }

        void begin_play_verification( ) {
            std::lock_guard<std::mutex> lock( m_apply_mutex );
            m_verification = {};
            m_verification.effective_strength = assist_strength;
            m_verification.effective_smoothing_ms = smoothing_ms;
            m_verification.effective_max_correction = max_correction;
            m_verification.effective_anticipation_ms = 520.f;
            m_adaptive_difficulty = 0.f;
            m_adaptive_large_jump_demand = 0.f;
            m_adaptive_last_update = {};
            m_adaptive_hard_active = false;
        }

        [[nodiscard]] aim_verification_t verification( ) const {
            std::lock_guard<std::mutex> lock( m_apply_mutex );
            return m_verification;
        }

    private:
        std::atomic<bool>                  m_in_play{ false };
        std::atomic<bool>                  m_user_blocked{ false };
        RECT                               m_window{};
        std::shared_ptr<aim_snapshot_t>     m_shared_snap = std::make_shared<aim_snapshot_t>( );
        std::mutex                         m_snap_mutex;
        assist::trace                 m_state{};
        mutable std::mutex                 m_apply_mutex;
        float m_prev_injected_x = 0.f;
        float m_prev_injected_y = 0.f;
        aim_verification_t m_verification{};
        float m_adaptive_difficulty = 0.f;
        float m_adaptive_large_jump_demand = 0.f;
        std::chrono::steady_clock::time_point m_adaptive_last_update{};
        bool m_adaptive_hard_active = false;

        struct adaptive_parameters_t {
            float difficulty = 0.f;
            float strength = 62.f;
            float smoothing_ms = 78.f;
            float max_correction = 0.78f;
            float anticipation_ms = 520.f;
            float boost = 0.f;
            float large_jump_demand = 0.f;
        };

        struct local_difficulty_t {
            float difficulty = 0.f;
            float large_jump_demand = 0.f;
        };

        static local_difficulty_t compute_local_difficulty(
            const std::vector<assist::waypoint_t>& targets,
            float hit_radius, float speed_mult ) {

            constexpr size_t k_lookahead_objects = 12;
            const size_t count = std::min( targets.size( ), k_lookahead_objects );
            if ( count < 2 )
                return {};

            float weighted_sum = 0.f;
            float weight_sum = 0.f;
            float peak = 0.f;
            float rescue_weighted_sum = 0.f;
            float rescue_weight_sum = 0.f;
            float rescue_peak = 0.f;
            int current_hard_run = 0;
            int longest_hard_run = 0;
            assist::vec2 previous_jump{};
            bool has_previous_jump = false;

            for ( size_t i = 0; i + 1 < count; ++i ) {
                const auto& a = targets[ i ];
                const auto& b = targets[ i + 1 ];
                const assist::vec2 jump{ b.x - a.x, b.y - a.y };
                const float distance = assist::length( jump );
                const float radius_units = distance / std::max( hit_radius, 1.f );
                const float jump_score = assist::smoothstep( 1.75f, 8.5f, radius_units );

                const float real_interval_ms = static_cast<float>(
                    std::max( b.start_time - a.start_time, 1 ) ) /
                    std::max( speed_mult, 0.25f );
                const float required_velocity = distance * 1000.f /
                    std::max( real_interval_ms, 20.f );
                const float velocity_score = assist::smoothstep(
                    450.f, 1800.f, required_velocity );

                float direction_score = 0.f;
                if ( has_previous_jump && assist::length_sq( previous_jump ) > 1.f && distance > 1.f ) {
                    const float cosine = std::clamp(
                        assist::dot( previous_jump, jump ) /
                        ( assist::length( previous_jump ) * distance ), -1.f, 1.f );
                    const float reversal = ( 1.f - cosine ) * 0.5f;
                    direction_score = assist::smoothstep( 0.25f, 0.95f, reversal );
                }

                // Spatial gating prevents tiny dense streams from being treated like aim
                // difficulty merely because their timestamps imply high cursor velocity.
                const float spatial_gate = assist::smoothstep( 0.8f, 2.5f, radius_units );
                const float transition_score = assist::saturate(
                    ( 0.35f * jump_score + 0.50f * velocity_score +
                      0.15f * direction_score ) * spatial_gate );
                const float weight = std::exp( -static_cast<float>( i ) / 5.f );
                weighted_sum += transition_score * weight;
                weight_sum += weight;
                peak = std::max( peak, transition_score );

                // A separate near-future demand score distinguishes genuinely large,
                // fast or reversing jumps from generally dense difficult patterns.
                const float rescue_score = assist::saturate(
                    ( 0.45f * jump_score + 0.40f * velocity_score +
                      0.15f * direction_score ) * spatial_gate );
                const float rescue_weight = std::exp( -static_cast<float>( i ) / 3.f );
                rescue_weighted_sum += rescue_score * rescue_weight;
                rescue_weight_sum += rescue_weight;
                rescue_peak = std::max(
                    rescue_peak, rescue_score * std::sqrt( rescue_weight ) );

                if ( transition_score >= 0.55f ) {
                    current_hard_run++;
                    longest_hard_run = std::max( longest_hard_run, current_hard_run );
                }
                else {
                    current_hard_run = 0;
                }

                previous_jump = jump;
                has_previous_jump = true;
            }

            const float weighted_mean = weight_sum > 0.f ? weighted_sum / weight_sum : 0.f;
            const float consecutive_score = assist::saturate(
                static_cast<float>( std::max( longest_hard_run - 1, 0 ) ) / 4.f );
            local_difficulty_t result{};
            result.difficulty = assist::saturate(
                0.55f * weighted_mean + 0.25f * peak + 0.20f * consecutive_score );
            const float rescue_mean = rescue_weight_sum > 0.f
                ? rescue_weighted_sum / rescue_weight_sum : 0.f;
            result.large_jump_demand = assist::saturate(
                0.55f * rescue_mean + 0.45f * rescue_peak );
            return result;
        }

        adaptive_parameters_t update_adaptive_parameters(
            float raw_difficulty, float raw_large_jump_demand ) {
            std::lock_guard<std::mutex> lock( m_apply_mutex );
            const auto now = std::chrono::steady_clock::now( );
            float dt = 1.f / 60.f;
            if ( m_adaptive_last_update.time_since_epoch( ).count( ) != 0 )
                dt = std::chrono::duration<float>( now - m_adaptive_last_update ).count( );
            m_adaptive_last_update = now;
            dt = std::clamp( dt, 0.001f, 0.100f );

            float target = assist::saturate( raw_difficulty );
            if ( std::abs( target - m_adaptive_difficulty ) < 0.025f )
                target = m_adaptive_difficulty;
            const float hard_attack = assist::smoothstep( 0.60f, 0.95f, target );
            const float attack_response = 0.22f + ( 0.12f - 0.22f ) * hard_attack;
            const float response = target > m_adaptive_difficulty ? attack_response : 0.95f;
            const float alpha = 1.f - std::exp( -dt / response );
            m_adaptive_difficulty += ( target - m_adaptive_difficulty ) * alpha;
            m_adaptive_difficulty = assist::saturate( m_adaptive_difficulty );

            const float large_jump_target = assist::saturate( raw_large_jump_demand );
            const float large_jump_response = large_jump_target > m_adaptive_large_jump_demand
                ? 0.10f : 0.32f;
            const float large_jump_alpha = 1.f - std::exp( -dt / large_jump_response );
            m_adaptive_large_jump_demand +=
                ( large_jump_target - m_adaptive_large_jump_demand ) * large_jump_alpha;
            m_adaptive_large_jump_demand = assist::saturate(
                m_adaptive_large_jump_demand );

            const float influence = adaptive_aim
                ? std::clamp( adaptation_strength, 0.f, 100.f ) * 0.01f : 0.f;
            const float boost_curve = assist::smoothstep(
                0.20f, 1.00f, m_adaptive_difficulty );
            const float applied_boost = influence * boost_curve;

            adaptive_parameters_t result{};
            result.difficulty = m_adaptive_difficulty;
            result.boost = applied_boost;
            result.large_jump_demand = m_adaptive_large_jump_demand;
            result.strength = std::clamp(
                assist_strength + ( 100.f - assist_strength ) * 0.85f * applied_boost,
                5.f, 100.f );
            result.smoothing_ms = std::clamp(
                smoothing_ms * ( 1.f - 0.30f * applied_boost ),
                35.f, 180.f );
            result.max_correction = std::clamp(
                max_correction * ( 1.f + 0.14f * applied_boost ),
                0.10f, 1.75f );
            result.anticipation_ms = std::clamp(
                520.f + 90.f * applied_boost,
                520.f, 610.f );

            m_verification.adaptive_difficulty = result.difficulty;
            m_verification.effective_strength = result.strength;
            m_verification.effective_smoothing_ms = result.smoothing_ms;
            m_verification.effective_max_correction = result.max_correction;
            m_verification.effective_anticipation_ms = result.anticipation_ms;

            const bool first_sample = m_verification.adaptive_sample_seconds <= 0.0;
            m_verification.adaptive_sample_seconds += dt;
            m_verification.adaptive_difficulty_time_sum += result.difficulty * dt;
            m_verification.adaptive_difficulty_peak = std::max(
                m_verification.adaptive_difficulty_peak, result.difficulty );
            const size_t difficulty_bucket = result.difficulty < 0.30f ? 0u :
                result.difficulty < 0.60f ? 1u : result.difficulty < 0.80f ? 2u : 3u;
            m_verification.difficulty_bucket_seconds[ difficulty_bucket ] += dt;
            m_verification.effective_strength_time_sum += result.strength * dt;
            m_verification.effective_smoothing_time_sum += result.smoothing_ms * dt;
            m_verification.effective_max_correction_time_sum += result.max_correction * dt;
            if ( first_sample ) {
                m_verification.effective_strength_min = result.strength;
                m_verification.effective_strength_max = result.strength;
                m_verification.effective_smoothing_min = result.smoothing_ms;
                m_verification.effective_smoothing_max = result.smoothing_ms;
                m_verification.effective_max_correction_min = result.max_correction;
                m_verification.effective_max_correction_max = result.max_correction;
                m_verification.effective_anticipation_min = result.anticipation_ms;
                m_verification.effective_anticipation_max = result.anticipation_ms;
            }
            else {
                m_verification.effective_strength_min = std::min(
                    m_verification.effective_strength_min, result.strength );
                m_verification.effective_strength_max = std::max(
                    m_verification.effective_strength_max, result.strength );
                m_verification.effective_smoothing_min = std::min(
                    m_verification.effective_smoothing_min, result.smoothing_ms );
                m_verification.effective_smoothing_max = std::max(
                    m_verification.effective_smoothing_max, result.smoothing_ms );
                m_verification.effective_max_correction_min = std::min(
                    m_verification.effective_max_correction_min, result.max_correction );
                m_verification.effective_max_correction_max = std::max(
                    m_verification.effective_max_correction_max, result.max_correction );
                m_verification.effective_anticipation_min = std::min(
                    m_verification.effective_anticipation_min, result.anticipation_ms );
                m_verification.effective_anticipation_max = std::max(
                    m_verification.effective_anticipation_max, result.anticipation_ms );
            }

            if ( result.difficulty >= 0.75f && !m_adaptive_hard_active ) {
                m_verification.adaptive_hard_pattern_activations++;
                m_adaptive_hard_active = true;
            }
            else if ( result.difficulty < 0.60f ) {
                m_adaptive_hard_active = false;
            }
            return result;
        }

        void clear_motion_state( ) {
            std::lock_guard<std::mutex> lock( m_apply_mutex );
            m_window = {};
            m_state = {};
            m_prev_injected_x = 0.f;
            m_prev_injected_y = 0.f;
        }

        assist::config build_config( const aim_snapshot_t* snap ) const {
            assist::config cfg{};
            cfg.enabled        = enabled;
            cfg.tablet_mode    = tablet_mode;
            cfg.ignore_sliders = ignore_sliders;
            cfg.strength       = snap ? snap->effective_strength : assist_strength;
            cfg.assist_radius  = assist_radius;
            cfg.smoothing_ms   = snap ? snap->effective_smoothing_ms : smoothing_ms;
            cfg.max_correction = snap ? snap->effective_max_correction : max_correction;
            cfg.speed_mult     = snap ? snap->speed_mult : 1.f;
            cfg.anticipation_ms = snap ? snap->effective_anticipation_ms : 520.f;
            cfg.adaptive_difficulty = snap ? snap->adaptive_difficulty : 0.f;
            cfg.adaptive_boost = snap ? snap->adaptive_boost : 0.f;
            cfg.large_jump_demand = snap ? snap->large_jump_demand : 0.f;

            return cfg;
        }

        bool try_apply_absolute( float cursor_x, float cursor_y, POINT* out_pos ) {
            std::lock_guard<std::mutex> lock( m_apply_mutex );
            m_verification.aim_reports++;

            std::shared_ptr<aim_snapshot_t> snap;
            {
                std::lock_guard<std::mutex> slock( m_snap_mutex );
                snap = m_shared_snap;
            }

            const bool has_targets = snap && !snap->targets.empty( );
            const bool has_motion =
                assist::length_sq( m_state.assist_offset ) > 0.0025f ||
                assist::length_sq( m_state.assist_velocity ) > 0.25f ||
                assist::length_sq( m_state.assist_acceleration ) > 1.f;

            if ( !has_targets && !has_motion ) {
                m_prev_injected_x = 0.f;
                m_prev_injected_y = 0.f;
                m_state.raw_pos = { cursor_x, cursor_y };
                return false;
            }

            const RECT frame = snap ? snap->window : m_window;
            const assist::waypoint_t* targets = has_targets ? snap->targets.data( ) : nullptr;
            const size_t target_count = has_targets ? snap->targets.size( ) : 0;
            auto cfg = build_config( snap.get( ) );
            float out_x = 0.f;
            float out_y = 0.f;

            if ( !assist::adjust( cursor_x, cursor_y, targets, target_count,
                    frame, cfg, m_state, &out_x, &out_y ) )
                return false;

            out_pos->x = static_cast<int>( std::lround( out_x ) );
            out_pos->y = static_cast<int>( std::lround( out_y ) );

            if ( m_state.has_target )
                m_verification.target_bearing_reports++;

            if ( m_state.high_difficulty_rescue_activation )
                m_verification.high_difficulty_rescue_activations++;
            if ( m_state.large_jump_rescue_activation ) {
                m_verification.large_jump_rescue_activations++;
                m_verification.first_rescue_miss_samples++;
                m_verification.first_rescue_miss_sum +=
                    m_state.closest_predicted_miss;
            }
            if ( m_state.final_correction_sample ) {
                m_verification.final_correction_miss_samples++;
                m_verification.final_correction_miss_sum +=
                    m_state.final_correction_miss;
            }

            if ( m_state.relevant_approach ) {
                m_verification.relevant_approach_reports++;
                m_verification.relevant_predicted_miss_samples++;
                m_verification.relevant_predicted_miss_sum +=
                    m_state.closest_predicted_miss;
            }

            if ( m_state.relevant_approach && m_state.requested_correction )
                m_verification.engaged_reports++;

            if ( m_state.max_correction_clamped )
                m_verification.max_correction_clamp_hits++;

            switch ( m_state.last_gate_result ) {
            case assist::gate_result::rejected_safe_trajectory:
                m_verification.rejected_safe_trajectory++;
                break;
            case assist::gate_result::rejected_distance:
                m_verification.rejected_distance++;
                break;
            case assist::gate_result::rejected_direction:
                m_verification.rejected_direction++;
                break;
            case assist::gate_result::rejected_timing:
                m_verification.rejected_timing++;
                break;
            case assist::gate_result::corrected:
                m_verification.corrected_reports++;
                m_verification.corrected_predicted_miss_samples++;
                m_verification.corrected_predicted_miss_sum +=
                    m_state.closest_predicted_miss;
                m_verification.peak_corrected_predicted_miss = std::max(
                    m_verification.peak_corrected_predicted_miss,
                    m_state.closest_predicted_miss );
                {
                    const size_t difficulty_bucket = cfg.adaptive_difficulty < 0.30f ? 0u :
                        cfg.adaptive_difficulty < 0.60f ? 1u :
                        cfg.adaptive_difficulty < 0.80f ? 2u : 3u;
                    m_verification.corrected_by_difficulty[ difficulty_bucket ]++;
                }
                break;
            default:
                break;
            }
            if ( m_state.safe_destination_multiplier > 0.f ) {
                m_verification.safe_destination_multiplier =
                    m_state.safe_destination_multiplier;
            }

            const float correction = assist::length( m_state.assist_offset );
            if ( correction > 0.05f )
                m_verification.non_zero_corrections++;

            if ( m_state.requested_correction ) {
                m_verification.requested_non_zero_samples++;
                m_verification.requested_correction_sum +=
                    m_state.requested_correction_magnitude;
                m_verification.peak_requested_correction = std::max(
                    m_verification.peak_requested_correction,
                    m_state.requested_correction_magnitude );
            }

            if ( tablet_mode ) {
                const float sent_delta_x = static_cast<float>( out_pos->x ) - cursor_x;
                const float sent_delta_y = static_cast<float>( out_pos->y ) - cursor_y;
                const float observed_delta = std::sqrt(
                    sent_delta_x * sent_delta_x + sent_delta_y * sent_delta_y );
                if ( m_state.requested_correction ) {
                    m_verification.corrected_observed_samples++;
                    m_verification.corrected_observed_sum += observed_delta;
                    if ( cfg.adaptive_difficulty < 0.60f ) {
                        m_verification.low_medium_correction_samples++;
                        m_verification.low_medium_correction_sum += observed_delta;
                    }
                    else {
                        m_verification.high_extreme_correction_samples++;
                        m_verification.high_extreme_correction_sum += observed_delta;
                    }
                }
                if ( observed_delta > 0.05f ) {
                    m_verification.observed_non_zero_samples++;
                    m_verification.observed_output_delta_sum += observed_delta;
                    m_verification.peak_observed_output_delta = std::max(
                        m_verification.peak_observed_output_delta, observed_delta );
                }
            }

            input::move_absolute_virtual_desktop( out_pos->x, out_pos->y );

            m_prev_injected_x = out_x - cursor_x;
            m_prev_injected_y = out_y - cursor_y;
            return true;
        }

        static float hit_radius_screen( float cs, int win_w, int win_h ) {
            const float playfield_height = static_cast<float>( win_h ) * 0.8f;
            const float osu_scale = ( playfield_height * ( 4.f / 3.f ) ) / 512.f;
            const float osu_radius = 54.4f - 4.48f * cs;
            return std::max( 8.f, osu_radius * osu_scale );
        }
    };

}
