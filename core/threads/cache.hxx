#pragma once

#include <core/game/client_factory.hxx>
#include <core/beatmap/stable_parser.hxx>
#include <core/beatmap/lazer_memory.hxx>
#include <core/beatmap/songs_resolver.hxx>
#include <impl/memory/ntdll.hxx>
#include <impl/memory/syscall.hxx>
#include <impl/memory/process.hxx>
#include <impl/defs/offsets_lazer.hxx>
#include <impl/util/playfield.hxx>
#include <impl/util/debug_log.hxx>
#include <impl/memory/input.hxx>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

namespace threads {

    class c_cache {
    public:
        c_cache( ) = default;

        bool init( ) {
            const bool ntdll_ok = m_ntdll.load( );
            bool syscall_ok = false;
            if ( ntdll_ok )
                syscall_ok = m_syscall.setup( m_ntdll );

            if ( syscall_ok )
                m_process.set_syscall( m_syscall );
            else
                m_process.enable_fallback( );

            m_lazer_memory.set_offsets( &m_lazer_offsets );
            input::init( );
            return true;
        }

        void start( ) {
            if ( m_running.exchange( true ) )
                return;

            m_process_thread = std::thread( [ this ] { process_loop( ); } );
            m_game_thread = std::thread( [ this ] {
                ::SetThreadPriority( ::GetCurrentThread( ), THREAD_PRIORITY_TIME_CRITICAL );
                game_loop( );
            } );
            m_beatmap_thread = std::thread( [ this ] { beatmap_loop( ); } );
        }

        void stop( ) {
            m_running = false;
            if ( m_process_thread.joinable( ) )
                m_process_thread.join( );
            if ( m_game_thread.joinable( ) )
                m_game_thread.join( );
            if ( m_beatmap_thread.joinable( ) )
                m_beatmap_thread.join( );
        }

        osu::full_snapshot_t get_snapshot( ) const {
            std::shared_lock lock( m_mutex );
            return m_snapshot;
        }

        [[nodiscard]] HANDLE process_handle( ) const {
            std::shared_lock lock( m_mutex );
            return m_process.handle( );
        }

        beatmap::c_stable_parser& stable_parser( ) { return m_stable_parser; }

        void invalidate_beatmap_cache( ) {
            std::unique_lock lock( m_mutex );
            m_snapshot.beatmap = {};
            m_last_map_sig.clear( );
        }

        using module_tick_fn = std::function<void( const osu::game_snapshot_t&, const osu::beatmap_data_t& )>;

        void set_module_tick( module_tick_fn fn ) {
            std::lock_guard lock( m_module_tick_mutex );
            m_module_tick = std::move( fn );
        }

    private:
        memory::c_clean_ntdll m_ntdll;
        memory::c_syscall m_syscall;
        memory::c_process m_process;
        offsets::lazer::table_t m_lazer_offsets;
        game::osu_client_ptr m_client;

        beatmap::c_stable_parser m_stable_parser;
        beatmap::c_lazer_memory m_lazer_memory;

        mutable std::shared_mutex m_mutex;
        osu::full_snapshot_t m_snapshot;

        std::atomic<bool> m_running{ false };
        std::thread m_process_thread;
        std::thread m_game_thread;
        std::thread m_beatmap_thread;

        std::string m_last_map_sig;
        bool m_was_in_play = false;
        int32_t m_last_beatmap_game_time = -1;
        uint64_t m_leave_play_wall_ms = 0;
        uint64_t m_last_lazer_diag_ms = 0;
        std::string m_last_lazer_diag_stage;
        std::atomic<bool> m_lazer_gameplay_ready{ false };


        module_tick_fn m_module_tick;
        std::mutex m_module_tick_mutex;

