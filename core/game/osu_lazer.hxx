#pragma once

#include <core/game/i_osu_client.hxx>
#include <impl/defs/offsets_lazer.hxx>
#include <impl/memory/scanner.hxx>
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <vector>

namespace game {

    struct resolved_mod_tokens_t {
        uint32_t easy = 0;
        uint32_t hidden = 0;
        uint32_t hardrock = 0;
        uint32_t doubletime = 0;
        uint32_t halftime = 0;
        uint32_t nightcore = 0;
        uint32_t flashlight = 0;
        bool resolved = false;
    };

    class c_osu_lazer : public i_osu_client {
    public:
        explicit c_osu_lazer( offsets::lazer::table_t offsets ) : m_off( offsets ) {}

        bool attach( memory::c_process& process ) override {
            const wchar_t* mods[] = {
                L"osu.Game.dll",
                L"osu.Framework.dll",
                L"osu.Game.Rulesets.Osu.dll",
                L"osu.Game.Rulesets.dll",
            };
            for ( auto mod_name : mods ) {
                if ( scan_module_for_game( process, mod_name ) )
                    return true;
            }

            if ( scan_private_memory_for_game( process ) )
                return true;

            return false;
        }

        static uint64_t get_module_base( int32_t pid, const wchar_t* name ) {
            HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, static_cast<DWORD>( pid ) );
            if ( snap == INVALID_HANDLE_VALUE )
                return 0;

            MODULEENTRY32W entry{};
            entry.dwSize = sizeof( entry );

            uint64_t base = 0;
            if ( Module32FirstW( snap, &entry ) ) {
                do {
                    if ( _wcsicmp( entry.szModule, name ) == 0 ) {
                        base = reinterpret_cast<uint64_t>( entry.modBaseAddr );
                        break;
                    }
                } while ( Module32NextW( snap, &entry ) );
            }

            CloseHandle( snap );
            return base;
        }

