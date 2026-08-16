#pragma once

#include <impl/defs/offsets_lazer.hxx>
#include <impl/memory/process.hxx>
#include <impl/util/playfield.hxx>
#include <impl/memory/input.hxx>
#include <impl/util/debug_log.hxx>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>
#include <array>
#include <Windows.h>

namespace beatmap {

    class c_lazer_ruleset_reader {
    private:
        struct resolved_chain_t {
            uint64_t drawable = 0;
            uint64_t beatmap = 0;
            uint64_t hit_list = 0;
            uint64_t items = 0;
            int32_t count = 0;
            uint32_t player_drawable_field = 0;
            uint32_t drawable_beatmap_field = 0;
            uint32_t beatmap_hitobjects_field = 0;
            uint32_t start_bindable_field = 0;
            uint32_t bindable_number_field = 0;
            bool valid = false;
        };

        static bool plausible_ptr( uint64_t p ) {
            return p >= 0x10000ull && p < 0x0000800000000000ull && ( p & 0x7ull ) == 0;
        }

        static bool plausible_managed_object( memory::c_process& process, uint64_t p ) {
            if ( !plausible_ptr( p ) )
                return false;
            const auto mt = process.read<uint64_t>( p );
            return plausible_ptr( mt );
        }

        static std::vector<uint32_t> nearby_offsets( uint32_t base, uint32_t radius ) {
            std::vector<uint32_t> values;
            values.reserve( static_cast<size_t>( radius / 4 + 2 ) );
            values.push_back( base );
            for ( uint32_t d = 8; d <= radius; d += 8 ) {
                values.push_back( base + d );
                if ( base >= d )
                    values.push_back( base - d );
            }
            return values;
        }

        static bool read_start_time_at(
            memory::c_process& process,
            uint64_t obj_ptr,
            uint32_t start_bindable_field,
            uint32_t bindable_number_field,
            double& out_time ) {

            const auto start_bindable = process.read<uint64_t>( obj_ptr + start_bindable_field );
            if ( !plausible_managed_object( process, start_bindable ) )
                return false;

            const double t = process.read<double>( start_bindable + bindable_number_field );
            if ( !std::isfinite( t ) || t < -10000.0 || t > 3600000.0 )
                return false;

            out_time = t;
            return true;
        }

        static bool resolve_start_layout(
            memory::c_process& process,
            uint64_t items,
            int32_t count,
            const offsets::lazer::table_t& off,
            uint32_t& start_field,
            uint32_t& value_field ) {

            const std::array<int32_t, 3> indices = {
                0,
                count > 2 ? count / 2 : 0,
                count - 1
            };
            std::array<uint64_t, 3> objects{};
            for ( size_t i = 0; i < indices.size( ); ++i ) {
                objects[ i ] = process.read<uint64_t>(
                    items + off.array_first_element + static_cast<uint64_t>( indices[ i ] ) * 8 );
                if ( !plausible_managed_object( process, objects[ i ] ) )
                    return false;
            }

            const auto start_fields = nearby_offsets( off.hit_object_start_time_bindable, 0x40 );
            const auto value_fields = nearby_offsets( off.bindable_number_value, 0x30 );

            for ( const auto sf : start_fields ) {
                std::array<uint64_t, 3> bindables{};
                bool bindables_ok = true;
                for ( size_t i = 0; i < objects.size( ); ++i ) {
                    bindables[ i ] = process.read<uint64_t>( objects[ i ] + sf );
                    if ( !plausible_managed_object( process, bindables[ i ] ) ) {
                        bindables_ok = false;
                        break;
                    }
                }
                if ( !bindables_ok )
                    continue;

                for ( const auto vf : value_fields ) {
                    double prev = -1e30;
                    bool times_ok = true;
                    for ( size_t i = 0; i < bindables.size( ); ++i ) {
                        const double t = process.read<double>( bindables[ i ] + vf );
                        if ( !std::isfinite( t ) || t < -10000.0 || t > 3600000.0 || t + 1.0 < prev ) {
                            times_ok = false;
                            break;
                        }
                        prev = t;
                    }
                    if ( times_ok ) {
                        start_field = sf;
                        value_field = vf;
                        return true;
                    }
                }
            }

            return false;
        }