        void try_attach_osu( ) {
            const auto pid = memory::c_process::find_pid_by_name( L"osu!.exe" );
            if ( !pid ) {
                return;
            }

            if ( !m_process.attach( pid ) ) {
                return;
            }

            m_client = game::try_attach( m_process, m_lazer_offsets );
            if ( !m_client ) {
                m_process.detach( );
                return;
            }

            const auto osu_hwnd = playfield::find_osu_window( static_cast<DWORD>( pid ) );
            input::set_target_window( osu_hwnd );
            input::invalidate_virtual_desktop( );

            if ( m_client->kind( ) == osu::client_kind_t::stable ) {
                const auto songs = beatmap::songs::resolve( m_process );
                m_stable_parser.set_songs_path( songs );
                m_last_map_sig.clear( );

                {
                    std::unique_lock lock( m_mutex );
                    char narrow[ 512 ]{};
                    WideCharToMultiByte( CP_UTF8, 0, songs.c_str( ), -1, narrow, 512, nullptr, nullptr );
                    m_snapshot.game.songs_path = narrow;
                }
            }
            else if ( m_client->kind( ) == osu::client_kind_t::lazer ) {
                dbg::log( "lazer process attached: pid=%d layout_version=%s",
                    pid, m_lazer_offsets.osu_version.c_str( ) );
                const auto data_dir = beatmap::songs::resolve_lazer_data_dir( m_process );
                m_lazer_memory.set_data_dir( data_dir );
                m_last_map_sig.clear( );

                {
                    std::unique_lock lock( m_mutex );
                    char narrow[ 512 ]{};
                    WideCharToMultiByte( CP_UTF8, 0, data_dir.c_str( ), -1, narrow, 512, nullptr, nullptr );
                    m_snapshot.game.songs_path = narrow;
                }

                std::thread( [ this ] { m_lazer_memory.warm_index( ); } ).detach( );
            }
        }

        bool is_process_alive( ) const {
            if ( !m_process.valid( ) )
                return false;
            DWORD exit_code = 0;
            if ( !GetExitCodeProcess( m_process.handle( ), &exit_code ) )
                return false;
            return exit_code == STILL_ACTIVE;
        }

        void force_detach( ) {
            m_client.reset( );
            m_process.detach( );
            input::set_target_window( nullptr );
            {
                std::unique_lock lock( m_mutex );
                m_snapshot.game = {};
                m_snapshot.beatmap = {};
            }
            m_last_map_sig.clear( );
            m_lazer_gameplay_ready = false;
        }

        void log_lazer_resolution_failure(
            const osu::game_snapshot_t& snap,
            const char* stage ) {

            const uint64_t now = GetTickCount64( );
            const std::string current_stage = stage ? stage : "unknown";
            if ( current_stage == m_last_lazer_diag_stage &&
                 now - m_last_lazer_diag_ms < 5000 )
                return;

            m_last_lazer_diag_ms = now;
            m_last_lazer_diag_stage = current_stage;
            const bool has_map = snap.map_id > 0 || !snap.beatmap_hash.empty( ) ||
                ( snap.set_id > 0 && !snap.beatmap_version.empty( ) );
            dbg::log(
                "lazer beatmap provider not attempted: stage=%s state=%d in_play=0 root=0x%llX player=0x%llX drawable=0x%llX api_match=%d offsets=1 hitobject_offsets=%d map_id=%d set_id=%d hash='%s' version='%s' has_map=%d time=%d",
                current_stage.c_str( ),
                static_cast<int>( snap.cur_state ),
                static_cast<unsigned long long>( snap.game_base ),
                static_cast<unsigned long long>( snap.player_screen ),
                static_cast<unsigned long long>( snap.drawable_ruleset ),
                snap.lazer_player_api_valid ? 1 : 0,
                m_lazer_offsets.has_hitobject_offsets( ) ? 1 : 0,
                snap.map_id, snap.set_id, snap.beatmap_hash.c_str( ),
                snap.beatmap_version.c_str( ), has_map ? 1 : 0, snap.cur_time );
        }