        void resolve_tokens( memory::c_process& process ) {
            if ( m_tokens.resolved )
                return;

            const uint64_t dll_base = get_module_base( process.pid( ), L"osu.Game.Rulesets.Osu.dll" );
            if ( !dll_base )
                return;

            const auto magic = process.read<uint16_t>( dll_base );
            if ( magic != 0x5a4d )
                return;

            const auto pe_offset = process.read<uint32_t>( dll_base + 0x3c );
            const auto pe_sig = process.read<uint32_t>( dll_base + pe_offset );
            if ( pe_sig != 0x00004550 )
                return;

            const auto opt_magic = process.read<uint16_t>( dll_base + pe_offset + 24 );
            const bool is_64bit = opt_magic == 0x20b;

            const auto data_dir_offset = pe_offset + 24 + ( is_64bit ? 112 : 96 );

            const auto cli_rva = process.read<uint32_t>( dll_base + data_dir_offset + 14 * 8 );
            if ( cli_rva == 0 )
                return;

            const auto cli_header_addr = dll_base + cli_rva;

            const auto meta_rva = process.read<uint32_t>( cli_header_addr + 8 );
            const auto meta_addr = dll_base + meta_rva;

            char bsjb[4] = { 0 };
            process.read_buffer( meta_addr, bsjb, 4 );
            if ( bsjb[0] != 'B' || bsjb[1] != 'S' || bsjb[2] != 'J' || bsjb[3] != 'B' )
                return;

            const auto version_len = process.read<uint32_t>( meta_addr + 12 );
            const auto streams_offset = 16 + version_len;
            const auto aligned_streams_offset = ( streams_offset + 3 ) & ~3;

            const auto num_streams = process.read<uint16_t>( meta_addr + aligned_streams_offset + 2 );

            uint64_t curr_stream_header = meta_addr + aligned_streams_offset + 4;
            uint64_t table_stream_addr = 0;
            uint64_t string_stream_addr = 0;

            for ( uint16_t i = 0; i < num_streams; ++i ) {
                const auto offset = process.read<uint32_t>( curr_stream_header );

                char name_buf[32] = { 0 };
                process.read_buffer( curr_stream_header + 8, name_buf, sizeof( name_buf ) - 1 );
                std::string name( name_buf );

                if ( name == "#~" || name == "#-" ) {
                    table_stream_addr = meta_addr + offset;
                } else if ( name == "#Strings" ) {
                    string_stream_addr = meta_addr + offset;
                }

                const auto name_len = name.length( );
                const auto total_len = 8 + name_len + 1;
                const auto aligned_total_len = ( total_len + 3 ) & ~3;
                curr_stream_header += aligned_total_len;
            }

            if ( table_stream_addr == 0 || string_stream_addr == 0 )
                return;

            const auto heap_sizes = process.read<uint8_t>( table_stream_addr + 6 );
            const auto valid_mask = process.read<uint64_t>( table_stream_addr + 8 );

            const auto string_idx_size = ( heap_sizes & 0x01 ) ? 4 : 2;
            const auto guid_idx_size = ( heap_sizes & 0x02 ) ? 4 : 2;

            uint32_t row_counts[64] = { 0 };
            uint64_t curr_ptr = table_stream_addr + 24;
            for ( int i = 0; i < 64; ++i ) {
                if ( ( valid_mask >> i ) & 1 ) {
                    row_counts[i] = process.read<uint32_t>( curr_ptr );
                    curr_ptr += 4;
                }
            }

            uint64_t table_data_ptr = curr_ptr;

            table_data_ptr += row_counts[0x00] * ( 2 + string_idx_size + 3 * guid_idx_size );

            const auto max_scope_rows = (std::max)( { row_counts[0x00], row_counts[0x1a], row_counts[0x23], row_counts[0x01] } );
            const auto resolution_scope_size = ( max_scope_rows < 16384 ) ? 2 : 4;
            table_data_ptr += row_counts[0x01] * ( resolution_scope_size + 2 * string_idx_size );

            const auto max_tdr_rows = (std::max)( { row_counts[0x02], row_counts[0x01], row_counts[0x1b] } );
            const auto typedef_or_ref_size = ( max_tdr_rows < 16384 ) ? 2 : 4;
            const auto field_idx_size = ( row_counts[0x04] < 65536 ) ? 2 : 4;
            const auto method_idx_size = ( row_counts[0x06] < 65536 ) ? 2 : 4;
            const auto typedef_row_size = 4 + 2 * string_idx_size + typedef_or_ref_size + field_idx_size + method_idx_size;

            const auto num_typedefs = row_counts[0x02];
            for ( uint32_t idx = 0; idx < num_typedefs; ++idx ) {
                const auto row_addr = table_data_ptr + idx * typedef_row_size;
                uint32_t name_idx = 0;
                if ( string_idx_size == 2 ) {
                    name_idx = process.read<uint16_t>( row_addr + 4 );
                } else {
                    name_idx = process.read<uint32_t>( row_addr + 4 );
                }

                char name_buf[64] = { 0 };
                process.read_buffer( string_stream_addr + name_idx, name_buf, sizeof( name_buf ) - 1 );
                std::string name( name_buf );

                const uint32_t token = 0x02000000 | ( idx + 1 );
                if ( name == "OsuModEasy" ) {
                    m_tokens.easy = token;
                } else if ( name == "OsuModHidden" ) {
                    m_tokens.hidden = token;
                } else if ( name == "OsuModHardRock" ) {
                    m_tokens.hardrock = token;
                } else if ( name == "OsuModDoubleTime" ) {
                    m_tokens.doubletime = token;
                } else if ( name == "OsuModHalfTime" ) {
                    m_tokens.halftime = token;
                } else if ( name == "OsuModNightcore" ) {
                    m_tokens.nightcore = token;
                } else if ( name == "OsuModFlashlight" ) {
                    m_tokens.flashlight = token;
                }
            }

            m_tokens.resolved = true;
        }