        static bool validate_hit_list(
            memory::c_process& process,
            uint64_t list,
            const offsets::lazer::table_t& off,
            resolved_chain_t& out ) {

            if ( !plausible_managed_object( process, list ) )
                return false;

            const auto items = process.read<uint64_t>( list + off.list_items );
            const auto count = process.read<int32_t>( list + off.list_size );
            if ( !plausible_managed_object( process, items ) || count <= 0 || count > 100000 )
                return false;

            uint32_t start_field = off.hit_object_start_time_bindable;
            uint32_t value_field = off.bindable_number_value;

            // Exact old timing layout first (cheap). If it moved, resolve only these two
            // fields from three real objects in this list.
            bool exact_ok = true;
            const std::array<int32_t, 3> indices = { 0, count > 2 ? count / 2 : 0, count - 1 };
            double prev = -1e30;
            for ( int32_t idx : indices ) {
                const auto obj = process.read<uint64_t>(
                    items + off.array_first_element + static_cast<uint64_t>( idx ) * 8 );
                double t = 0.0;
                if ( !plausible_managed_object( process, obj ) ||
                     !read_start_time_at( process, obj, start_field, value_field, t ) ||
                     t + 1.0 < prev ) {
                    exact_ok = false;
                    break;
                }
                prev = t;
            }

            if ( !exact_ok && !resolve_start_layout( process, items, count, off, start_field, value_field ) )
                return false;

            out.hit_list = list;
            out.items = items;
            out.count = count;
            out.start_bindable_field = start_field;
            out.bindable_number_field = value_field;
            return true;
        }

        static bool try_chain(
            memory::c_process& process,
            uint64_t drawable,
            uint32_t player_field,
            uint32_t beatmap_field,
            uint32_t hitobjects_field,
            const offsets::lazer::table_t& off,
            resolved_chain_t& out ) {

            if ( !plausible_managed_object( process, drawable ) )
                return false;

            const auto beatmap = process.read<uint64_t>( drawable + beatmap_field );
            if ( !plausible_managed_object( process, beatmap ) )
                return false;

            const auto hit_list = process.read<uint64_t>( beatmap + hitobjects_field );
            resolved_chain_t candidate{};
            if ( !validate_hit_list( process, hit_list, off, candidate ) )
                return false;

            candidate.drawable = drawable;
            candidate.beatmap = beatmap;
            candidate.player_drawable_field = player_field;
            candidate.drawable_beatmap_field = beatmap_field;
            candidate.beatmap_hitobjects_field = hitobjects_field;
            candidate.valid = true;
            out = candidate;
            return true;
        }

        static bool resolve_chain(
            memory::c_process& process,
            const osu::game_snapshot_t& game,
            const offsets::lazer::table_t& off,
            resolved_chain_t& out ) {

            // Cache a successful resolution for this Player object. If resolution fails,
            // throttle retries so a bad offset cannot peg a CPU core.
            static int32_t cached_pid = 0;
            static uint64_t cached_player = 0;
            static resolved_chain_t cached{};
            static ULONGLONG last_attempt = 0;

            const ULONGLONG now = GetTickCount64( );
            const bool same_player = cached_pid == process.pid( ) && cached_player == game.player_screen;

            if ( same_player && cached.valid ) {
                resolved_chain_t revalidated{};
                if ( try_chain( process, cached.drawable, cached.player_drawable_field,
                        cached.drawable_beatmap_field, cached.beatmap_hitobjects_field, off, revalidated ) ) {
                    out = revalidated;
                    return true;
                }
                cached = {};
            }

            if ( same_player && now - last_attempt < 750 )
                return false;

            cached_pid = process.pid( );
            cached_player = game.player_screen;
            last_attempt = now;
            cached = {};

            const auto beatmap_fields = nearby_offsets( off.drawable_osu_beatmap, 0x80 );
            const auto hitlist_fields = nearby_offsets( off.beatmap_hit_objects, 0x20 );

            // 1) Try the currently configured DrawableRuleset first.
            if ( plausible_managed_object( process, game.drawable_ruleset ) ) {
                for ( const auto beatmap_field : beatmap_fields ) {
                    for ( const auto hitlist_field : hitlist_fields ) {
                        if ( try_chain( process, game.drawable_ruleset, off.player_drawable_ruleset,
                                beatmap_field, hitlist_field, off, out ) ) {
                            cached = out;
                            dbg::log( "lazer chain resolved: player_drawable=0x%X drawable_beatmap=0x%X beatmap_hitobjects=0x%X start_bindable=0x%X bindable_value=0x%X count=%d",
                                out.player_drawable_field, out.drawable_beatmap_field,
                                out.beatmap_hitobjects_field, out.start_bindable_field, out.bindable_number_field, out.count );
                            return true;
                        }
                    }
                }
            }

            // 2) If Player::DrawableRuleset itself moved, look only near the old field.
            // This runs at most once every 750ms while broken, not every game update.
            if ( plausible_managed_object( process, game.player_screen ) ) {
                const auto player_fields = nearby_offsets( off.player_drawable_ruleset, 0x80 );
                for ( const auto player_field : player_fields ) {
                    const auto drawable = process.read<uint64_t>( game.player_screen + player_field );
                    if ( !plausible_managed_object( process, drawable ) )
                        continue;

                    // Fast path: exact downstream fields.
                    if ( try_chain( process, drawable, player_field,
                            off.drawable_osu_beatmap, off.beatmap_hit_objects, off, out ) ) {
                        cached = out;
                        dbg::log( "lazer chain resolved: player_drawable=0x%X drawable_beatmap=0x%X beatmap_hitobjects=0x%X start_bindable=0x%X bindable_value=0x%X count=%d",
                            out.player_drawable_field, out.drawable_beatmap_field,
                            out.beatmap_hitobjects_field, out.start_bindable_field, out.bindable_number_field, out.count );
                        return true;
                    }

                    for ( const auto beatmap_field : beatmap_fields ) {
                        for ( const auto hitlist_field : hitlist_fields ) {
                            if ( try_chain( process, drawable, player_field,
                                    beatmap_field, hitlist_field, off, out ) ) {
                                cached = out;
                                dbg::log( "lazer chain resolved: player_drawable=0x%X drawable_beatmap=0x%X beatmap_hitobjects=0x%X start_bindable=0x%X bindable_value=0x%X count=%d",
                                    out.player_drawable_field, out.drawable_beatmap_field,
                                    out.beatmap_hitobjects_field, out.start_bindable_field, out.bindable_number_field, out.count );
                                return true;
                            }
                        }
                    }
                }
            }

            return false;
        }

