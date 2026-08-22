#pragma once

#include <core/beatmap/i_beatmap_provider.hxx>
#include <core/beatmap/stable_parser.hxx>
#include <core/beatmap/lazer_index.hxx>
#include <core/beatmap/lazer_ruleset_reader.hxx>
#include <impl/defs/offsets_lazer.hxx>
#include <impl/util/debug_log.hxx>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <mutex>

namespace beatmap {

    class c_lazer_memory : public i_beatmap_provider {
    public:
        void set_offsets( const offsets::lazer::table_t* offsets ) { m_offsets = offsets; }

        void set_data_dir( std::wstring path ) {
            if ( path != m_data_dir ) {
                m_data_dir = std::move( path );
                m_index.invalidate( );
            }
        }

        [[nodiscard]] const std::wstring& data_dir( ) const { return m_data_dir; }
        [[nodiscard]] const std::wstring& last_beatmap_path( ) const { return m_last_beatmap_path; }
        [[nodiscard]] bool index_ready( ) const { return m_index_ready.load( ); }

        void warm_index( ) {
            if ( !m_data_dir.empty( ) ) {
                m_index.ensure_built( m_data_dir );
                m_index_ready = true;
            }
        }

        void invalidate_cache( ) {
            m_last_sig.clear( );
            m_cached = {};
            m_last_fail_sig.clear( );
        }

        void refresh_index( ) {
            m_index.invalidate( );
            if ( !m_data_dir.empty( ) )
                m_index.ensure_built( m_data_dir );
        }

        bool try_resolve_gameplay(
            memory::c_process& process,
            const osu::game_snapshot_t& game,
            uint64_t& drawable,
            const char*& failure_stage ) {

            if ( !m_offsets ) {
                failure_stage = "lazer offset table unavailable";
                drawable = 0;
                return false;
            }

            std::lock_guard lock( m_ruleset_mutex );
            return m_ruleset_reader.try_resolve_gameplay(
                process, game, *m_offsets, drawable, failure_stage );
        }