        int32_t read_lazer_mods( memory::c_process& process ) {
            int32_t mods = 0;
            const auto selected_mods_bindable = process.read<uint64_t>( m_game_base + 1120 );
            if ( !selected_mods_bindable )
                return mods;

            const auto selected_mods_list = process.read<uint64_t>( selected_mods_bindable + m_off.bindable_value );
            if ( !selected_mods_list )
                return mods;

            const auto items = process.read<uint64_t>( selected_mods_list + m_off.list_items );
            const auto size = process.read<int32_t>( selected_mods_list + m_off.list_size );
            if ( !items || size <= 0 || size > 32 )
                return mods;

            resolve_tokens( process );
            if ( !m_tokens.resolved )
                return mods;

            for ( int32_t i = 0; i < size; ++i ) {
                const auto mod_ptr = process.read<uint64_t>( items + m_off.array_first_element + static_cast<uint64_t>( i ) * 8 );
                if ( !mod_ptr )
                    continue;

                const auto mt = process.read<uint64_t>( mod_ptr );
                if ( !mt )
                    continue;

                const auto token_rid = process.read<uint16_t>( mt + 0xA );
                const uint32_t token = 0x02000000 | token_rid;

                if ( token == m_tokens.easy ) {
                    mods |= 2;
                } else if ( token == m_tokens.hidden ) {
                    mods |= 8;
                } else if ( token == m_tokens.hardrock ) {
                    mods |= 16;
                } else if ( token == m_tokens.doubletime ) {
                    mods |= 64;
                } else if ( token == m_tokens.halftime ) {
                    mods |= 256;
                } else if ( token == m_tokens.nightcore ) {
                    mods |= 512;
                } else if ( token == m_tokens.flashlight ) {
                    mods |= 1024;
                }
            }
            return mods;
        }