    public:
        bool try_load(
            memory::c_process& process,
            const osu::game_snapshot_t& game,
            const offsets::lazer::table_t& off,
            osu::beatmap_data_t& out ) {

            if ( !off.has_hitobject_offsets( ) )
                return false;

            if ( game.player_screen == 0 || game.cur_state != osu::game_state_t::play )
                return false;

            resolved_chain_t chain{};
            if ( !resolve_chain( process, game, off, chain ) ) {
                out.error = "lazer timing chain unresolved";
                return false;
            }

            out.screen_width = 1920;
            out.screen_height = 1080;
            playfield::get_window_size( input::target_window( ), out.screen_width, out.screen_height );
            out.hr = ( game.cur_mod_state & 16 ) != 0;

            out.cs = 5.f;
            out.od = 5.f;
            out.ar = 5.f;

            const auto difficulty = process.read<uint64_t>( chain.beatmap + 0x08 );
            if ( plausible_managed_object( process, difficulty ) ) {
                const float cs_val = process.read<float>( difficulty + 0x1c );
                const float od_val = process.read<float>( difficulty + 0x20 );
                const float ar_val = process.read<float>( difficulty + 0x24 );
                if ( cs_val >= 0.f && cs_val <= 12.f ) out.cs = cs_val;
                if ( od_val >= 0.f && od_val <= 12.f ) out.od = od_val;
                if ( ar_val >= 0.f && ar_val <= 13.f ) out.ar = ar_val;
            }

            std::vector<osu::hit_object_t> objects;
            objects.reserve( static_cast<size_t>( chain.count ) );

            for ( int32_t i = 0; i < chain.count; ++i ) {
                const auto obj_ptr = process.read<uint64_t>(
                    chain.items + off.array_first_element + static_cast<uint64_t>( i ) * 8 );
                if ( !plausible_managed_object( process, obj_ptr ) )
                    continue;

                double start_time_raw = 0.0;
                if ( !read_start_time_at( process, obj_ptr, chain.start_bindable_field,
                        chain.bindable_number_field, start_time_raw ) )
                    continue;

                // Relax only needs timing. Keep position support when the old position
                // field still validates, but do not throw the entire object away if it moved.
                float x = process.read<float>( obj_ptr + off.osu_hit_object_position_xy );
                float y = process.read<float>( obj_ptr + off.osu_hit_object_position_xy + 4 );
                if ( !std::isfinite( x ) || !std::isfinite( y ) ||
                     x < -1024.f || x > 1536.f || y < -768.f || y > 1152.f ) {
                    x = 256.f;
                    y = 192.f;
                }

                osu::hit_object_t obj{};
                obj.start_time = static_cast<int32_t>( start_time_raw );
                obj.end_time = obj.start_time;
                obj.x = x;
                obj.y = out.hr ? ( 384.f - y ) : y;
                obj.type = static_cast<uint8_t>( osu::hit_object_type_t::circle );

                // Duration objects (sliders / spinners) are identified from their actual nested
                // hit-object timeline instead of the old per-object 0xD0 heuristic.  The latter can
                // legitimately read as zero on some Slider instances, which made Relax treat those
                // sliders as circles and release after a normal ~48 ms tap.
                if ( read_nested_end( process, off, chain, obj_ptr, obj ) ) {
                    const bool is_center =
                        std::abs( x - 256.f ) < 1.f && std::abs( y - 192.f ) < 1.f;

                    if ( is_center ) {
                        obj.type = static_cast<uint8_t>( osu::hit_object_type_t::spinner );
                    }
                    else {
                        obj.type = static_cast<uint8_t>( osu::hit_object_type_t::slider );
                        read_slider_path( process, off, obj_ptr, obj, out.hr );
                    }
                }

                objects.push_back( obj );
            }

            if ( objects.empty( ) ) {
                out.error = "lazer timing chain resolved but no hit object times decoded";
                return false;
            }

            std::sort( objects.begin( ), objects.end( ),
                []( const osu::hit_object_t& a, const osu::hit_object_t& b ) {
                    return a.start_time < b.start_time;
                } );

            for ( size_t i = 0; i < objects.size( ); ++i ) {
                objects[ i ].stack_index = 0;
                for ( int32_t j = static_cast<int32_t>( i ) - 1; j >= 0; --j ) {
                    const auto& prev = objects[ static_cast<size_t>( j ) ];
                    if ( objects[ i ].start_time - prev.start_time > 2000 )
                        break;

                    const float dx = objects[ i ].x - prev.x;
                    const float dy = objects[ i ].y - prev.y;
                    const float dist = std::sqrt( dx * dx + dy * dy );
                    if ( dist < 5.0f ) {
                        objects[ i ].stack_index = prev.stack_index + 1;
                        break;
                    }
                }
            }

            for ( auto& obj : objects )
                project_to_screen( obj, out.screen_width, out.screen_height );

            out.objects = std::move( objects );
            out.loaded = true;
            out.beatmap_path = "(memory:auto-resolved lazer timing chain)";
            return true;
        }