        bool try_load( memory::c_process& process, const osu::game_snapshot_t& game, osu::beatmap_data_t& out ) override {
            out = {};
            out.map_id = game.map_id;
            out.set_id = game.set_id;

            const bool has_map =
                game.map_id > 0 || !game.beatmap_hash.empty( ) ||
                ( game.set_id > 0 && !game.beatmap_version.empty( ) );

            const std::string sig =
                std::to_string( game.map_id ) + "|" + std::to_string( game.set_id ) + "|" +
                game.beatmap_hash + "|" + game.beatmap_version + "|" +
                std::to_string( game.cur_mod_state );

            const bool in_play = game.cur_state == osu::game_state_t::play;
            const bool offsets_present = m_offsets != nullptr;
            const bool hitobject_offsets_ready = offsets_present && m_offsets->has_hitobject_offsets( );
            const bool diagnostics = should_log_diagnostics( sig );
            std::string live_failure;
            std::string file_failure;

            if ( diagnostics ) {
                dbg::log(
                    "lazer beatmap provider: state=%d in_play=%d drawable=0x%llX offsets=%d hitobject_offsets=%d map_id=%d set_id=%d hash='%s' version='%s' has_map=%d",
                    static_cast<int>( game.cur_state ), in_play ? 1 : 0,
                    static_cast<unsigned long long>( game.drawable_ruleset ),
                    offsets_present ? 1 : 0, hitobject_offsets_ready ? 1 : 0,
                    game.map_id, game.set_id, game.beatmap_hash.c_str( ),
                    game.beatmap_version.c_str( ), has_map ? 1 : 0 );
            }

            // During lazer gameplay we already have DrawableRuleset.  Do not make the
            // in-memory hit-object reader depend on BeatmapInfo/hash/id offsets being valid.
            // Those metadata offsets can move independently and previously prevented the
            // ruleset reader from being attempted at all.
            if ( in_play && hitobject_offsets_ready && game.drawable_ruleset != 0 ) {
                if ( diagnostics ) {
                    dbg::log( "lazer live reader attempted: drawable=0x%llX",
                        static_cast<unsigned long long>( game.drawable_ruleset ) );
                }
                std::lock_guard lock( m_ruleset_mutex );
                if ( m_ruleset_reader.try_load( process, game, *m_offsets, out ) ) {
                    if ( diagnostics )
                        dbg::log( "lazer live reader succeeded: objects=%zu", out.objects.size( ) );
                    m_last_sig = sig;
                    m_cached = out;
                    m_last_fail_sig.clear( );
                    m_last_beatmap_path.clear( );
                    return true;
                }

                live_failure = out.error.empty( ) ? "ruleset reader returned false without an error" : out.error;
                if ( diagnostics )
                    dbg::log( "lazer live reader failed: reason='%s'", live_failure.c_str( ) );
            }
            else {
                if ( !in_play )
                    live_failure = "game state is not PLAY";
                else if ( game.drawable_ruleset == 0 )
                    live_failure = "DrawableRuleset pointer is null";
                else if ( !offsets_present )
                    live_failure = "lazer offset table is null";
                else
                    live_failure = "hit-object offset table is incomplete";

                if ( diagnostics )
                    dbg::log( "lazer live reader skipped: reason='%s'", live_failure.c_str( ) );
            }

            // File/index lookup still requires identifying metadata.
            if ( !has_map ) {
                out.error = live_failure + "; file fallback unavailable because map metadata is empty";
                if ( diagnostics )
                    dbg::log( "lazer file fallback skipped: reason='map metadata unavailable' data_dir='%ls' index_ready=%d",
                        m_data_dir.c_str( ), m_index_ready.load( ) ? 1 : 0 );
                return false;
            }

            if ( !in_play && sig == m_last_sig && !m_cached.objects.empty( ) ) {
                out = m_cached;
                return true;
            }

            if ( !m_data_dir.empty( ) ) {
                m_index.ensure_built( m_data_dir );

                std::wstring path;
                std::wstring hash_path;
                std::wstring map_id_path;
                std::wstring set_version_path;

                if ( !game.beatmap_hash.empty( ) ) {
                    hash_path = find_hash_path( m_data_dir, game.beatmap_hash );
                    if ( hash_path.empty( ) )
                        hash_path = m_index.find_by_hash( game.beatmap_hash );
                    path = hash_path;
                }

                if ( path.empty( ) && game.map_id > 0 ) {
                    map_id_path = m_index.find_by_map_id( game.map_id );
                    path = map_id_path;
                }

                if ( path.empty( ) && game.set_id > 0 && !game.beatmap_version.empty( ) ) {
                    set_version_path = m_index.find_by_set_and_version( game.set_id, game.beatmap_version );
                    path = set_version_path;
                }

                if ( diagnostics ) {
                    dbg::log(
                        "lazer file fallback lookup: data_dir='%ls' index_ready=%d hash_result='%ls' map_id_result='%ls' set_version_result='%ls' candidate='%ls' exists=%d",
                        m_data_dir.c_str( ), m_index_ready.load( ) ? 1 : 0,
                        hash_path.c_str( ), map_id_path.c_str( ), set_version_path.c_str( ),
                        path.c_str( ), !path.empty( ) && std::filesystem::is_regular_file( path ) ? 1 : 0 );
                }

                if ( !path.empty( ) ) {
                    m_last_fail_sig.clear( );

                    if ( m_parser.load_from_path( path, game, out ) ) {
                        if ( diagnostics )
                            dbg::log( "lazer file fallback parser succeeded: path='%ls' objects=%zu",
                                path.c_str( ), out.objects.size( ) );
                        m_last_beatmap_path = path;
                        m_last_sig = sig;
                        m_cached = out;
                        return true;
                    }

                    if ( diagnostics )
                        dbg::log( "lazer file fallback parser failed: path='%ls' reason='%s'",
                            path.c_str( ), out.error.c_str( ) );
                    file_failure = out.error.empty( )
                        ? "file fallback parser returned false without an error"
                        : std::string( "file fallback parser failed: " ) + out.error;
                }
                else if ( sig != m_last_fail_sig ) {
                    if ( m_index_ready )
                        m_last_fail_sig = sig;
                    refresh_index( );
                }

                if ( path.empty( ) )
                    file_failure = "file fallback found no path for the available map metadata";
            }
            else {
                file_failure = "file fallback data directory is empty";
                if ( diagnostics ) {
                    dbg::log( "lazer file fallback skipped: reason='data directory is empty' index_ready=%d",
                        m_index_ready.load( ) ? 1 : 0 );
                }
            }

            if ( file_failure.empty( ) )
                file_failure = "file fallback did not produce a usable beatmap";
            out.error = live_failure.empty( ) ? file_failure : live_failure + "; " + file_failure;
            return false;
        }

    private:
        const offsets::lazer::table_t* m_offsets = nullptr;
        std::wstring m_data_dir;
        std::wstring m_last_beatmap_path;
        std::string m_last_sig;
        c_stable_parser m_parser;
        c_lazer_index m_index;
        c_lazer_ruleset_reader m_ruleset_reader;
        std::mutex m_ruleset_mutex;
        osu::beatmap_data_t m_cached;
        std::string m_last_fail_sig;
        std::atomic<bool> m_index_ready{ false };
        uint64_t m_last_diag_ms = 0;
        std::string m_last_diag_sig;

        bool should_log_diagnostics( const std::string& sig ) {
            const uint64_t now = GetTickCount64( );
            if ( sig == m_last_diag_sig && now - m_last_diag_ms < 3000 )
                return false;
            m_last_diag_sig = sig;
            m_last_diag_ms = now;
            return true;
        }


        static std::wstring find_hash_path( const std::wstring& data_dir, const std::string& hash_in ) {
            if ( data_dir.empty( ) || hash_in.empty( ) )
                return L"";

            std::string hash = hash_in;
            std::transform( hash.begin( ), hash.end( ), hash.begin( ),
                []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );

            const auto hash_wide = std::wstring( hash.begin( ), hash.end( ) );
            const auto files_root = std::filesystem::path( data_dir ) / L"files";

            if ( hash.size( ) >= 1 ) {
                auto candidate = files_root / std::wstring( 1, hash[ 0 ] ) / hash_wide;
                if ( std::filesystem::is_regular_file( candidate ) )
                    return candidate.wstring( );
            }

            if ( hash.size( ) >= 2 ) {
                auto candidate = files_root / std::wstring( hash.begin( ), hash.begin( ) + 2 ) / hash_wide;
                if ( std::filesystem::is_regular_file( candidate ) )
                    return candidate.wstring( );
            }

            return L"";
        }
    };

}