        void update( memory::c_process& process, osu::game_snapshot_t& snap ) override {
            snap.client = osu::client_kind_t::lazer;
            snap.game_base = m_game_base;
            snap.offset_version = m_off.osu_version;
            snap.player_screen = 0;
            snap.drawable_ruleset = 0;
            snap.cur_mod_state = 0;
            snap.speed_mult = 1.f;

            if ( !m_game_base )
                return;

            snap.cur_mod_state = read_lazer_mods( process );

            const auto beatmap_clock = process.read<uint64_t>( m_game_base + m_off.game_base_beatmap_clock );
            if ( beatmap_clock ) {
                const auto final_source = process.read<uint64_t>( beatmap_clock + m_off.framed_clock_final_source );
                if ( final_source ) {
                    const auto raw_time = process.read<double>( final_source + m_off.framed_clock_current_time );
                    snap.cur_time = static_cast<int32_t>( raw_time );

                    const auto f1 = process.read<uint64_t>( final_source + 8 );
                    const auto f2 = f1 ? process.read<uint64_t>( f1 + 8 ) : 0;
                    const auto f3 = f2 ? process.read<uint64_t>( f2 + 8 ) : 0;
                    if ( f3 ) {
                        double rate = process.read<double>( f3 + 144 );
                        if ( rate > 0.01 && rate < 5.0 ) {
                            snap.speed_mult = static_cast<float>( rate );
                        }
                    }
                }
            }

            if ( snap.speed_mult == 1.f ) {
                if ( ( snap.cur_mod_state & 64 ) != 0 || ( snap.cur_mod_state & 512 ) != 0 ) {
                    snap.speed_mult = 1.5f;
                } else if ( ( snap.cur_mod_state & 256 ) != 0 ) {
                    snap.speed_mult = 0.75f;
                }
            }

            const auto beatmap_bindable = process.read<uint64_t>( m_game_base + m_off.game_base_beatmap );
            if ( beatmap_bindable ) {
                const auto working_beatmap = process.read<uint64_t>( beatmap_bindable + m_off.bindable_value );
                if ( working_beatmap ) {
                    const auto beatmap_info = process.read<uint64_t>( working_beatmap + m_off.working_map_info );
                    const auto set_info = process.read<uint64_t>( working_beatmap + m_off.working_map_set_info );
                    if ( beatmap_info ) {
                        snap.map_id = process.read<int32_t>( beatmap_info + m_off.map_info_online_id );
                        process.read_dotnet_string( beatmap_info + m_off.map_info_hash, snap.beatmap_hash );
                        process.read_dotnet_string( beatmap_info + m_off.map_info_difficulty, snap.beatmap_version );
                    }
                    if ( set_info )
                        snap.set_id = process.read<int32_t>( set_info + m_off.set_info_online_id );
                }
            }

            const auto screen_stack = process.read<uint64_t>( m_game_base + m_off.game_screen_stack );
            const auto stack = screen_stack ? process.read<uint64_t>( screen_stack + m_off.screen_stack_stack ) : 0;
            const auto stack_count = stack ? process.read<int32_t>( stack + 0x10 ) : 0;

            if ( !screen_stack || !stack || stack_count <= 0 )
                return;

            const auto items = process.read<uint64_t>( stack + 0x8 );
            const auto current_screen = process.read<uint64_t>( items + m_off.array_first_element + 0x8 * static_cast<uint64_t>( stack_count - 1 ) );
            snap.player_screen = current_screen;

            const auto base_api = process.read<uint64_t>( m_game_base + m_off.game_base_api );
            auto screen_api = process.read<uint64_t>( current_screen + m_off.submitting_player_api );
            if ( !screen_api || screen_api != base_api )
                screen_api = process.read<uint64_t>( current_screen + m_off.player_api );

            const auto drawable_ruleset = process.read<uint64_t>( current_screen + m_off.player_drawable_ruleset );

            if ( screen_api != 0 && screen_api == base_api && drawable_ruleset != 0 ) {
                snap.cur_state = osu::game_state_t::play;
                snap.drawable_ruleset = drawable_ruleset;
            }
            else {
                snap.cur_state = osu::game_state_t::main_menu;
            }
        }

        osu::client_kind_t kind( ) const override { return osu::client_kind_t::lazer; }

        [[nodiscard]] const offsets::lazer::table_t& offsets( ) const { return m_off; }

    private:
        offsets::lazer::table_t m_off;
        uint64_t m_game_base = 0;
        resolved_mod_tokens_t m_tokens;

        bool verify_game_base( memory::c_process& process, uint64_t addr ) {
            if ( !addr || addr == 0xFFFFFFFFFFFFFFFF )
                return false;
            if ( addr < 0x10000 || addr > 0x7FFFFFFFFFFF )
                return false;

            const auto api = process.read<uint64_t>( addr + m_off.game_base_api );
            if ( !api || api == 0xFFFFFFFFFFFFFFFF || api < 0x10000 || api > 0x7FFFFFFFFFFF )
                return false;

            const auto api_game = process.read<uint64_t>( api + m_off.api_access_game );
            if ( api_game != addr )
                return false;

            const auto screen_stack = process.read<uint64_t>( addr + m_off.game_screen_stack );
            if ( !screen_stack || screen_stack < 0x10000 || screen_stack > 0x7FFFFFFFFFFF )
                return false;

            const auto beatmap_bindable = process.read<uint64_t>( addr + m_off.game_base_beatmap );
            if ( !beatmap_bindable || beatmap_bindable < 0x10000 || beatmap_bindable > 0x7FFFFFFFFFFF )
                return false;

            return true;
        }

