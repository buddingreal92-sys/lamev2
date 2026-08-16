#pragma once

#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <core/relax/nt_input.hxx>
#include <Windows.h>
#include <vector>
#include <algorithm>
#include <random>
#include <cmath>
#include <chrono>
#include <mutex>

namespace relax {

    enum class tap_style_t : int {
        alternate = 0,
        singletap = 1
    };

    enum class tap_style_state_t : int {
        singletap,
        burst,
        alternate,
        recovery
    };

    class c_relax {
    public:
        bool enabled = true;

        float ur = 70.f;

        int tap_style = static_cast<int>( tap_style_t::singletap );
        int primary_key = 0;
        int singletap_speed_bpm = 390;
        int burst_tolerance = 2;
        float stamina = 90.f;

        float k1_hold_center = 46.f;
        float k1_hold_spread = 10.f;

        float k2_hold_center = 46.f;
        float k2_hold_spread = 10.f;

        float hold_floor = 14.f;
        float hold_ceiling = 110.f;

        int32_t manual_offset_ms = 0;

        bool timing_variation = true;
        int32_t early_variation_ms = -14;
        int32_t late_variation_ms = 14;
        int32_t timing_drift_ms = 3;

        [[nodiscard]] size_t queue_size( ) const {
            std::lock_guard lock( m_mtx );
            return m_click_queue.size( );
        }
        [[nodiscard]] bool is_synced( ) const { std::lock_guard lock( m_mtx ); return m_in_play; }
        [[nodiscard]] int last_hit_obj_idx( ) const {
            std::lock_guard lock( m_mtx );
            return m_last_hit_obj_idx;
        }

        [[nodiscard]] bool is_active( ) const {
            std::lock_guard lock( m_mtx );
            return enabled && m_in_play && !m_click_queue.empty( );
        }

        void on_leave_play( const osu::game_snapshot_t& game ) {
            std::lock_guard lock( m_mtx );
            leave_play( game );
        }

    private:
        void leave_play( const osu::game_snapshot_t& game ) {
            release_all_keys( game );
            m_click_queue.clear( );
            m_last_hit_obj_idx = -1;
            m_scheduled_through_idx = -1;
            m_last_click_time = -99999;
            m_in_play = false;
            reset_tap_controller( );
            m_last_audio_time = 0;
            m_last_audio_sync_time = 0.0;
            m_last_jitter = 0.f;
            reset_timing_drift( );
        }

    public:

        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            std::lock_guard lock( m_mtx );
            if ( !enabled || game.cur_state != osu::game_state_t::play || !map.loaded || map.objects.empty( ) ) {
                if ( m_in_play )
                    leave_play( game );
                return;
            }

            m_in_play = true;
            const int game_time = game.cur_time;

            if ( game_time < m_last_game_time - 200 ) {
                reset_state( game );
            }

            schedule_clicks( game, map );

            m_last_game_time = game_time;

            advance_past_objects( game, map );
            purge_stale( game_time );
            flush_queue( game );
        }

    private:
        struct scheduled_click_t {
            int press_time = 0;
            int release_time = 0;
            int timing_variation_ms = 0;
            WORD key = 0;
            bool pressed = false;
            bool released = false;
        };

        std::vector<scheduled_click_t> m_click_queue;
        bool m_in_play = false;
        int m_last_game_time = 0;
        int m_last_hit_obj_idx = -1;
        int m_scheduled_through_idx = -1;
        int m_last_click_time = -99999;
        tap_style_state_t m_tap_state = tap_style_state_t::singletap;
        int m_last_key_index = -1;
        int m_recovery_notes = 0;
        float m_fatigue = 0.f;
        int m_controller_tap_style = -1;
        int m_controller_primary_key = -1;
        bool m_left_down = false;
        bool m_right_down = false;
        int m_last_audio_time = 0;
        double m_last_audio_sync_time = 0.0;
        float m_last_jitter = 0.f;
        float m_timing_drift = 0.f;
        float m_timing_drift_target = 0.f;
        int m_timing_drift_last_time = 0;
        int m_timing_drift_next_target_time = 0;
        bool m_timing_drift_initialized = false;