    private:
        static bool read_nested_end(
            memory::c_process& process,
            const offsets::lazer::table_t& off,
            const resolved_chain_t& chain,
            uint64_t obj_ptr,
            osu::hit_object_t& obj ) {

            // The auto-resolver already discovered the current HitObject::StartTimeBindable
            // and Bindable<double>::Value fields. Re-use those for nested objects as well.
            // Also resolve the nested List<HitObject> field itself near the old location.
            static int32_t cached_pid = 0;
            static uint32_t cached_nested_field = 0;

            if ( cached_pid != process.pid( ) ) {
                cached_pid = process.pid( );
                cached_nested_field = 0;
            }

            auto try_nested_field = [&]( uint32_t nested_field ) -> bool {
                const auto nested = process.read<uint64_t>( obj_ptr + nested_field );
                if ( !plausible_managed_object( process, nested ) )
                    return false;

                const auto n_items = process.read<uint64_t>( nested + off.list_items );
                const auto n_count = process.read<int32_t>( nested + off.list_size );
                if ( !plausible_managed_object( process, n_items ) || n_count <= 0 || n_count > 1000 )
                    return false;

                // Slider/Spinner nested objects are sorted by StartTime by HitObject::ApplyDefaults().
                // Validate a few points so a random List field cannot be mistaken for the timeline.
                const std::array<int32_t, 3> indices = {
                    0,
                    n_count > 2 ? n_count / 2 : 0,
                    n_count - 1
                };

                double prev = static_cast<double>( obj.start_time ) - 5.0;
                double end_time_raw = 0.0;
                for ( const int32_t idx : indices ) {
                    const auto child = process.read<uint64_t>(
                        n_items + off.array_first_element + static_cast<uint64_t>( idx ) * 8 );
                    if ( !plausible_managed_object( process, child ) )
                        return false;

                    double t = 0.0;
                    if ( !read_start_time_at( process, child, chain.start_bindable_field,
                            chain.bindable_number_field, t ) )
                        return false;

                    if ( t + 1.0 < prev || t < static_cast<double>( obj.start_time ) - 5.0 ||
                         t > static_cast<double>( obj.start_time ) + 300000.0 )
                        return false;

                    prev = t;
                    end_time_raw = t;
                }

                // Circles have an empty nested list. For sliders/spinners the last nested object's
                // start time is a real duration endpoint (slider tail / final spinner tick).
                if ( end_time_raw <= static_cast<double>( obj.start_time ) + 1.0 )
                    return false;

                obj.end_time = static_cast<int32_t>( std::round( end_time_raw ) );
                return true;
            };

            if ( cached_nested_field != 0 && try_nested_field( cached_nested_field ) )
                return true;

            const auto nested_fields = nearby_offsets( off.hit_object_nested_objects, 0x80 );
            for ( const auto field : nested_fields ) {
                if ( try_nested_field( field ) ) {
                    if ( cached_nested_field != field ) {
                        cached_nested_field = field;
                        dbg::log( "lazer nested duration field resolved: 0x%X", field );
                    }
                    return true;
                }
            }

            return false;
        }

