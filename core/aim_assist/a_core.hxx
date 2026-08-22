#pragma once

#include <impl/util/playfield.hxx>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace assist {

    struct waypoint_t {
        float   x = 0.f;
        float   y = 0.f;
        float   hit_radius = 0.f;
        int32_t start_time = 0;
        int32_t end_time = 0;
        int32_t game_time = 0;
        bool    alive = false;
        bool    is_slider = false;
        float   slider_geometry_demand = 0.f;
        float   slider_excursion_radii = 0.f;
        float   slider_duration_ms = 0.f;
        int32_t slider_repeat = 1;
        bool    slider_compact = false;
        bool    slider_compact_repeat = false;
    };

    struct config {
        bool  enabled = true;
        bool  tablet_mode = true;
        bool  ignore_sliders = false;
        float strength = 62.f;          // percent
        float assist_radius = 2.55f;    // multiples of the hit radius
        float smoothing_ms = 78.f;
        float max_correction = 0.78f;   // multiples of the hit radius
        float speed_mult = 1.f;
        float anticipation_ms = 520.f;
        float adaptive_difficulty = 0.f;
        float adaptive_boost = 0.f;
        float large_jump_demand = 0.f;
    };

    struct vec2 {
        float x = 0.f;
        float y = 0.f;
    };

    enum class gate_result : uint8_t {
        none,
        rejected_safe_trajectory,
        rejected_distance,
        rejected_direction,
        rejected_timing,
        rejected_slider_low_demand,
        corrected
    };

    struct trace {
        // Raw input and synthetic correction are intentionally separate. In Tablet Mode,
        // raw_pos is always the current OS-mapped pen report, never the assisted output.
        vec2 raw_pos{};
        vec2 raw_velocity{};
        vec2 assist_offset{};
        vec2 assist_velocity{};
        vec2 assist_acceleration{};

        float last_max_displacement = 1.f;

        int32_t bind_id = 0;
        float bind_x = 0.f;
        float bind_y = 0.f;
        bool requested_correction = false;
        bool relevant_approach = false;
        bool has_target = false;
        bool max_correction_clamped = false;
        float requested_correction_magnitude = 0.f;
        float rescue_demand = 0.f;
        float prediction_confidence = 0.f;
        uint32_t raw_history_reports = 0;
        int32_t high_rescue_bind_id = 0;
        int32_t large_rescue_bind_id = 0;
        bool high_difficulty_rescue_activation = false;
        bool large_jump_rescue_activation = false;
        bool has_last_corrected_miss = false;
        float last_corrected_miss = 0.f;
        bool final_correction_sample = false;
        float final_correction_miss = 0.f;
        float safe_destination_multiplier = 0.f;
        float closest_predicted_miss = 0.f;
        bool target_is_slider = false;
        bool slider_target_new = false;
        bool slider_assist_activation = false;
        bool slider_rejected_safe = false;
        bool slider_rejected_low_demand = false;
        bool slider_compact_suppressed = false;
        float slider_assist_demand = 0.f;
        float slider_geometry_demand = 0.f;
        float slider_entry_difficulty = 0.f;
        float slider_demand_state = 0.f;
        int32_t slider_demand_bind_id = 0;
        int32_t slider_activation_bind_id = 0;
        gate_result last_gate_result = gate_result::none;
        float target_switch_damping_remaining = 0.f;
        vec2 target_switch_direction{};
        bool has_raw = false;
        std::chrono::steady_clock::time_point last_update{};
    };

    inline float saturate( float v ) {
        return std::clamp( v, 0.f, 1.f );
    }

    inline float smoothstep( float edge0, float edge1, float x ) {
        if ( edge1 <= edge0 )
            return x >= edge1 ? 1.f : 0.f;
        const float t = saturate( ( x - edge0 ) / ( edge1 - edge0 ) );
        return t * t * ( 3.f - 2.f * t );
    }

    struct slider_demand_result_t {
        float demand = 1.f;
        float entry_difficulty = 0.f;
        float predicted_miss_demand = 0.f;
        bool already_safe = false;
        bool low_demand = false;
        bool compact_suppressed = false;
    };

    // Pure, normalized decision helper used by the live controller and offline tests.
    // Geometry can permit assistance, but only predicted miss evidence creates demand.
    inline slider_demand_result_t compute_slider_assist_demand(
        const waypoint_t& target,
        float current_distance,
        float predicted_distance,
        float zero_inner_radius,
        float accepted_safe_radius,
        float hit_radius,
        float eta_ms,
        float speed_mult = 1.f ) {

        slider_demand_result_t result{};
        if ( !target.is_slider )
            return result;

        result.demand = 0.f;
        const float radius = std::max( hit_radius, 1.f );
        if ( predicted_distance <= zero_inner_radius ) {
            result.already_safe = true;
            result.low_demand = true;
            return result;
        }

        const float distance_radii = current_distance / radius;
        const float real_eta_ms = eta_ms / std::max( speed_mult, 0.25f );
        const float seconds_remaining = std::max( real_eta_ms, 25.f ) * 0.001f;
        const float remaining_radii = std::max(
            current_distance - accepted_safe_radius, 0.f ) / radius;
        const float required_radii_per_second = remaining_radii / seconds_remaining;
        const float distance_difficulty = smoothstep( 1.35f, 4.20f, distance_radii );
        const float speed_difficulty = smoothstep(
            4.50f, 13.0f, required_radii_per_second );
        result.entry_difficulty = saturate(
            0.42f * distance_difficulty + 0.58f * speed_difficulty );

        const float full_miss_radius = std::max(
            accepted_safe_radius, radius * 1.18f );
        result.predicted_miss_demand = smoothstep(
            zero_inner_radius, full_miss_radius, predicted_distance );
        const float strong_miss = smoothstep(
            radius * 1.00f, radius * 1.65f, predicted_distance );
        const float geometry = saturate( target.slider_geometry_demand );
        float need = geometry + ( 1.f - geometry ) * result.entry_difficulty;

        // A clear miss can still rescue an otherwise easy slider; geometry suppression
        // must not turn compact sliders into an unconditional ignore rule.
        need = std::max( need, 0.85f * strong_miss );
        float compact_scale = 1.f;
        if ( target.slider_compact ) {
            compact_scale = 0.35f + 0.65f * std::max(
                result.entry_difficulty, strong_miss );
            result.compact_suppressed = compact_scale < 0.90f;
        }

        result.demand = saturate(
            result.predicted_miss_demand * need * compact_scale );
        result.low_demand = result.demand < 0.06f;
        return result;
    }

    inline vec2 operator+( vec2 a, vec2 b ) { return { a.x + b.x, a.y + b.y }; }
    inline vec2 operator-( vec2 a, vec2 b ) { return { a.x - b.x, a.y - b.y }; }
    inline vec2 operator*( vec2 v, float s ) { return { v.x * s, v.y * s }; }
    inline vec2& operator+=( vec2& a, vec2 b ) { a.x += b.x; a.y += b.y; return a; }

    inline float dot( vec2 a, vec2 b ) { return a.x * b.x + a.y * b.y; }
    inline float length_sq( vec2 v ) { return dot( v, v ); }
    inline float length( vec2 v ) { return std::sqrt( length_sq( v ) ); }

    inline vec2 limit_length( vec2 v, float maximum ) {
        const float magnitude = length( v );
        if ( magnitude <= maximum || magnitude <= 0.0001f )
            return v;
        return v * ( maximum / magnitude );
    }

    inline const waypoint_t* select_target(
        const waypoint_t* markers,
        size_t count,
        int32_t tick,
        const config& params,
        const trace& mem ) {

        if ( !markers || count == 0 )
            return nullptr;

        // Timing-aware hysteresis: retain the selected object while it remains in the
        // future snapshot. This prevents close patterns from swapping targets by geometry.
        if ( mem.bind_id != 0 ) {
            for ( size_t i = 0; i < count; ++i ) {
                const auto& marker = markers[ i ];
                if ( marker.start_time == mem.bind_id && marker.start_time > tick &&
                     ( !params.ignore_sliders || !marker.is_slider ) ) {
                    return &marker;
                }
            }
        }

        const waypoint_t* earliest = nullptr;
        for ( size_t i = 0; i < count; ++i ) {
            const auto& marker = markers[ i ];
            if ( marker.start_time <= tick || ( params.ignore_sliders && marker.is_slider ) )
                continue;
            if ( !earliest || marker.start_time < earliest->start_time )
                earliest = &marker;
        }
        return earliest;
    }

    inline bool adjust(
        float sx, float sy,
        const waypoint_t* markers, size_t n_markers,
        const RECT& frame,
        const config& params,
        trace& mem,
        float* ox, float* oy ) {

        if ( !ox || !oy )
            return false;

        if ( !params.enabled ) {
            mem = {};
            *ox = sx;
            *oy = sy;
            return true;
        }

        const auto now = std::chrono::steady_clock::now( );
        float dt = 1.f / 240.f;
        if ( mem.last_update.time_since_epoch( ).count( ) != 0 )
            dt = std::chrono::duration<float>( now - mem.last_update ).count( );
        mem.last_update = now;

        // Very small duplicate reports and long stalls must not destabilise velocity or
        // allow one oversized correction step.
        dt = std::clamp( dt, 0.0005f, 0.025f );

        const vec2 raw{ sx, sy };
        mem.requested_correction = false;
        mem.relevant_approach = false;
        mem.has_target = false;
        mem.max_correction_clamped = false;
        mem.requested_correction_magnitude = 0.f;
        mem.rescue_demand = 0.f;
        mem.high_difficulty_rescue_activation = false;
        mem.large_jump_rescue_activation = false;
        mem.final_correction_sample = false;
        mem.target_is_slider = false;
        mem.slider_target_new = false;
        mem.slider_assist_activation = false;
        mem.slider_rejected_safe = false;
        mem.slider_rejected_low_demand = false;
        mem.slider_compact_suppressed = false;
        mem.slider_assist_demand = 0.f;
        mem.slider_geometry_demand = 0.f;
        mem.slider_entry_difficulty = 0.f;
        mem.last_gate_result = gate_result::none;
        if ( !mem.has_raw ) {
            mem.raw_pos = raw;
            mem.has_raw = true;
        }

        const vec2 previous_filtered_velocity = mem.raw_velocity;
        vec2 measured_velocity = ( raw - mem.raw_pos ) * ( 1.f / dt );
        measured_velocity = limit_length( measured_velocity, 12000.f );

        // A short exponential filter removes report quantisation while preserving tablet
        // flick direction. It uses only successive unmodified raw positions.
        const float velocity_alpha = 1.f - std::exp( -dt / 0.018f );
        mem.raw_velocity += ( measured_velocity - mem.raw_velocity ) * velocity_alpha;
        mem.raw_pos = raw;
        if ( mem.raw_history_reports < UINT32_MAX )
            mem.raw_history_reports++;

        const float previous_speed = length( previous_filtered_velocity );
        const float filtered_speed = length( mem.raw_velocity );
        float direction_consistency = 1.f;
        if ( previous_speed > 80.f && filtered_speed > 80.f ) {
            const float cosine = std::clamp(
                dot( previous_filtered_velocity, mem.raw_velocity ) /
                ( previous_speed * filtered_speed ), -1.f, 1.f );
            direction_consistency = 0.5f + 0.5f * cosine;
        }
        const float speed_consistency = 1.f - saturate(
            std::abs( filtered_speed - previous_speed ) /
            std::max( std::max( filtered_speed, previous_speed ), 250.f ) );
        const float history_confidence = smoothstep(
            2.f, 6.f, static_cast<float>( mem.raw_history_reports ) );
        mem.prediction_confidence = history_confidence *
            ( 0.55f + 0.30f * direction_consistency + 0.15f * speed_consistency );

        int32_t tick = 0;
        if ( markers && n_markers > 0 )
            tick = markers[ 0 ].game_time;

        const waypoint_t* target = select_target( markers, n_markers, tick, params, mem );
        vec2 desired_offset{};
        float max_displacement = mem.last_max_displacement;

        if ( target ) {
            mem.has_target = true;
            const bool target_new = mem.bind_id != target->start_time;
            const bool target_changed = mem.bind_id != 0 && mem.bind_id != target->start_time;
            mem.target_is_slider = target->is_slider;
            mem.slider_target_new = target->is_slider && target_new;
            if ( target_changed && mem.has_last_corrected_miss ) {
                mem.final_correction_sample = true;
                mem.final_correction_miss = mem.last_corrected_miss;
                mem.has_last_corrected_miss = false;
            }
            const auto projected = playfield::playfield_to_screen(
                target->x, target->y, frame );
            const vec2 target_screen{
                static_cast<float>( projected.x ),
                static_cast<float>( projected.y )
            };

            // Changing targets updates the destination but deliberately preserves the
            // correction offset, velocity and acceleration for a continuous transition.
            mem.bind_id = target->start_time;
            mem.bind_x = target_screen.x;
            mem.bind_y = target_screen.y;

            const float hit_radius = std::max( target->hit_radius, 4.f );
            const float outer_radius = hit_radius * std::clamp( params.assist_radius, 1.1f, 4.f );
            const float strength = saturate( params.strength * 0.01f );
            // Strength is controller authority, not only a final magnitude multiplier.
            // Low authority accepts a generous raw hit and rescues only clear misses;
            // high authority asks for a more reliable interior path without centre-locking.
            const float authority = smoothstep( 0.10f, 0.95f, strength );
            // Extra follow-through is deliberately concentrated in the upper half of the
            // slider so low/medium settings retain their established subtle character.
            const float upper_authority = smoothstep( 0.45f, 0.95f, strength );
            const float hard_jump_rescue = saturate( params.adaptive_boost ) *
                smoothstep( 0.60f, 0.85f, params.adaptive_difficulty ) *
                smoothstep( 0.45f, 0.85f, params.large_jump_demand );
            const float accepted_safe_multiplier =
                0.86f + ( 0.58f - 0.86f ) * authority;
            const float safe_destination_multiplier =
                0.74f + ( 0.46f - 0.74f ) * authority;
            const float accepted_safe_radius = hit_radius * accepted_safe_multiplier;
            const float safe_destination_radius = hit_radius * safe_destination_multiplier;
            mem.safe_destination_multiplier = safe_destination_multiplier;
            max_displacement = hit_radius * std::clamp( params.max_correction, 0.1f, 1.75f );
            mem.last_max_displacement = max_displacement;

            const vec2 to_target = target_screen - raw;
            const float current_distance = length( to_target );
            const vec2 target_direction = current_distance > 0.001f
                ? to_target * ( 1.f / current_distance )
                : vec2{};

            const int32_t eta_ms = target->start_time - tick;
            constexpr float prediction_horizon_max = 0.090f;
            const float base_prediction_horizon = std::clamp(
                static_cast<float>( eta_ms ) * 0.001f, 0.012f, prediction_horizon_max );
            // A short or inconsistent input history shortens only the speculative part of
            // prediction. Stable tablet motion still receives the full 90 ms look-ahead.
            const float prediction_horizon = base_prediction_horizon *
                ( 0.65f + 0.35f * mem.prediction_confidence );
            // Use the closest point on the short raw-velocity segment rather than only its
            // endpoint. A natural trajectory that passes through the hit region therefore
            // produces little or no correction even if it would be past the centre at the
            // end of the prediction horizon.
            float closest_time = 0.f;
            const float raw_speed_sq = length_sq( mem.raw_velocity );
            if ( raw_speed_sq > 1.f )
                closest_time = std::clamp( dot( to_target, mem.raw_velocity ) / raw_speed_sq,
                    0.f, prediction_horizon );
            const vec2 predicted_raw = raw + mem.raw_velocity * closest_time;
            const vec2 predicted_to_target = target_screen - predicted_raw;
            const float predicted_distance = length( predicted_to_target );
            mem.closest_predicted_miss = predicted_distance;

            // Distance envelope: still zero outside the configured radius, but begins
            // contributing sooner on a legitimate approach instead of concentrating most
            // of its authority in the last few pixels before the target.
            const float distance_progress = saturate(
                ( outer_radius - current_distance ) / std::max( outer_radius - accepted_safe_radius, 1.f ) );
            // Remain deliberately weak at the outer edge, then reach useful authority
            // through the middle of the radius and full authority before the safe region.
            // Higher authority reaches useful distance gain earlier once inside the
            // configured radius. The gain remains exactly zero at and beyond its edge.
            const float distance_full_progress =
                0.72f + ( 0.38f - 0.72f ) * authority -
                0.10f * upper_authority - 0.06f * saturate( params.adaptive_boost ) -
                0.05f * hard_jump_rescue;
            const float outer_gate = current_distance < outer_radius
                ? smoothstep( 0.f, std::max( distance_full_progress, 0.20f ), distance_progress ) : 0.f;

            // Preserve a true zero-assistance interior, then add a graduated cross-track
            // stabilisation band inside the accepted-safe radius at high authority. This
            // still points only toward the safe destination radius, never exact centre.
            const float full_miss_multiplier =
                1.18f + ( 0.92f - 1.18f ) * authority -
                0.06f * hard_jump_rescue;
            const float adaptive_interior_authority = saturate(
                upper_authority + 0.18f * saturate( params.adaptive_boost ) );
            const float zero_inner_multiplier = accepted_safe_multiplier +
                ( safe_destination_multiplier - accepted_safe_multiplier ) *
                adaptive_interior_authority;
            const float zero_inner_radius = hit_radius * zero_inner_multiplier;
            const float interior_gate_ceiling = std::min(
                0.40f, 0.32f * upper_authority + 0.08f * saturate( params.adaptive_boost ) );
            float inner_gate = 0.f;
            if ( predicted_distance <= accepted_safe_radius ) {
                inner_gate = interior_gate_ceiling * smoothstep(
                    zero_inner_radius, accepted_safe_radius, predicted_distance );
            }
            else {
                inner_gate = interior_gate_ceiling + ( 1.f - interior_gate_ceiling ) *
                    smoothstep( accepted_safe_radius,
                        hit_radius * full_miss_multiplier, predicted_distance );
            }

            // Begin weak trajectory steering well before the hit, so the damped offset has
            // time to develop on tablet jumps.  The gain remains zero for distant future
            // objects and naturally reaches full strength in the final approach.
            const float eta = static_cast<float>( std::max( eta_ms, 0 ) );
            // Strength adds only a modest authority window around Adaptive Aim's cached
            // anticipation value; it does not replace or dramatically extend it.
            const float strength_anticipation_offset =
                -30.f + ( 35.f - -30.f ) * authority;
            const float anticipation_ms = std::clamp(
                params.anticipation_ms + strength_anticipation_offset +
                20.f * hard_jump_rescue, 440.f, 640.f );
            const float early_timing_gain = 0.12f + ( 0.28f - 0.12f ) * authority;
            float timing_gate = 1.f;
            if ( eta > 70.f ) {
                if ( eta <= 360.f )
                    timing_gate = early_timing_gain + ( 1.f - early_timing_gain ) *
                        ( 1.f - smoothstep( 70.f, 360.f, eta ) );
                else
                    timing_gate = early_timing_gain *
                        ( 1.f - smoothstep( 360.f, anticipation_ms, eta ) );
            }

            float direction_gate = 1.f;
            const float raw_speed = length( mem.raw_velocity );
            if ( raw_speed > 80.f && current_distance > 0.001f ) {
                const float approach = dot( mem.raw_velocity, target_direction ) / raw_speed;
                // Strong, deliberate movement away fades assistance instead of reversing
                // the player's hand. Slight misses retain enough authority to be corrected.
                const float direction_zero = -0.72f + ( -0.95f - -0.72f ) * authority;
                const float direction_full = -0.05f + ( -0.45f - -0.05f ) * authority;
                direction_gate = smoothstep( direction_zero, direction_full, approach );
            }

            mem.relevant_approach = current_distance <= outer_radius
                && timing_gate > 0.05f && direction_gate > 0.05f;

            float slider_demand_gate = 1.f;
            if ( target->is_slider ) {
                const slider_demand_result_t slider_demand = compute_slider_assist_demand(
                    *target, current_distance, predicted_distance, zero_inner_radius,
                    accepted_safe_radius, hit_radius, eta, params.speed_mult );

                if ( mem.slider_demand_bind_id != target->start_time )
                    mem.slider_demand_bind_id = target->start_time;

                const float demand_response = slider_demand.demand > mem.slider_demand_state
                    ? 0.045f : 0.075f;
                const float demand_alpha = 1.f - std::exp(
                    -dt / demand_response );
                mem.slider_demand_state +=
                    ( slider_demand.demand - mem.slider_demand_state ) * demand_alpha;
                mem.slider_demand_state = saturate( mem.slider_demand_state );

                slider_demand_gate = mem.slider_demand_state;
                mem.slider_assist_demand = slider_demand_gate;
                mem.slider_geometry_demand = saturate(
                    target->slider_geometry_demand );
                mem.slider_entry_difficulty = slider_demand.entry_difficulty;
                mem.slider_rejected_safe = slider_demand.already_safe;
                mem.slider_rejected_low_demand = slider_demand.low_demand;
                mem.slider_compact_suppressed =
                    target->slider_compact && slider_demand.compact_suppressed;
            }
            else {
                mem.slider_demand_bind_id = 0;
                const float demand_alpha = 1.f - std::exp( -dt / 0.075f );
                mem.slider_demand_state +=
                    ( 0.f - mem.slider_demand_state ) * demand_alpha;
            }

            const float needed_distance = std::max(
                predicted_distance - safe_destination_radius, 0.f );
            mem.rescue_demand = saturate(
                needed_distance / std::max( max_displacement, hit_radius ) );
            vec2 minimum_path_translation{};
            if ( predicted_distance > 0.001f ) {
                minimum_path_translation = predicted_to_target *
                    ( needed_distance / predicted_distance );
            }

            const float speculative_miss = smoothstep(
                outer_radius, outer_radius * 2.5f, predicted_distance );
            const float speculative_reduction = 0.35f *
                ( 1.f - 0.35f * hard_jump_rescue );
            const float prediction_gate = 1.f - speculative_reduction * speculative_miss *
                ( 1.f - mem.prediction_confidence );
            const float envelope = outer_gate * inner_gate * timing_gate *
                direction_gate * prediction_gate * slider_demand_gate;
            const float correction_gain = strength * envelope;
            if ( length_sq( minimum_path_translation ) >
                 max_displacement * max_displacement &&
                 max_displacement * correction_gain > 0.05f ) {
                mem.max_correction_clamped = true;
            }
            desired_offset = limit_length( minimum_path_translation, max_displacement );
            desired_offset = desired_offset * correction_gain;
            desired_offset = limit_length( desired_offset, max_displacement );
            mem.requested_correction_magnitude = length( desired_offset );
            mem.requested_correction = mem.requested_correction_magnitude > 0.05f;
            if ( mem.requested_correction ) {
                if ( target->is_slider &&
                     mem.slider_activation_bind_id != target->start_time ) {
                    mem.slider_activation_bind_id = target->start_time;
                    mem.slider_assist_activation = true;
                }
                mem.has_last_corrected_miss = true;
                mem.last_corrected_miss = predicted_distance;
                if ( params.adaptive_boost > 0.01f &&
                     params.adaptive_difficulty >= 0.60f &&
                     mem.high_rescue_bind_id != target->start_time ) {
                    mem.high_rescue_bind_id = target->start_time;
                    mem.high_difficulty_rescue_activation = true;
                }
                if ( hard_jump_rescue > 0.05f &&
                     mem.large_rescue_bind_id != target->start_time ) {
                    mem.large_rescue_bind_id = target->start_time;
                    mem.large_jump_rescue_activation = true;
                }
            }

            // Record one mutually-exclusive result per target-bearing report. This keeps
            // rejection telemetry cheap and makes selectivity measurable after a play.
            if ( current_distance >= outer_radius || outer_gate <= 0.001f )
                mem.last_gate_result = gate_result::rejected_distance;
            else if ( timing_gate <= 0.001f )
                mem.last_gate_result = gate_result::rejected_timing;
            else if ( direction_gate <= 0.001f )
                mem.last_gate_result = gate_result::rejected_direction;
            else if ( predicted_distance <= zero_inner_radius || inner_gate <= 0.001f )
                mem.last_gate_result = gate_result::rejected_safe_trajectory;
            else if ( target->is_slider &&
                      ( mem.slider_rejected_low_demand || slider_demand_gate <= 0.01f ) )
                mem.last_gate_result = gate_result::rejected_slider_low_demand;
            else if ( mem.requested_correction )
                mem.last_gate_result = gate_result::corrected;
            else {
                const float weakest_gate = std::min(
                    std::min( outer_gate, timing_gate ), std::min( direction_gate, inner_gate ) );
                if ( weakest_gate == timing_gate )
                    mem.last_gate_result = gate_result::rejected_timing;
                else if ( weakest_gate == direction_gate )
                    mem.last_gate_result = gate_result::rejected_direction;
                else if ( weakest_gate == outer_gate )
                    mem.last_gate_result = gate_result::rejected_distance;
                else
                    mem.last_gate_result = gate_result::rejected_safe_trajectory;
            }

            if ( target_changed ) {
                vec2 new_direction = predicted_distance > 0.001f
                    ? predicted_to_target * ( 1.f / predicted_distance ) : target_direction;
                mem.target_switch_direction = new_direction;
                mem.target_switch_damping_remaining = 0.080f;
            }

        }
        else {
            if ( mem.has_last_corrected_miss ) {
                mem.final_correction_sample = true;
                mem.final_correction_miss = mem.last_corrected_miss;
                mem.has_last_corrected_miss = false;
            }
            mem.bind_id = 0;
            mem.slider_demand_bind_id = 0;
            const float demand_alpha = 1.f - std::exp( -dt / 0.075f );
            mem.slider_demand_state +=
                ( 0.f - mem.slider_demand_state ) * demand_alpha;
        }

        // Critically damped offset control. Limits are derived from the resolution-scaled
        // maximum displacement, so the four public settings behave consistently across
        // tablet areas and display sizes.
        const float smoothing = std::clamp( params.smoothing_ms, 35.f, 180.f ) * 0.001f;
        const float response_authority = smoothstep(
            0.45f, 0.95f, saturate( params.strength * 0.01f ) );
        const float hard_jump_response = saturate( params.adaptive_boost ) *
            smoothstep( 0.60f, 0.85f, params.adaptive_difficulty ) *
            smoothstep( 0.45f, 0.85f, params.large_jump_demand );
        const float adaptive_rescue_response = saturate(
            saturate( params.adaptive_boost ) *
            smoothstep( 0.60f, 0.85f, params.adaptive_difficulty ) *
            smoothstep( 0.15f, 0.65f, mem.rescue_demand ) *
            ( 1.f + 0.18f * hard_jump_response ) );
        const float response_scale = 1.f + 0.35f * response_authority +
            0.15f * adaptive_rescue_response;
        const float omega = 2.6f * response_scale / smoothing;
        vec2 requested_acceleration =
            ( desired_offset - mem.assist_offset ) * ( omega * omega ) -
            mem.assist_velocity * ( 2.f * omega );

        // Preserve output continuity at object changes, but rapidly remove the old
        // correction component that is perpendicular or opposite to the new miss vector.
        if ( mem.target_switch_damping_remaining > 0.f &&
             length_sq( mem.target_switch_direction ) > 0.5f ) {
            const vec2 direction = mem.target_switch_direction;
            const float offset_parallel = dot( mem.assist_offset, direction );
            const float velocity_parallel = dot( mem.assist_velocity, direction );
            const vec2 incompatible_offset = mem.assist_offset -
                direction * std::max( offset_parallel, 0.f );
            const vec2 incompatible_velocity = mem.assist_velocity -
                direction * std::max( velocity_parallel, 0.f );
            const float transition_weight = saturate(
                mem.target_switch_damping_remaining / 0.080f );
            const float switch_clear_scale = 1.f +
                0.60f * adaptive_rescue_response;
            requested_acceleration += incompatible_offset *
                ( -omega * omega * 1.8f * switch_clear_scale * transition_weight );
            requested_acceleration += incompatible_velocity *
                ( -omega * 2.4f * switch_clear_scale * transition_weight );
            mem.target_switch_damping_remaining = std::max(
                0.f, mem.target_switch_damping_remaining - dt );
        }

        const float velocity_factor = 2.20f + 0.80f * response_authority +
            0.35f * adaptive_rescue_response;
        const float acceleration_factor = 3.0f + 0.80f * response_authority +
            0.50f * adaptive_rescue_response;
        const float max_velocity = std::max(
            60.f, max_displacement * velocity_factor / smoothing );
        const float max_acceleration = std::max(
            1800.f, max_velocity * acceleration_factor / smoothing );
        requested_acceleration = limit_length( requested_acceleration, max_acceleration );

        // Filtering bounded acceleration also keeps target switches from producing a sharp
        // acceleration discontinuity without exposing a separate jerk control.
        const float acceleration_filter_scale = 1.f + 0.30f * response_authority;
        const float acceleration_alpha = 1.f - std::exp( -dt / std::max(
            0.008f, smoothing * 0.18f / acceleration_filter_scale ) );
        mem.assist_acceleration +=
            ( requested_acceleration - mem.assist_acceleration ) * acceleration_alpha;
        mem.assist_acceleration = limit_length( mem.assist_acceleration, max_acceleration );

        mem.assist_velocity += mem.assist_acceleration * dt;
        mem.assist_velocity = limit_length( mem.assist_velocity, max_velocity );
        mem.assist_offset += mem.assist_velocity * dt;

        const float offset_length = length( mem.assist_offset );
        if ( offset_length > max_displacement && offset_length > 0.001f ) {
            mem.max_correction_clamped = true;
            const vec2 outward = mem.assist_offset * ( 1.f / offset_length );
            mem.assist_offset = outward * max_displacement;
            const float outward_speed = dot( mem.assist_velocity, outward );
            if ( outward_speed > 0.f )
                mem.assist_velocity = mem.assist_velocity - outward * outward_speed;
        }

        if ( !target && length_sq( mem.assist_offset ) < 0.0025f &&
             length_sq( mem.assist_velocity ) < 0.25f ) {
            mem.assist_offset = {};
            mem.assist_velocity = {};
            mem.assist_acceleration = {};
        }

        // Tablet: raw mapped pen position + bounded correction offset.
        // Mouse: sx/sy has already had the previous injected offset removed by c_aimbot.
        *ox = raw.x + mem.assist_offset.x;
        *oy = raw.y + mem.assist_offset.y;
        return true;
    }
}