        c_nt_input m_nt;

        mutable std::mutex m_mtx;

        std::mt19937 m_rng{ []() -> uint32_t {
            try { return std::random_device{}( ); }
            catch ( ... ) { return static_cast<uint32_t>( std::chrono::high_resolution_clock::now( ).time_since_epoch( ).count( ) ); }
        }( ) };
        std::normal_distribution<float> m_norm{ 0.f, 1.f };
        std::uniform_real_distribution<float> m_unit{ 0.f, 1.f };
        std::uniform_real_distribution<float> m_sub_ms{ -1.49f, 1.49f };

        float generate_hold( float center, float spread ) {
            const float z = m_norm( m_rng );

            const float skew = 0.3f;
            const float shaped = z + skew * ( z * z - 1.f );

            float hold = center + shaped * spread;
            hold += ( m_unit( m_rng ) - 0.5f ) * 2.f;

            return std::clamp( hold, hold_floor, hold_ceiling );
        }

        float hit_jitter( const osu::beatmap_data_t& map ) {
            const float sigma = ur / 10.f;
            float new_jitter = m_norm( m_rng ) * ( sigma * 2.2f ) + m_sub_ms( m_rng );
            float jitter = 0.6f * m_last_jitter + 0.4f * new_jitter;
            m_last_jitter = jitter;

            float actual_od = map.od;
            if ( map.hr ) actual_od = std::min( 10.f, actual_od * 1.4f );
            if ( map.ez ) actual_od = actual_od * 0.5f;

            const float hit_window_300 = 79.5f - 6.f * actual_od;
            const float max_deviation = std::max( 5.f, hit_window_300 - 1.f );

            return std::clamp( jitter, -max_deviation, max_deviation );
        }

        void reset_timing_drift( ) {
            m_timing_drift = 0.f;
            m_timing_drift_target = 0.f;
            m_timing_drift_last_time = 0;
            m_timing_drift_next_target_time = 0;
            m_timing_drift_initialized = false;
        }

        float slow_timing_bias( int object_time ) {
            const float limit = static_cast<float>( std::clamp( timing_drift_ms, 0, 8 ) );
            if ( limit <= 0.f ) {
                reset_timing_drift( );
                return 0.f;
            }

            if ( !m_timing_drift_initialized ) {
                m_timing_drift_initialized = true;
                m_timing_drift_last_time = object_time;
                m_timing_drift_target = ( m_unit( m_rng ) * 2.f - 1.f ) * limit;
                m_timing_drift_next_target_time = object_time +
                    static_cast<int>( std::lround( 2000.f + m_unit( m_rng ) * 4000.f ) );
                return 0.f;
            }

            m_timing_drift_target = std::clamp( m_timing_drift_target, -limit, limit );
            m_timing_drift = std::clamp( m_timing_drift, -limit, limit );

            const int elapsed_ms = std::max( object_time - m_timing_drift_last_time, 0 );
            constexpr float response_ms = 2500.f;
            const float alpha = 1.f - std::exp(
                -static_cast<float>( elapsed_ms ) / response_ms );
            m_timing_drift += ( m_timing_drift_target - m_timing_drift ) * alpha;
            m_timing_drift = std::clamp( m_timing_drift, -limit, limit );
            m_timing_drift_last_time = object_time;

            if ( object_time >= m_timing_drift_next_target_time ) {
                m_timing_drift_target = ( m_unit( m_rng ) * 2.f - 1.f ) * limit;
                m_timing_drift_next_target_time = object_time +
                    static_cast<int>( std::lround( 2000.f + m_unit( m_rng ) * 4000.f ) );
            }

            return m_timing_drift;
        }

        static void resolve_keys( const osu::game_snapshot_t& game, WORD& k1, WORD& k2 ) {
            k1 = static_cast<WORD>( game.left_key );
            k2 = static_cast<WORD>( game.right_key );
            if ( !k1 ) k1 = 'Z';
            if ( !k2 ) k2 = 'X';
        }