        bool try_game_chain( memory::c_process& process, uint64_t candidate, uint64_t offset ) {
            if ( !candidate || candidate == 0xFFFFFFFFFFFFFFFF )
                return false;
            if ( candidate < 0x10000 || candidate > 0x7FFFFFFFFFFF )
                return false;

            {
                const auto api = process.read<uint64_t>( candidate + m_off.ext_link_opener_api );
                if ( api && api != 0xFFFFFFFFFFFFFFFF && api > 0x10000 && api < 0x7FFFFFFFFFFF ) {
                    const auto game = process.read<uint64_t>( api + m_off.api_access_game );
                    if ( verify_game_base( process, game ) ) {
                        m_game_base = game;
                        return true;
                    }
                }
            }

            if ( verify_game_base( process, candidate ) ) {
                m_game_base = candidate;
                return true;
            }

            {
                const auto game = process.read<uint64_t>( candidate + m_off.api_access_game );
                if ( game && game != 0xFFFFFFFFFFFFFFFF && game > 0x10000 && game < 0x7FFFFFFFFFFF ) {
                    if ( verify_game_base( process, game ) ) {
                        m_game_base = game;
                        return true;
                    }
                }
            }

            return false;
        }

        bool scan_range_for_game( memory::c_process& process, uint64_t start, size_t len ) {
            if ( len < 8 )
                return false;

            constexpr size_t chunk_size = 8 * 1024 * 1024;
            std::vector<uint8_t> buffer;

            for ( size_t offset = 0; offset < len; offset += chunk_size ) {
                size_t to_read = (std::min)( len - offset, chunk_size );
                if ( to_read < 8 )
                    break;

                buffer.resize( to_read );
                if ( !process.read_buffer( start + offset, buffer.data( ), to_read ) )
                    continue;

                for ( size_t i = 0; i <= to_read - 8; i += 8 ) {
                    uint64_t candidate;
                    std::memcpy( &candidate, &buffer[ i ], 8 );
                    if ( candidate < 0x10000 || candidate > 0x7FFFFFFFFFFF )
                        continue;
                    if ( candidate & 7 )
                        continue;
                    if ( try_game_chain( process, candidate, offset + i ) )
                        return true;
                }
            }
            return false;
        }

        bool scan_module_for_game( memory::c_process& process, const wchar_t* mod_name ) {
            const auto mod_base = get_module_base( process.pid( ), mod_name );
            if ( !mod_base )
                return false;

            MEMORY_BASIC_INFORMATION mbi{};
            uint64_t scan_addr = mod_base;
            uint64_t module_end = mod_base + 0x1000000;
            while ( scan_addr < module_end ) {
                if ( !VirtualQueryEx( process.handle( ), reinterpret_cast<LPCVOID>( scan_addr ), &mbi, sizeof( mbi ) ) )
                    break;
                if ( mbi.State == MEM_COMMIT &&
                     ( mbi.Protect & ( PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE ) ) ) {
                    if ( scan_range_for_game( process, reinterpret_cast<uint64_t>( mbi.BaseAddress ),
                             (std::min)( mbi.RegionSize, (size_t)0x200000 ) ) )
                        return true;
                }
                scan_addr = reinterpret_cast<uint64_t>( mbi.BaseAddress ) + mbi.RegionSize;
            }
            return false;
        }

        bool scan_private_memory_for_game( memory::c_process& process ) {
            MEMORY_BASIC_INFORMATION mbi{};
            uint64_t addr = 0;
            while ( VirtualQueryEx( process.handle( ), reinterpret_cast<LPCVOID>( addr ), &mbi, sizeof( mbi ) ) ) {
                if ( mbi.State == MEM_COMMIT &&
                     mbi.Type == MEM_PRIVATE &&
                     ( mbi.Protect & ( PAGE_READWRITE | PAGE_EXECUTE_READWRITE ) ) &&
                     mbi.RegionSize >= 0x10000 ) {
                    if ( scan_range_for_game( process, reinterpret_cast<uint64_t>( mbi.BaseAddress ), mbi.RegionSize ) )
                        return true;
                }
                addr = reinterpret_cast<uint64_t>( mbi.BaseAddress ) + mbi.RegionSize;
            }
            return false;
        }
    };

}