        static void read_slider_path(
            memory::c_process& process,
            const offsets::lazer::table_t& off,
            uint64_t obj_ptr,
            osu::hit_object_t& obj,
            bool hr ) {

            const auto path_wrap = process.read<uint64_t>( obj_ptr + off.slider_path_wrapper );
            if ( !path_wrap ) return;

            const auto bl = process.read<uint64_t>( path_wrap + off.path_ctrl_points_list );
            if ( !bl ) return;

            const auto cp_items = process.read<uint64_t>( bl + off.list_items );
            const auto cp_count = process.read<int32_t>( bl + off.list_size );
            if ( !cp_items || cp_count < 2 || cp_count > 500 ) return;

            const size_t data_size = static_cast<size_t>( cp_count ) * 8;
            std::vector<uint8_t> buf( data_size );
            if ( !process.read_buffer( cp_items + off.array_first_element, buf.data( ), data_size ) )
                return;

            const auto type_bind = process.read<uint64_t>( obj_ptr + off.slider_path_wrapper + 0x10 );
            char type_char = 'L';
            if ( type_bind ) {
                int32_t type_val = process.read<int32_t>( type_bind + off.bindable_value );
                if ( type_val == 0 ) type_char = 'C';
                else if ( type_val == 1 ) type_char = 'B';
                else if ( type_val == 2 ) type_char = 'L';
                else if ( type_val == 3 ) type_char = 'P';
            }

            std::ostringstream curve;
            curve << type_char;

            float total_len = 0.f;
            float prev_x = obj.x;
            float prev_y = obj.y;

            for ( int32_t ci = 1; ci < cp_count; ++ci ) {
                const size_t base = static_cast<size_t>( ci ) * 8;
                float ox, oy;
                std::memcpy( &ox, &buf[ base ], 4 );
                std::memcpy( &oy, &buf[ base + 4 ], 4 );

                if ( !std::isfinite( ox ) || !std::isfinite( oy ) )
                    continue;
                if ( std::abs( ox ) > 1000.f || std::abs( oy ) > 1000.f )
                    continue;

                float abs_x = obj.x + ox;
                float abs_y = hr ? ( obj.y - oy ) : ( obj.y + oy );

                curve << "|" << abs_x << ":" << abs_y;

                float dx = abs_x - prev_x;
                float dy = abs_y - prev_y;
                total_len += std::sqrt( dx * dx + dy * dy );
                prev_x = abs_x;
                prev_y = abs_y;
            }

            const std::string curve_str = curve.str( );
            if ( curve_str.size( ) > 2 ) {
                obj.slider_curve_str = curve_str;
                obj.slider_length = total_len;
            }
        }

        static void project_to_screen( osu::hit_object_t& obj, int32_t sw, int32_t sh ) {
            const float playfield_height = sh * 0.8f;
            const float playfield_width = playfield_height * ( 4.f / 3.f );
            const float scale = playfield_width / 512.f;
            const float offset_x = ( sw - playfield_width ) * 0.5f;
            const float offset_y = ( sh - playfield_height ) * 0.5f;

            const float stack_offset = -static_cast<float>( obj.stack_index ) * 6.f * scale;
            obj.screen_x = offset_x + ( obj.x * scale ) + stack_offset;
            obj.screen_y = offset_y + ( obj.y * scale ) + stack_offset + 17.f;
        }
    };

}