        void process_loop( ) {
            using namespace std::chrono_literals;
            while ( m_running ) {
                if ( !m_process.valid( ) || !m_client ) {
                    if ( m_process.valid( ) )
                        m_process.detach( );
                    try_attach_osu( );
                }
                else if ( !is_process_alive( ) ) {
                    force_detach( );
                }
                else if ( !input::target_window( ) || !IsWindow( input::target_window( ) ) ) {
                    const auto hwnd = playfield::find_osu_window( static_cast<DWORD>( m_process.pid( ) ) );
                    if ( hwnd ) {
                        input::set_target_window( hwnd );
                        input::invalidate_virtual_desktop( );
                    }
                }

                {
                    std::unique_lock lock( m_mutex );
                    const bool was_attached = m_snapshot.game.attached;
                    const bool is_attached = m_process.valid( ) && m_client != nullptr;
                    m_snapshot.game.attached = is_attached;
                    m_snapshot.game.pid = m_process.pid( );
                    m_snapshot.game.offset_version = m_lazer_offsets.osu_version;
                    if ( is_attached && m_client ) {
                        m_snapshot.game.client = m_client->kind( );
                    } else {
                        m_snapshot.game.client = osu::client_kind_t::none;
                    }

                }

                std::this_thread::sleep_for( 5ms );
            }
        }

        void game_loop( ) {
            using namespace std::chrono_literals;
            while ( m_running ) {
                bool in_play = false;
                if ( m_client && m_process.valid( ) ) {
                    osu::game_snapshot_t snap;
                    snap.attached = true;
                    snap.pid = m_process.pid( );
                    m_client->update( m_process, snap );

                    bool used_validated_fallback = false;
                    if ( snap.client == osu::client_kind_t::lazer &&
                         snap.cur_state != osu::game_state_t::play ) {
                        const char* failure_stage = nullptr;

                        if ( snap.game_base == 0 ) {
                            failure_stage = "game root unavailable";
                        }
                        else if ( snap.player_screen == 0 ) {
                            failure_stage = "screen stack/player unavailable";
                        }
                        else if ( !snap.lazer_player_api_valid ) {
                            failure_stage = "Player API field/equality unresolved";
                        }
                        else {
                            uint64_t resolved_drawable = 0;
                            if ( m_lazer_memory.try_resolve_gameplay(
                                    m_process, snap, resolved_drawable, failure_stage ) ) {
                                snap.drawable_ruleset = resolved_drawable;
                                snap.cur_state = osu::game_state_t::play;
                                used_validated_fallback = true;
                            }
                        }

                        if ( snap.cur_state != osu::game_state_t::play ) {
                            m_lazer_gameplay_ready = false;
                            log_lazer_resolution_failure( snap, failure_stage );
                        }
                    }

                    in_play = snap.cur_state == osu::game_state_t::play;

                    if ( snap.client == osu::client_kind_t::lazer && in_play &&
                         !m_lazer_gameplay_ready.exchange( true ) ) {
                        dbg::log(
                            "lazer gameplay resolved: source=%s root=0x%llX player=0x%llX drawable=0x%llX map_id=%d set_id=%d time=%d",
                            used_validated_fallback ? "validated-nearby-field fallback" : "exact layout",
                            static_cast<unsigned long long>( snap.game_base ),
                            static_cast<unsigned long long>( snap.player_screen ),
                            static_cast<unsigned long long>( snap.drawable_ruleset ),
                            snap.map_id, snap.set_id, snap.cur_time );
                        m_lazer_gameplay_ready = true;
                        m_last_lazer_diag_stage.clear( );
                    }

                    {
                        std::unique_lock lock( m_mutex );
                        if ( snap.songs_path.empty( ) )
                            snap.songs_path = m_snapshot.game.songs_path;
                        m_snapshot.game = snap;
                    }

                    module_tick_fn tick;
                    {
                        std::lock_guard lock( m_module_tick_mutex );
                        tick = m_module_tick;
                    }
                    if ( tick ) {
                        std::shared_lock lock( m_mutex );
                        tick( m_snapshot.game, m_snapshot.beatmap );
                    }
                }

                if ( in_play )
                    std::this_thread::sleep_for( 0ms );
                else
                    std::this_thread::sleep_for( 10ms );
            }
        }