        void reset_state( const osu::game_snapshot_t& game ) {
            release_all_keys( game );
            m_click_queue.clear( );
            m_last_hit_obj_idx = -1;
            m_scheduled_through_idx = -1;
            m_last_click_time = -99999;
            reset_tap_controller( );
            m_last_audio_time = 0;
            m_last_audio_sync_time = 0.0;
            m_last_jitter = 0.f;
            reset_timing_drift( );
        }

        void release_all_keys( const osu::game_snapshot_t& game ) {
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );
            const bool use_nt = m_nt.available( );
            if ( m_left_down ) {
                if ( use_nt ) m_nt.release( k1 );
                else input::release_vk( k1 );
                m_left_down = false;
            }
            if ( m_right_down ) {
                if ( use_nt ) m_nt.release( k2 );
                else input::release_vk( k2 );
                m_right_down = false;
            }
        }

        void press_key( WORD vk, bool& down ) {
            if ( !vk || down ) return;
            if ( m_nt.available( ) ) m_nt.press( vk );
            else input::press_vk( vk );
            down = true;
        }

        void release_key( WORD vk, bool& down ) {
            if ( !vk || !down ) return;
            if ( m_nt.available( ) ) m_nt.release( vk );
            else input::release_vk( vk );
            down = false;
        }

        struct tap_pattern_t {
            int notes = 1;
            float average_interval_ms = 9999.f;
            bool clearly_fast = false;
        };

        void reset_tap_controller( ) {
            m_tap_state = tap_style_state_t::singletap;
            m_last_key_index = -1;
            m_recovery_notes = 0;
            m_fatigue = 0.f;
            m_controller_tap_style = -1;
            m_controller_primary_key = -1;
        }

        float comfortable_interval_ms( ) const {
            const int speed = std::clamp( singletap_speed_bpm, 100, 400 );
            return 60000.f / static_cast<float>( speed );
        }

        int tolerated_burst_notes( ) const {
            static constexpr int notes_by_level[] = { 2, 4, 6 };
            return notes_by_level[ std::clamp( burst_tolerance, 0, 2 ) ];
        }

        tap_pattern_t analyze_tap_pattern( const osu::beatmap_data_t& map, int object_index ) const {
            tap_pattern_t result{};
            const float comfort_ms = comfortable_interval_ms( );
            const float transition_interval = comfort_ms * 1.12f;
            float interval_sum = 0.f;
            int interval_count = 0;

            // Seven intervals covers the current object plus up to seven upcoming heads.
            for ( int look = 0; look < 7; ++look ) {
                const int current = object_index + look;
                const int next = current + 1;
                if ( next >= static_cast<int>( map.objects.size( ) ) )
                    break;

                const auto& a = map.objects[ static_cast<size_t>( current ) ];
                const auto& b = map.objects[ static_cast<size_t>( next ) ];
                if ( ( a.type & 8 ) != 0 || ( b.type & 8 ) != 0 )
                    break;

                const int delta = b.start_time - a.start_time;
                if ( delta <= 0 || static_cast<float>( delta ) > transition_interval )
                    break;

                interval_sum += static_cast<float>( delta );
                interval_count++;
            }

            if ( interval_count > 0 ) {
                result.notes = interval_count + 1;
                result.average_interval_ms = interval_sum / static_cast<float>( interval_count );
                result.clearly_fast = result.average_interval_ms <= comfort_ms * 0.88f;
            }
            return result;
        }

        void recover_tap_stamina( const osu::beatmap_data_t& map, int object_index, int previous_interval_ms ) {
            if ( object_index <= 0 || previous_interval_ms <= 0 )
                return;

            const float endurance = std::clamp( stamina, 0.f, 100.f ) / 100.f;
            const float comfort_ms = comfortable_interval_ms( );
            const float extra_gap = std::max( 0.f, static_cast<float>( previous_interval_ms ) - comfort_ms * 0.75f );
            m_fatigue -= extra_gap * ( 0.00022f + endurance * 0.00012f );

            const auto& previous = map.objects[ static_cast<size_t>( object_index - 1 ) ];
            if ( ( previous.type & 8 ) != 0 || ( ( previous.type & 2 ) != 0 && previous.end_time > previous.start_time ) ) {
                const int held_rest = std::clamp( previous.end_time - previous.start_time, 0, previous_interval_ms );
                m_fatigue -= static_cast<float>( held_rest ) * ( 0.00035f + endurance * 0.00015f );
            }

            m_fatigue = std::clamp( m_fatigue, 0.f, 1.f );
            const int reset_gap = std::max( 650, static_cast<int>( std::round( comfort_ms * 3.f ) ) );
            if ( previous_interval_ms >= reset_gap || ( previous.type & 8 ) != 0 ) {
                m_fatigue = 0.f;
                m_tap_state = tap_style_state_t::singletap;
                m_recovery_notes = 0;
            }
        }

        void apply_tap_load( int previous_interval_ms, bool alternating ) {
            if ( previous_interval_ms <= 0 || previous_interval_ms > 2000 )
                return;

            const float endurance = std::clamp( stamina, 0.f, 100.f ) / 100.f;
            const float rate_ratio = comfortable_interval_ms( ) /
                static_cast<float>( std::max( previous_interval_ms, 1 ) );

            if ( alternating ) {
                m_fatigue -= 0.025f + endurance * 0.020f;
            }
            else if ( rate_ratio <= 0.72f ) {
                m_fatigue -= ( 0.72f - rate_ratio ) * ( 0.035f + endurance * 0.025f );
            }
            else {
                const float demand = rate_ratio - 0.72f;
                const float capacity = 0.55f + endurance * 0.75f;
                m_fatigue += demand * 0.11f / capacity;
            }
            m_fatigue = std::clamp( m_fatigue, 0.f, 1.f );
        }

        int choose_tap_key( const osu::beatmap_data_t& map, int object_index, int previous_interval_ms ) {
            const int primary = std::clamp( primary_key, 0, 1 );
            const int configured_style = std::clamp( tap_style, 0, 1 );
            if ( configured_style != m_controller_tap_style || primary != m_controller_primary_key ) {
                m_tap_state = tap_style_state_t::singletap;
                m_last_key_index = -1;
                m_recovery_notes = 0;
                m_fatigue = 0.f;
                m_controller_tap_style = configured_style;
                m_controller_primary_key = primary;
            }
            recover_tap_stamina( map, object_index, previous_interval_ms );

            if ( static_cast<tap_style_t>( configured_style ) == tap_style_t::alternate ) {
                m_tap_state = tap_style_state_t::alternate;
            }
            else {
                const auto& obj = map.objects[ static_cast<size_t>( object_index ) ];
                if ( ( obj.type & 8 ) != 0 ) {
                    m_tap_state = tap_style_state_t::singletap;
                    m_recovery_notes = 0;
                }
                else {
                    const tap_pattern_t pattern = analyze_tap_pattern( map, object_index );
                    const bool dense_pattern = pattern.notes >= 2;
                    const bool sustained = pattern.notes > tolerated_burst_notes( );
                    const float endurance = std::clamp( stamina, 0.f, 100.f ) / 100.f;
                    const bool fatigued = m_fatigue >= 0.45f + endurance * 0.45f;
                    const int recovery_gap = std::max( 360,
                        static_cast<int>( std::round( comfortable_interval_ms( ) * 1.65f ) ) );

                    switch ( m_tap_state ) {
                        case tap_style_state_t::singletap:
                            if ( dense_pattern && pattern.clearly_fast )
                                m_tap_state = sustained ? tap_style_state_t::alternate : tap_style_state_t::burst;
                            else if ( dense_pattern && ( sustained || fatigued ) )
                                m_tap_state = tap_style_state_t::alternate;
                            break;

                        case tap_style_state_t::burst:
                        case tap_style_state_t::alternate:
                            if ( !dense_pattern ) {
                                m_tap_state = tap_style_state_t::recovery;
                                m_recovery_notes = 1;
                            }
                            else if ( m_tap_state == tap_style_state_t::burst && sustained ) {
                                m_tap_state = tap_style_state_t::alternate;
                            }
                            break;

                        case tap_style_state_t::recovery:
                            if ( dense_pattern ) {
                                m_tap_state = sustained ? tap_style_state_t::alternate : tap_style_state_t::burst;
                            }
                            else if ( previous_interval_ms >= recovery_gap ) {
                                m_tap_state = tap_style_state_t::singletap;
                                m_recovery_notes = 0;
                            }
                            else if ( m_recovery_notes > 0 ) {
                                m_recovery_notes--;
                            }
                            else {
                                m_tap_state = tap_style_state_t::singletap;
                            }
                            break;
                    }
                }
            }

            const bool alternating = m_tap_state != tap_style_state_t::singletap;
            int chosen = primary;
            if ( alternating && m_last_key_index >= 0 )
                chosen = 1 - m_last_key_index;

            apply_tap_load( previous_interval_ms, alternating );
            m_last_key_index = chosen;
            return chosen;
        }

        void advance_past_objects( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            const int gt = game.cur_time;
            while ( m_last_hit_obj_idx + 1 < static_cast<int>( map.objects.size( ) ) ) {
                const auto& obj = map.objects[ static_cast<size_t>( m_last_hit_obj_idx + 1 ) ];
                if ( obj.start_time < gt - 50 )
                    m_last_hit_obj_idx++;
                else
                    break;
            }
        }

        void purge_stale( int game_time ) {
            m_click_queue.erase(
                std::remove_if( m_click_queue.begin( ), m_click_queue.end( ),
                    [ game_time ]( const scheduled_click_t& c ) {
                        return !c.pressed && c.press_time < game_time - 50;
                    } ),
                m_click_queue.end( ) );
        }

        void schedule_clicks( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );

            int current_stream_length = 0;
            int last_obj_start_time = -99999;
            if ( m_scheduled_through_idx >= 0 && m_scheduled_through_idx < static_cast<int>( map.objects.size( ) ) ) {
                last_obj_start_time = map.objects[ static_cast<size_t>( m_scheduled_through_idx ) ].start_time;
            }

            for ( int i = m_scheduled_through_idx + 1; i < static_cast<int>( map.objects.size( ) ); ++i ) {
                const auto& obj = map.objects[ static_cast<size_t>( i ) ];

                const int prev_interval = obj.start_time - last_obj_start_time;
                if ( prev_interval > 0 && prev_interval < 100 ) {
                    current_stream_length++;
                }
                else {
                    current_stream_length = 0;
                }
                last_obj_start_time = obj.start_time;
                const float jitter_ms = hit_jitter( map );
                float natural_error_ms = jitter_ms;
                int applied_variation = static_cast<int>( std::round( natural_error_ms ) );
                if ( timing_variation ) {
                    const int early = std::clamp( early_variation_ms, -25, 0 );
                    const int late = std::clamp( late_variation_ms, 0, 25 );
                    natural_error_ms += slow_timing_bias( obj.start_time );
                    applied_variation = static_cast<int>( std::round( natural_error_ms ) );
                    applied_variation = std::clamp( applied_variation, early, late );
                }

                const int base_press_time = obj.start_time + manual_offset_ms;
                int press_time = base_press_time + applied_variation;

                if ( timing_variation && m_last_click_time > -99999 ) {
                    if ( base_press_time > m_last_click_time ) {
                        // If the existing scheduler order was valid, variation may approach
                        // the previous hit but may never cross it.
                        press_time = std::max( press_time, m_last_click_time + 1 );
                    }
                    else if ( press_time < base_press_time ) {
                        // Do not let an early bounded UR value worsen an order collision
                        // that was already present in the legacy timing path.
                        press_time = base_press_time;
                    }
                    applied_variation = press_time - base_press_time;
                }

                const bool is_slider = ( obj.type & 2 ) != 0;
                const bool is_spinner = ( obj.type & 8 ) != 0;
                const int slider_dur = obj.end_time - obj.start_time;
                const int natural_hold = ( ( is_slider && slider_dur >= 120 ) || is_spinner ) ? slider_dur : 0;
                int hold_dur = 0;

                int next_interval = 9999;
                if ( i + 1 < static_cast<int>( map.objects.size( ) ) ) {
                    next_interval = map.objects[ static_cast<size_t>( i + 1 ) ].start_time - obj.start_time;
                    if ( next_interval < 0 ) next_interval = 0;
                }

                const int chosen_key_index = choose_tap_key( map, i, prev_interval );
                const WORD chosen = chosen_key_index == 1 ? k2 : k1;
                if ( !chosen ) continue;

                if ( natural_hold > 0 ) {
                    const float tail = ( m_unit( m_rng ) * 2.f - 1.f ) * 5.f + m_sub_ms( m_rng );
                    hold_dur = natural_hold + static_cast<int>( tail );
                    if ( hold_dur < 15 ) hold_dur = 15;
                }
                else {
                    float center = chosen_key_index == 1 ? k2_hold_center : k1_hold_center;
                    float spread = chosen_key_index == 1 ? k2_hold_spread : k1_hold_spread;

                    if ( next_interval < 250 ) {
                        float max_allowed_center = static_cast<float>( next_interval ) * 0.52f;
                        float dynamic_floor = 34.f + m_unit( m_rng ) * 6.f;
                        if ( max_allowed_center < dynamic_floor ) {
                            max_allowed_center = dynamic_floor;
                        }
                        if ( max_allowed_center < center ) {
                            spread = spread * std::sqrt( max_allowed_center / center );
                            center = max_allowed_center;
                        }
                    }
                    float map_drift = ( static_cast<float>( i ) / static_cast<float>( map.objects.size( ) ) ) * 7.f;
                    float stream_drift = std::min( static_cast<float>( current_stream_length ) * 1.3f, 20.f );
                    float total_drift = map_drift + stream_drift;

                    center += total_drift;
                    float safe_cap = static_cast<float>( next_interval ) * 0.85f;
                    if ( center > safe_cap ) {
                        center = safe_cap;
                    }

                    hold_dur = static_cast<int>( generate_hold( center, spread ) );
                }

                if ( is_slider ) {
                    // Compensate from the final scheduled head press so the existing real
                    // slider endpoint remains the minimum release target.
                    const int needed_hold = obj.end_time - press_time;
                    const int floor_hold = slider_dur - 30;
                    if ( hold_dur < floor_hold ) hold_dur = floor_hold;
                    if ( hold_dur < needed_hold ) hold_dur = needed_hold;
                }

                const int release_time = press_time + hold_dur;

                m_click_queue.push_back( {
                    press_time, release_time, applied_variation, chosen, false, false } );
                m_last_click_time = press_time;
                m_scheduled_through_idx = i;
            }
        }

        inline double get_time_ms( ) {
            static const auto start = std::chrono::high_resolution_clock::now( );
            return std::chrono::duration<double, std::milli>( std::chrono::high_resolution_clock::now( ) - start ).count( );
        }

        double get_interpolated_game_time( const osu::game_snapshot_t& game ) {
            const double now = get_time_ms( );
            const int gt = game.cur_time;
            
            if ( gt != m_last_audio_time ) {
                m_last_audio_time = gt;
                m_last_audio_sync_time = now;
            }
            
            double elapsed = now - m_last_audio_sync_time;
            const double speed = game.speed_mult > 0.01f ? game.speed_mult : 1.f;
            
            if ( elapsed > 30.0 / speed ) {
                elapsed = 0.0;
            }
            
            return static_cast<double>( gt ) + elapsed * speed;
        }

        void flush_queue( const osu::game_snapshot_t& game ) {
            const double est_gt = get_interpolated_game_time( game );
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );

            for ( auto& c : m_click_queue ) {
                if ( !c.pressed && est_gt >= static_cast<double>( c.press_time ) ) {
                    bool& ref = ( c.key == k1 ) ? m_left_down : m_right_down;
                    press_key( c.key, ref );
                    c.pressed = true;
                    continue;
                }
                if ( c.pressed && !c.released && est_gt >= static_cast<double>( c.release_time ) ) {
                    bool& ref = ( c.key == k1 ) ? m_left_down : m_right_down;
                    release_key( c.key, ref );
                    c.released = true;
                }
            }

            m_click_queue.erase(
                std::remove_if( m_click_queue.begin( ), m_click_queue.end( ),
                    []( const scheduled_click_t& c ) { return c.released; } ),
                m_click_queue.end( ) );
        }
    };

}