        void beatmap_loop( ) {
            using namespace std::chrono_literals;
            while ( m_running ) {
                osu::game_snapshot_t game_snap;
                {
                    std::shared_lock lock( m_mutex );
                    game_snap = m_snapshot.game;
                }

                const bool in_play = game_snap.cur_state == osu::game_state_t::play;
                const bool in_select_play = game_snap.cur_state == osu::game_state_t::select_play;
                const bool load_state = in_play || in_select_play;

                if ( m_was_in_play && !in_play ) {
                    if ( m_leave_play_wall_ms == 0 ) {
                        LARGE_INTEGER qpc, freq;
                        QueryPerformanceCounter( &qpc );
                        QueryPerformanceFrequency( &freq );
                        m_leave_play_wall_ms = static_cast<uint64_t>(
                            static_cast<double>( qpc.QuadPart ) / static_cast<double>( freq.QuadPart ) * 1000.0 );
                    } else {
                        LARGE_INTEGER qpc, freq;
                        QueryPerformanceCounter( &qpc );
                        QueryPerformanceFrequency( &freq );
                        uint64_t now_ms = static_cast<uint64_t>(
                            static_cast<double>( qpc.QuadPart ) / static_cast<double>( freq.QuadPart ) * 1000.0 );
                        if ( now_ms - m_leave_play_wall_ms >= 500 ) {
                            std::unique_lock lock( m_mutex );
                            m_snapshot.beatmap = {};
                            m_last_map_sig.clear( );
                            m_last_beatmap_game_time = -1;
                            m_was_in_play = false;
                            m_leave_play_wall_ms = 0;
                        }
                    }
                } else if ( in_play ) {
                    m_was_in_play = true;
                    m_leave_play_wall_ms = 0;
                }

                if ( in_play && game_snap.cur_time < m_last_beatmap_game_time - 200 )
                    m_last_map_sig.clear( );
                if ( in_play )
                    m_last_beatmap_game_time = game_snap.cur_time;

                bool beatmap_loaded = false;
                {
                    std::shared_lock lock( m_mutex );
                    beatmap_loaded = m_snapshot.beatmap.loaded;
                }

                const bool has_map =
                    game_snap.map_id != 0 ||
                    !game_snap.beatmap_hash.empty( ) ||
                    ( game_snap.set_id > 0 && !game_snap.beatmap_version.empty( ) ) ||
                    ( !game_snap.map_folder.empty( ) && !game_snap.map_file.empty( ) );
                const std::string map_sig =
                    std::to_string( game_snap.map_id ) + "|" + std::to_string( game_snap.set_id ) + "|" +
                    game_snap.map_folder + "|" + game_snap.map_file + "|" + game_snap.beatmap_hash + "|" +
                    game_snap.beatmap_version + "|" + std::to_string( game_snap.cur_mod_state );
                const bool map_changed = has_map && map_sig != m_last_map_sig;
                const bool lazer_direct_memory =
                    in_play && game_snap.client == osu::client_kind_t::lazer &&
                    game_snap.drawable_ruleset != 0;
                const bool retry_reload =
                    in_play && !beatmap_loaded && ( has_map || lazer_direct_memory );

                // Lazer's DrawableRuleset contains the converted beatmap directly.  Allow
                // that memory path to run even if BeatmapInfo/hash/id offsets are stale.
                if ( game_snap.attached && load_state && ( has_map || lazer_direct_memory ) &&
                     ( map_changed || retry_reload ) ) {

                    if ( map_changed ) {
                        m_lazer_memory.invalidate_cache( );
                        {
                            std::unique_lock lock( m_mutex );
                            m_snapshot.beatmap = {};
                        }
                    }

                    osu::beatmap_data_t beatmap;
                    bool ok = false;

                    if ( game_snap.client == osu::client_kind_t::stable )
                        ok = m_stable_parser.try_load( m_process, game_snap, beatmap );
                    else if ( game_snap.client == osu::client_kind_t::lazer )
                        ok = m_lazer_memory.try_load( m_process, game_snap, beatmap );



                    {
                        std::unique_lock lock( m_mutex );
                        if ( ok )
                            m_snapshot.beatmap = beatmap;
                        if ( ok )
                            m_last_map_sig = map_sig;
                    }
                }

                if ( in_play && !beatmap_loaded )
                    std::this_thread::sleep_for( 2ms );
                else
                    std::this_thread::sleep_for( 10ms );
            }
        }
    };

}
