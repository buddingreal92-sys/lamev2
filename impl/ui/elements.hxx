#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <impl/ui/theme.hxx>
#include <impl/ui/animations.hxx>
#include <windows.h>
#include <string>
#include <unordered_map>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace ui {

    inline float snap_text_y( float y ) {
        return std::floor( y + 0.5f );
    }

    // hover state of the most recently drawn custom widget (used by tip())
    inline bool g_item_hovered = false;

    // ==================================================================
    // panels & sections
    // ==================================================================

    inline void draw_panel( ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, const char* title = nullptr ) {
        const float r = theme::panel_rounding;
        dl->AddRectFilled( ImVec2( p0.x + 2.f, p0.y + 3.f ), ImVec2( p1.x + 2.f, p1.y + 3.f ),
            theme::fade( IM_COL32( 0, 0, 0, 60 ) ), r );
        dl->AddRectFilled( p0, p1, theme::fade( theme::panel_bg() ), r );
        dl->AddRect( p0, p1, theme::fade( theme::border() ), r, 0, 1.0f );
        dl->AddLine( ImVec2( p0.x + r, p0.y + 1.f ), ImVec2( p1.x - r, p0.y + 1.f ),
            theme::fade( IM_COL32( 255, 255, 255, 10 ) ), 1.0f );

        if ( title ) {
            // accent tick + title + hairline under the header strip
            dl->AddRectFilled( ImVec2( p0.x + 12.f, p0.y + 9.f ), ImVec2( p0.x + 15.f, p0.y + 19.f ),
                theme::fade( theme::accent() ), 1.f );
            dl->AddText( theme::font_tiny, theme::font_tiny->LegacySize,
                ImVec2( p0.x + 21.f, snap_text_y( p0.y + 8.f ) ), theme::fade( theme::text_accent() ), title );
            dl->AddLine( ImVec2( p0.x + 1.f, p0.y + 27.f ), ImVec2( p1.x - 1.f, p0.y + 27.f ),
                theme::fade( IM_COL32( 30, 30, 36, 255 ) ), 1.0f );
        }
    }

    // small uppercase group label with a short accent dash, advances y
    inline void section_label( ImDrawList* dl, float x, float& y, const char* text ) {
        const ImVec2 ts = theme::font_tiny->CalcTextSizeA( theme::font_tiny->LegacySize, FLT_MAX, 0.f, text );
        dl->AddText( theme::font_tiny, theme::font_tiny->LegacySize,
            ImVec2( x, snap_text_y( y ) ), theme::fade( theme::text_dim() ), text );
        dl->AddLine( ImVec2( x + ts.x + 8.f, y + ts.y * 0.5f + 0.5f ),
            ImVec2( x + ts.x + 26.f, y + ts.y * 0.5f + 0.5f ),
            theme::fade( theme::accent_alpha( 0.35f ) ), 1.f );
        y += ts.y + 6.f;
    }

    // tiny module state dot
    inline void status_dot( ImDrawList* dl, const ImVec2& c, bool on, float r = 2.5f ) {
        if ( on ) {
            dl->AddCircleFilled( c, r + 2.f, theme::fade( theme::accent_alpha( 0.16f * breathe( 2.2f, 0.6f, 1.f ) ) ), 10 );
            dl->AddCircleFilled( c, r, theme::fade( theme::accent_hot() ), 10 );
        }
        else {
            dl->AddCircleFilled( c, r, theme::fade( IM_COL32( 58, 57, 66, 255 ) ), 10 );
        }
    }

    // aligned label/value diagnostic row (mono value, right aligned)
    inline void kv_row( ImDrawList* dl, float x, float& y, float w, const char* label, const char* value,
                        ImU32 value_col = 0 ) {
        const float fs = theme::font_mono->LegacySize;
        dl->AddText( theme::font_small, theme::font_small->LegacySize,
            ImVec2( x, snap_text_y( y ) ), theme::fade( theme::text_dim() ), label );
        const ImVec2 vs = theme::font_mono->CalcTextSizeA( fs, FLT_MAX, 0.f, value );
        dl->AddText( theme::font_mono, fs, ImVec2( x + w - vs.x, snap_text_y( y ) ),
            theme::fade( value_col ? value_col : theme::text() ), value );
        y += std::max( theme::font_small->LegacySize, fs ) + 3.f;
    }

    // collapsible group header; returns true while open
    inline bool collapsible( ImDrawList* dl, float x, float& y, float w, const char* label, bool default_open = false ) {
        static std::unordered_map<ImGuiID, bool> open_state;
        const ImGuiID id = ImGui::GetID( label );
        auto [ it, ins ] = open_state.try_emplace( id, default_open );

        const float h = 20.f;
        const ImVec2 sp0 = ImGui::GetWindowPos( );
        ImGui::SetCursorScreenPos( ImVec2( x, y ) );
        ImGui::InvisibleButton( label, ImVec2( w, h ) );
        const bool hov = ImGui::IsItemHovered( );
        if ( ImGui::IsItemClicked( ImGuiMouseButton_Left ) ) it->second = !it->second;

        const float t = anim( id, it->second ? 1.f : 0.f, 16.f, it->second ? 1.f : 0.f );

        // rotating chevron
        const ImVec2 c( x + 6.f, y + h * 0.5f );
        const float a = t * 1.5708f; // 0 = right, 90deg = down
        const float ca = std::cos( a ), sa = std::sin( a );
        auto rot = [&]( float px, float py ) {
            return ImVec2( c.x + px * ca - py * sa, c.y + px * sa + py * ca );
        };
        const ImU32 ch_col = theme::fade( hov ? theme::text_bright() : theme::text_dim() );
        dl->AddTriangleFilled( rot( -1.5f, -3.5f ), rot( 3.5f, 0.f ), rot( -1.5f, 3.5f ), ch_col );

        dl->AddText( theme::font_tiny, theme::font_tiny->LegacySize,
            ImVec2( x + 16.f, snap_text_y( y + ( h - theme::font_tiny->LegacySize ) * 0.5f ) ),
            theme::fade( hov ? theme::text_bright() : theme::text_dim() ), label );
        (void)sp0;

        y += h + 2.f;
        return t > 0.5f;
    }

    // ==================================================================
    // themed tooltip (one per frame, ~0.45s hover delay)
    // ==================================================================

    namespace _tip {
        inline const char* candidate = nullptr;   // set this frame
        inline const char* current = nullptr;     // carried across frames
        inline float timer = 0.f;
    }

    // call directly after a widget to attach a tooltip to it
    inline void tip( const char* text ) {
        if ( g_item_hovered ) _tip::candidate = text;
    }

    // call once at the end of the menu window
    inline void render_tooltip( ) {
        if ( _tip::candidate != _tip::current ) {
            _tip::current = _tip::candidate;
            _tip::timer = 0.f;
        }
        else if ( _tip::current ) {
            _tip::timer += g_delta_time;
        }
        const char* text = _tip::current;
        _tip::candidate = nullptr;
        if ( !text || _tip::timer < 0.45f ) return;

        const float a = theme::ease_out_cubic( std::clamp( ( _tip::timer - 0.45f ) / 0.10f, 0.f, 1.f ) );
        ImDrawList* fdl = ImGui::GetForegroundDrawList( );
        const ImVec2 mouse = ImGui::GetIO( ).MousePos;
        const float fs = theme::font_small->LegacySize;
        const float wrap = 240.f;
        const ImVec2 ts = theme::font_small->CalcTextSizeA( fs, FLT_MAX, wrap, text );
        ImVec2 p0( mouse.x + 14.f, mouse.y + 18.f );
        const ImVec2 disp = ImGui::GetIO( ).DisplaySize;
        if ( p0.x + ts.x + 18.f > disp.x ) p0.x = disp.x - ts.x - 18.f;
        if ( p0.y + ts.y + 14.f > disp.y ) p0.y = mouse.y - ts.y - 20.f;
        const ImVec2 p1( p0.x + ts.x + 16.f, p0.y + ts.y + 12.f );

        const float prev = theme::content_mul;
        theme::content_mul = a;
        fdl->AddRectFilled( ImVec2( p0.x + 2, p0.y + 2 ), ImVec2( p1.x + 2, p1.y + 2 ), theme::fade( IM_COL32( 0, 0, 0, 90 ) ), 3.f );
        fdl->AddRectFilled( p0, p1, theme::fade( IM_COL32( 20, 19, 26, 250 ) ), 3.f );
        fdl->AddRect( p0, p1, theme::fade( theme::accent_alpha( 0.35f ) ), 3.f, 0, 1.f );
        fdl->AddText( theme::font_small, fs, ImVec2( p0.x + 8.f, p0.y + 6.f ), theme::fade( theme::text() ), text, nullptr, wrap );
        theme::content_mul = prev;
    }

    // ==================================================================
    // legacy named-bind capture (kept for widgets that request it)
    // ==================================================================

    struct s_bind_entry {
        std::string key;
        bool waiting = false;
        int start_frame = -1;
    };

    inline std::unordered_map<std::string, s_bind_entry> g_binds;

    inline void init_bind( const char* id, const char* default_key ) {
        if ( g_binds.find( id ) == g_binds.end( ) )
            g_binds[ id ] = { default_key ? default_key : "", false, -1 };
    }

    inline bool is_any_bind_waiting( ) {
        for ( auto& [ id, e ] : g_binds )
            if ( e.waiting ) return true;
        return false;
    }

    inline bool is_bind_waiting( const char* id ) {
        const auto it = g_binds.find( id );
        return it != g_binds.end( ) && it->second.waiting;
    }

    inline void start_bind_waiting( const char* id ) {
        for ( auto& [ bid, e ] : g_binds ) e.waiting = false;
        const auto it = g_binds.find( id );
        if ( it != g_binds.end( ) ) {
            it->second.waiting = true;
            it->second.start_frame = ImGui::GetFrameCount( );
        }
    }

    inline const char* get_bind_display( const char* id ) {
        static char buf[ 48 ];
        const auto it = g_binds.find( id );
        if ( it == g_binds.end( ) ) return "";
        if ( it->second.waiting ) return "press key";
        if ( it->second.key.empty( ) ) return "";
        snprintf( buf, sizeof( buf ), "[%s]", it->second.key.c_str( ) );
        return buf;
    }

    inline void update_bind_capture( ) {
        for ( auto& [ id, e ] : g_binds ) {
            if ( !e.waiting ) continue;
            if ( ImGui::GetFrameCount( ) <= e.start_frame + 1 ) continue;

            ImGuiIO& io = ImGui::GetIO( );
            if ( ImGui::IsKeyPressed( ImGuiKey_Escape ) ) { e.waiting = false; return; }

            if ( io.MouseClicked[ 0 ] ) { e.key = "M1"; e.waiting = false; return; }
            if ( io.MouseClicked[ 1 ] ) { e.key = "M2"; e.waiting = false; return; }
            if ( io.MouseClicked[ 2 ] ) { e.key = "M3"; e.waiting = false; return; }
            if ( io.MouseClicked[ 3 ] ) { e.key = "M4"; e.waiting = false; return; }
            if ( io.MouseClicked[ 4 ] ) { e.key = "M5"; e.waiting = false; return; }

            for ( int k = static_cast<int>( ImGuiKey_NamedKey_BEGIN ); k < static_cast<int>( ImGuiKey_NamedKey_END ); ++k ) {
                const auto key = static_cast<ImGuiKey>( k );
                if ( key == ImGuiKey_Escape || !ImGui::IsKeyPressed( key ) ) continue;

                const char* raw = ImGui::GetKeyName( key );
                std::string name = raw;
                if ( name == "LeftAlt" || name == "RightAlt" ) name = "ALT";
                else if ( name == "LeftCtrl" || name == "RightCtrl" ) name = "CTRL";
                else if ( name == "LeftShift" || name == "RightShift" ) name = "SHIFT";
                else if ( name == "LeftSuper" || name == "RightSuper" ) name = "WIN";
                else if ( name == "MouseLeft" ) name = "M1";
                else if ( name == "MouseRight" ) name = "M2";
                else if ( name == "MouseMiddle" ) name = "M3";
                else {
                    for ( auto& c : name ) c = static_cast<char>( toupper( static_cast<unsigned char>( c ) ) );
                }
                e.key = name;
                e.waiting = false;
                return;
            }
        }
    }

    // ==================================================================
    // checkbox — square purple identity, animated fill + check stroke
    // ==================================================================

    inline bool checkbox( const char* label, bool* v, const char* bind_id = nullptr, const char* default_bind = nullptr, float col_w = 316.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );
        const float width = ( col_w > 0.f ) ? col_w : ImGui::GetContentRegionAvail( ).x;

        const float row_h = theme::compact_row_height;
        const float box_size = 11.0f;

        if ( bind_id ) init_bind( bind_id, default_bind );

        float bind_w = 0.f;
        if ( bind_id ) {
            const char* bd = get_bind_display( bind_id );
            bind_w = ImGui::CalcTextSize( bd ).x + 6.f;
        }

        const float cb_row_w = width - bind_w;
        ImGui::SetCursorScreenPos( pos );
        ImGui::InvisibleButton( label, ImVec2( cb_row_w, row_h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool clicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );
        if ( clicked ) *v = !( *v );
        g_item_hovered = hov;

        const ImGuiID id = ImGui::GetID( label );
        const float fill = anim( id, *v ? 1.f : 0.f, 15.f, *v ? 1.f : 0.f );
        const float hovt = anim( id + 1, hov ? 1.f : 0.f, 12.f, 0.f );

        const ImVec2 sp0 = ImVec2( pos.x, pos.y + ( row_h - box_size ) * 0.5f );
        const ImVec2 sp1 = ImVec2( sp0.x + box_size, sp0.y + box_size );

        dl->AddRectFilled( sp0, sp1, theme::fade( IM_COL32( 13, 13, 16, 255 ) ), theme::control_rounding );
        if ( fill > 0.01f ) {
            const float inset = ( 1.f - theme::ease_out_cubic( fill ) ) * box_size * 0.5f;
            dl->AddRectFilled( ImVec2( sp0.x + inset, sp0.y + inset ), ImVec2( sp1.x - inset, sp1.y - inset ),
                theme::fade( theme::accent_alpha( 0.55f + 0.45f * fill ) ), theme::control_rounding );
        }
        const ImU32 box_border = *v ? theme::accent() : theme::lerp_color( theme::border_bright(), theme::accent_dim(), hovt );
        dl->AddRect( sp0, sp1, theme::fade( box_border ), theme::control_rounding, 0, 1.0f );

        if ( fill > 0.35f ) {
            const float ca = std::clamp( ( fill - 0.35f ) / 0.65f, 0.f, 1.f );
            const float cx = sp0.x + box_size * 0.5f, cy = sp0.y + box_size * 0.5f;
            const ImU32 ck = theme::fade( IM_COL32( 245, 242, 250, static_cast<int>( 255 * ca ) ) );
            dl->AddLine( ImVec2( cx - 2.8f, cy + 0.2f ), ImVec2( cx - 0.8f, cy + 2.2f ), ck, 1.5f );
            dl->AddLine( ImVec2( cx - 0.8f, cy + 2.2f ), ImVec2( cx + 2.9f, cy - 2.2f ), ck, 1.5f );
        }

        const float tly = snap_text_y( pos.y + ( row_h - ImGui::GetTextLineHeight( ) ) * 0.5f );
        dl->AddText( ImVec2( pos.x + box_size + 9.f, tly ),
            theme::fade( hov ? theme::text_bright() : theme::text() ), label );

        if ( bind_id ) {
            const bool waiting = is_bind_waiting( bind_id );
            const char* bd = get_bind_display( bind_id );
            const ImVec2 bts = ImGui::CalcTextSize( bd );
            const float bx = pos.x + width - bts.x;

            const std::string btn_id = std::string( "##bnd_" ) + bind_id;
            ImGui::SetCursorScreenPos( ImVec2( bx - 3.f, pos.y ) );
            ImGui::InvisibleButton( btn_id.c_str( ), ImVec2( bts.x + 6.f, row_h ) );
            const bool bhov = ImGui::IsItemHovered( );
            const bool bclicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );

            if ( bclicked ) {
                if ( waiting ) g_binds[ bind_id ].waiting = false;
                else start_bind_waiting( bind_id );
            }

            static std::unordered_map<std::string, float> pulse;
            float& ph = pulse[ bind_id ];
            if ( waiting ) ph = fmodf( ph + g_delta_time * 4.f, 6.2832f );

            const ImU32 bc = waiting
                ? theme::accent_alpha( 0.55f + 0.45f * sinf( ph ) )
                : ( bhov ? theme::text_accent() : theme::text_dim() );

            dl->AddText( ImVec2( bx, tly ), theme::fade( bc ), bd );
        }

        ImGui::SetCursorScreenPos( ImVec2( pos.x, pos.y + row_h + 1.0f ) );
        return clicked;
    }

    // ==================================================================
    // slider — thin rounded track, eased fill, marker glow while engaged
    // ==================================================================

    inline bool slider_float( const char* label, float* v, float vmin, float vmax, const char* suffix = "", const char* fmt = "%.0f", float col_w = 316.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );
        const float avail_w = ( col_w > 0.f ) ? col_w : ImGui::GetContentRegionAvail( ).x;

        const float lbl_h = ImGui::GetTextLineHeight( );
        const float track_h = 4.0f;
        const float total_h = lbl_h + 6.0f + track_h + 5.0f;

        const ImVec2 tp0 = ImVec2( pos.x, pos.y + lbl_h + 5.0f );
        const ImVec2 tp1 = ImVec2( tp0.x + avail_w, tp0.y + track_h );

        const std::string id = std::string( "##sl" ) + label;
        ImGui::SetCursorScreenPos( ImVec2( tp0.x, tp0.y - 5.0f ) );
        ImGui::InvisibleButton( id.c_str( ), ImVec2( avail_w, track_h + 10.0f ) );
        const bool held = ImGui::IsItemActive( );
        const bool hov = ImGui::IsItemHovered( );
        g_item_hovered = hov || held;
        bool changed = false;

        if ( held ) {
            const float mx = ImGui::GetIO( ).MousePos.x;
            const float t = ImClamp( ( mx - tp0.x ) / avail_w, 0.0f, 1.0f );
            const float nv = vmin + t * ( vmax - vmin );
            if ( fabsf( nv - *v ) > 1e-5f ) { *v = nv; changed = true; }
        }

        const float t = ( vmax > vmin ) ? ImClamp( ( *v - vmin ) / ( vmax - vmin ), 0.0f, 1.0f ) : 0.0f;
        const ImGuiID iid = ImGui::GetID( id.c_str( ) );
        const float display_t = anim( iid, t, held ? 30.f : 16.f, t );
        const float engage = anim( iid + 1, ( hov || held ) ? 1.f : 0.f, 12.f, 0.f );

        dl->AddText( pos, theme::fade( hov || held ? theme::text_bright() : theme::text() ), label );

        dl->AddRectFilled( tp0, tp1, theme::fade( theme::lerp_color( theme::track_bg(), IM_COL32( 40, 40, 48, 255 ), engage ) ), 2.f );

        if ( display_t > 0.001f ) {
            const ImVec2 fe = ImVec2( tp0.x + display_t * avail_w, tp1.y );
            dl->AddRectFilled( tp0, fe, theme::fade( theme::track_fill() ), 2.f );
            dl->AddRectFilled( ImVec2( tp0.x, tp0.y ), ImVec2( fe.x, tp0.y + 1.5f ),
                theme::fade( IM_COL32( 255, 255, 255, 26 ) ), 2.f );
        }

        const float marker_x = tp0.x + display_t * avail_w;
        if ( engage > 0.01f )
            dl->AddCircleFilled( ImVec2( marker_x, tp0.y + track_h * 0.5f ), 7.f,
                theme::fade( theme::accent_alpha( 0.16f * engage ) ), 14 );
        dl->AddRectFilled( ImVec2( marker_x - 1.5f, tp0.y - 2.5f ), ImVec2( marker_x + 1.5f, tp1.y + 2.5f ),
            theme::fade( ( held || hov ) ? theme::text_bright() : theme::handle() ), 1.f );

        char buf[ 32 ], full[ 48 ];
        snprintf( buf, sizeof( buf ), fmt, *v );
        snprintf( full, sizeof( full ), "%s%s", buf, suffix );
        const ImVec2 text_size = ImGui::CalcTextSize( full );
        dl->AddText( ImVec2( pos.x + avail_w - text_size.x, pos.y ),
            theme::fade( ( held || hov ) ? theme::text_bright() : theme::text_accent() ), full );

        ImGui::SetCursorScreenPos( ImVec2( pos.x, pos.y + total_h ) );
        return changed;
    }

    inline bool slider_int( const char* label, int* v, int vmin, int vmax, const char* suffix = "", float col_w = 316.0f ) {
        float fv = static_cast<float>( *v );
        if ( slider_float( label, &fv, static_cast<float>( vmin ), static_cast<float>( vmax ), suffix, "%.0f", col_w ) ) {
            *v = static_cast<int>( std::round( fv ) );
            return true;
        }
        return false;
    }

    // ==================================================================
    // dropdown — deferred foreground popup, animated open
    // ==================================================================

    namespace _dd {
        inline ImGuiID open_id = 0;
        inline int open_frame = -1;
        inline ImVec2 open_pos = {};
        inline float open_w = 0.f;
        inline float open_anim = 0.f;
        inline const char** items = nullptr;
        inline int count = 0;
        inline int* selected = nullptr;
    }

    inline bool dropdown( const char* label, int* sel, const char** items, int count, float col_w = 316.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );

        const float h = 22.0f;
        const float w = ( col_w > 0.f ) ? col_w : ImGui::GetContentRegionAvail( ).x;

        ImGui::SetCursorScreenPos( pos );
        ImGui::InvisibleButton( label, ImVec2( w, h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool clicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );
        g_item_hovered = hov;

        const ImGuiID my_id = ImGui::GetID( label );

        if ( clicked ) {
            if ( _dd::open_id == my_id ) _dd::open_id = 0;
            else {
                _dd::open_id = my_id;
                _dd::open_frame = ImGui::GetFrameCount( );
                _dd::open_pos = ImVec2( pos.x, pos.y + h + 2.f );
                _dd::open_w = w;
                _dd::open_anim = 0.f;
                _dd::items = items;
                _dd::count = count;
                _dd::selected = sel;
            }
        }

        const bool is_open = ( _dd::open_id == my_id );

        dl->AddRectFilled( pos, ImVec2( pos.x + w, pos.y + h ),
            theme::fade( hov ? theme::control_hover() : theme::control_bg() ), theme::control_rounding );
        dl->AddRect( pos, ImVec2( pos.x + w, pos.y + h ),
            theme::fade( is_open ? theme::accent_dim() : ( hov ? theme::border_bright() : theme::border() ) ),
            theme::control_rounding, 0, 1.0f );

        const char* cur = ( *sel >= 0 && *sel < count ) ? items[ *sel ] : "";
        const float ty = snap_text_y( pos.y + ( h - ImGui::GetTextLineHeight( ) ) * 0.5f );
        dl->AddText( ImVec2( pos.x + 8.0f, ty ), theme::fade( theme::text() ), cur );

        // chevron, flips while open
        const float ax = pos.x + w - 15.0f;
        const float ay = pos.y + h * 0.5f;
        const float flip = anim( my_id + 7, is_open ? 1.f : 0.f, 14.f, 0.f );
        const float dir = 1.f - 2.f * flip;
        dl->AddTriangleFilled(
            ImVec2( ax, ay - 1.5f * dir ),
            ImVec2( ax + 6.0f, ay - 1.5f * dir ),
            ImVec2( ax + 3.0f, ay + 2.0f * dir ),
            theme::fade( is_open ? theme::accent() : theme::text_dim() ) );

        ImGui::SetCursorScreenPos( ImVec2( pos.x, pos.y + h + 5.0f ) );
        return clicked;
    }

    inline void render_open_dropdown( ) {
        if ( _dd::open_id == 0 ) return;

        _dd::open_anim = theme::approach( _dd::open_anim, 1.f, g_delta_time, 18.f );
        const float oa = theme::ease_out_cubic( _dd::open_anim );

        ImDrawList* fdl = ImGui::GetForegroundDrawList( );
        const float item_h = 20.0f;
        const float total = static_cast<float>( _dd::count ) * item_h + 4.0f;
        const ImVec2 p0 = ImVec2( _dd::open_pos.x, _dd::open_pos.y - ( 1.f - oa ) * 4.f );
        const ImVec2 p1 = ImVec2( p0.x + _dd::open_w, p0.y + total );
        const ImVec2 mouse = ImGui::GetIO( ).MousePos;

        const float prev_mul = theme::content_mul;
        theme::content_mul = oa;

        fdl->AddRectFilled( ImVec2( p0.x + 2, p0.y + 3 ), ImVec2( p1.x + 2, p1.y + 3 ), theme::fade( IM_COL32( 0, 0, 0, 90 ) ), 3.f );
        fdl->AddRectFilled( p0, p1, theme::fade( IM_COL32( 20, 20, 25, 252 ) ), 3.f );
        fdl->AddRect( p0, p1, theme::fade( theme::border_bright() ), 3.f, 0, 1.0f );

        int click_sel = -1;
        const bool click_any = ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && ( ImGui::GetFrameCount( ) > _dd::open_frame );

        for ( int i = 0; i < _dd::count; i++ ) {
            const ImVec2 ip0 = ImVec2( p0.x + 1.0f, p0.y + 2.0f + static_cast<float>( i ) * item_h );
            const ImVec2 ip1 = ImVec2( p1.x - 1.0f, ip0.y + item_h );

            const bool hov_i = ( mouse.x >= ip0.x && mouse.x <= ip1.x && mouse.y >= ip0.y && mouse.y <= ip1.y );
            const bool sel_i = ( *_dd::selected == i );

            if ( hov_i ) {
                fdl->AddRectFilled( ip0, ip1, theme::fade( theme::tab_hover_bg() ) );
                fdl->AddRectFilled( ImVec2( ip0.x, ip0.y ), ImVec2( ip0.x + 2, ip1.y ), theme::fade( theme::accent() ) );
            }
            else if ( sel_i ) {
                fdl->AddRectFilled( ImVec2( ip0.x, ip0.y ), ImVec2( ip0.x + 2, ip1.y ), theme::fade( theme::accent_dim() ) );
            }

            const float ty = snap_text_y( ip0.y + ( item_h - ImGui::GetTextLineHeight( ) ) * 0.5f );
            fdl->AddText( ImVec2( ip0.x + 9.0f, ty ),
                theme::fade( sel_i ? theme::text_accent() : ( hov_i ? theme::text_bright() : theme::text() ) ), _dd::items[ i ] );

            if ( hov_i && click_any ) click_sel = i;
        }

        theme::content_mul = prev_mul;

        if ( click_sel >= 0 ) {
            *_dd::selected = click_sel;
            _dd::open_id = 0;
            return;
        }

        if ( click_any ) {
            if ( !( mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y ) )
                _dd::open_id = 0;
        }
    }

    // ==================================================================
    // button — subtle fill animation + 1px press nudge
    // ==================================================================

    inline bool button( const char* label, float w, float h = 22.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );
        const ImVec2 p1 = ImVec2( pos.x + w, pos.y + h );

        ImGui::InvisibleButton( label, ImVec2( w, h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool held = ImGui::IsItemActive( );
        const bool clicked = ImGui::IsItemClicked( );
        g_item_hovered = hov;

        const ImGuiID id = ImGui::GetID( label );
        const float hovt = anim( id, ( hov || held ) ? 1.f : 0.f, 12.f, 0.f );

        ImU32 bg = theme::lerp_color( theme::control_bg(), theme::control_hover(), hovt );
        ImU32 bord = theme::lerp_color( theme::border(), theme::accent_dim(), hovt );
        if ( held ) { bg = IM_COL32( 38, 27, 48, 255 ); bord = theme::accent(); }

        dl->AddRectFilled( pos, p1, theme::fade( bg ), theme::control_rounding );
        dl->AddRect( pos, p1, theme::fade( bord ), theme::control_rounding, 0, 1.0f );

        const ImVec2 ts = ImGui::CalcTextSize( label, ImGui::FindRenderedTextEnd( label ) );
        const float tx = pos.x + ( w - ts.x ) * 0.5f;
        const float ty = snap_text_y( pos.y + ( h - ts.y ) * 0.5f + ( held ? 1.f : 0.f ) );
        dl->AddText( ImVec2( tx, ty ), theme::fade( hov ? theme::text_bright() : theme::text() ),
            label, ImGui::FindRenderedTextEnd( label ) );

        return clicked;
    }

    // ==================================================================
    // keybind row — label left, key pill right; returns pill click
    // ==================================================================

    inline bool key_pill( const char* id, const char* label, const char* key_text, bool waiting, float w ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );
        const float h = 24.f;

        const float tly = snap_text_y( pos.y + ( h - ImGui::GetTextLineHeight( ) ) * 0.5f );
        dl->AddText( ImVec2( pos.x, tly ), theme::fade( theme::text() ), label );

        const char* shown = waiting ? "..." : key_text;
        const ImVec2 kts = ImGui::CalcTextSize( shown );
        const float pill_w = std::max( kts.x + 18.f, 44.f );
        const ImVec2 pp0 = ImVec2( pos.x + w - pill_w, pos.y + 1.f );
        const ImVec2 pp1 = ImVec2( pos.x + w, pos.y + h - 1.f );

        ImGui::SetCursorScreenPos( pp0 );
        ImGui::InvisibleButton( id, ImVec2( pill_w, h - 2.f ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool clicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );
        g_item_hovered = hov;

        ImU32 bord = hov ? theme::border_bright() : theme::border();
        ImU32 txt = hov ? theme::text_bright() : theme::text_accent();
        if ( waiting ) {
            bord = theme::accent_alpha( 0.55f + 0.45f * std::sin( g_time * 5.f ) );
            txt = theme::accent_hot();
        }

        dl->AddRectFilled( pp0, pp1, theme::fade( theme::control_bg() ), 3.f );
        dl->AddRect( pp0, pp1, theme::fade( bord ), 3.f, 0, 1.f );
        dl->AddText( ImVec2( pp0.x + ( pill_w - kts.x ) * 0.5f, snap_text_y( pp0.y + ( h - 2.f - ImGui::GetTextLineHeight( ) ) * 0.5f ) ),
            theme::fade( txt ), shown );

        ImGui::SetCursorScreenPos( ImVec2( pos.x, pos.y + h + 3.f ) );
        return clicked;
    }

    // ==================================================================
    // text input
    // ==================================================================

    namespace _ti {
        inline ImGuiID focused_id = 0;
    }

    inline bool text_input( const char* id, char* buf, int buf_size, float w, float h = 22.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );
        const ImGuiID my_id = ImGui::GetID( id );
        const ImVec2 p1 = ImVec2( pos.x + w, pos.y + h );

        ImGui::InvisibleButton( id, ImVec2( w, h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool clicked = ImGui::IsItemClicked( );
        g_item_hovered = hov;

        if ( clicked ) _ti::focused_id = ( _ti::focused_id == my_id ) ? 0 : my_id;
        if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && !hov && _ti::focused_id == my_id ) _ti::focused_id = 0;

        const bool focused = ( _ti::focused_id == my_id );
        bool changed = false;

        if ( focused ) {
            ImGuiIO& io = ImGui::GetIO( );
            io.WantCaptureKeyboard = true;
            int len = static_cast<int>( strlen( buf ) );

            if ( ImGui::IsKeyPressed( ImGuiKey_Backspace ) && len > 0 ) {
                buf[ --len ] = '\0'; changed = true;
            }
            for ( int i = 0; i < io.InputQueueCharacters.Size; i++ ) {
                const ImWchar c = io.InputQueueCharacters[ i ];
                if ( c < 32 || len >= buf_size - 1 ) continue;
                buf[ len++ ] = static_cast<char>( c );
                buf[ len ] = '\0'; changed = true;
            }
            if ( changed ) io.InputQueueCharacters.resize( 0 );
        }

        dl->AddRectFilled( pos, p1, theme::fade( hov ? theme::control_hover() : theme::control_bg() ), theme::control_rounding );
        const ImU32 bord = focused ? theme::accent() : ( hov ? theme::border_bright() : theme::border() );
        dl->AddRect( pos, p1, theme::fade( bord ), theme::control_rounding, 0, 1.0f );

        const int len = static_cast<int>( strlen( buf ) );
        const float ty = snap_text_y( pos.y + ( h - ImGui::GetTextLineHeight( ) ) * 0.5f );
        if ( len > 0 )
            dl->AddText( ImVec2( pos.x + 7.0f, ty ), theme::fade( theme::text_bright() ), buf );
        else if ( !focused )
            dl->AddText( ImVec2( pos.x + 7.0f, ty ), theme::fade( theme::text_dim() ), "..." );

        if ( focused && static_cast<int>( ImGui::GetTime( ) * 2.0 ) % 2 == 0 ) {
            const ImVec2 ts = ImGui::CalcTextSize( buf, buf + len );
            const float cx = pos.x + 7.0f + ts.x + 1.0f;
            dl->AddLine( ImVec2( cx, ty ), ImVec2( cx, ty + ImGui::GetTextLineHeight( ) ), theme::fade( theme::text_bright() ), 1.0f );
        }

        return changed;
    }

    // ==================================================================
    // color picker (unchanged behavior, faded rendering)
    // ==================================================================

    namespace _cp {
        inline ImGuiID open_id = 0;
        inline ImVec2 open_pos = {};
        inline ImVec4* editing_color = nullptr;
        inline float hue = 0.0f, sat = 1.0f, val = 1.0f, alpha = 1.0f;
        inline ImVec2 menu_pos_when_opened = {};
        inline bool dragging_inside = false;
    }

    inline bool color_picker( const char* label, ImVec4* color, float right_edge, float offset_from_right = 0.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 cursor_pos = ImGui::GetCursorScreenPos( );
        const float picker_w = 20.0f, picker_h = 16.0f;
        const float pos_x = right_edge - picker_w - offset_from_right;
        const ImVec2 pos = ImVec2( pos_x, cursor_pos.y );

        ImGui::SetCursorScreenPos( pos );
        ImGui::InvisibleButton( label, ImVec2( picker_w, picker_h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool clicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );
        const ImGuiID my_id = ImGui::GetID( label );

        if ( clicked ) {
            if ( _cp::open_id == my_id ) _cp::open_id = 0;
            else {
                _cp::open_id = my_id;
                _cp::open_pos = ImVec2( pos.x + picker_w * 0.5f, pos.y + picker_h * 0.5f );
                _cp::editing_color = color;
                _cp::menu_pos_when_opened = ImGui::GetWindowPos( );
                ImGui::ColorConvertRGBtoHSV( color->x, color->y, color->z, _cp::hue, _cp::sat, _cp::val );
                _cp::alpha = color->w;
            }
        }

        const ImVec2 p1 = ImVec2( pos.x + picker_w, pos.y + picker_h );
        dl->AddRectFilled( pos, p1, theme::fade( IM_COL32( 10, 10, 18, 220 ) ), 3.0f );
        const ImU32 col_tl = ImGui::ColorConvertFloat4ToU32( *color );
        const ImVec4 darker = ImVec4( color->x * 0.5f, color->y * 0.5f, color->z * 0.5f, 1.0f );
        const ImU32 col_br = ImGui::ColorConvertFloat4ToU32( darker );
        dl->AddRectFilledMultiColor( ImVec2( pos.x + 1, pos.y + 1 ), ImVec2( p1.x - 1, p1.y - 1 ), col_tl, col_tl, col_br, col_br );

        if ( hov ) {
            dl->AddRect( pos, p1, theme::fade( theme::accent_alpha( 150 ) ), 3.0f, 0, 1.5f );
        }

        ImGui::SetCursorScreenPos( ImVec2( cursor_pos.x, cursor_pos.y ) );
        return false;
    }

    inline void render_open_color_picker( ) {
        if ( _cp::open_id == 0 || _cp::editing_color == nullptr ) { _cp::open_id = 0; return; }

        const ImVec2 current_menu_pos = ImGui::GetWindowPos( );
        if ( fabsf( current_menu_pos.x - _cp::menu_pos_when_opened.x ) > 0.1f || fabsf( current_menu_pos.y - _cp::menu_pos_when_opened.y ) > 0.1f ) {
            _cp::open_id = 0; return;
        }

        ImDrawList* fdl = ImGui::GetForegroundDrawList( );
        const float picker_w = 230.0f, picker_h = 260.0f;
        const float sv_size = 190.0f, hue_w = 16.0f, alpha_h = 16.0f, gap = 8.0f;
        const ImVec2 p0 = _cp::open_pos;
        const ImVec2 p1 = ImVec2( p0.x + picker_w, p0.y + picker_h );
        const ImVec2 mouse = ImGui::GetIO( ).MousePos;
        const bool mouse_down = ImGui::IsMouseDown( ImGuiMouseButton_Left );
        const bool mouse_clicked = ImGui::IsMouseClicked( ImGuiMouseButton_Left );
        const bool inside_picker = ( mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y );

        if ( inside_picker && mouse_clicked ) _cp::dragging_inside = true;
        if ( !mouse_down ) _cp::dragging_inside = false;
        if ( inside_picker || _cp::dragging_inside ) ImGui::GetIO( ).WantCaptureMouse = true;
        if ( mouse_clicked && !inside_picker ) { _cp::open_id = 0; _cp::dragging_inside = false; return; }

        fdl->AddRectFilled( ImVec2( p0.x + 2, p0.y + 2 ), ImVec2( p1.x + 2, p1.y + 2 ), theme::fade( IM_COL32( 0, 0, 0, 80 ) ), 6.0f );
        fdl->AddRectFilled( p0, p1, theme::fade( IM_COL32( 10, 10, 18, 220 ) ), 6.0f );
        fdl->AddRect( p0, p1, theme::fade( IM_COL32( 255, 255, 255, 18 ) ), 6.0f, 0, 1.5f );

        const ImVec2 sv_pos = ImVec2( p0.x + 12.0f, p0.y + 12.0f );
        const ImVec2 sv_end = ImVec2( sv_pos.x + sv_size, sv_pos.y + sv_size );

        ImVec4 hue_col;
        ImGui::ColorConvertHSVtoRGB( _cp::hue, 1.0f, 1.0f, hue_col.x, hue_col.y, hue_col.z );
        const ImU32 hue_u32 = ImGui::ColorConvertFloat4ToU32( ImVec4( hue_col.x, hue_col.y, hue_col.z, 1.0f ) );

        fdl->AddRectFilledMultiColor( sv_pos, sv_end, IM_COL32( 255, 255, 255, 255 ), hue_u32, hue_u32, IM_COL32( 0, 0, 0, 255 ) );
        fdl->AddRectFilledMultiColor( sv_pos, sv_end, IM_COL32( 0, 0, 0, 0 ), IM_COL32( 0, 0, 0, 0 ), IM_COL32( 0, 0, 0, 255 ), IM_COL32( 0, 0, 0, 255 ) );
        fdl->AddRect( sv_pos, sv_end, IM_COL32( 40, 40, 60, 100 ), 2.0f );

        const bool sv_hov = ( mouse.x >= sv_pos.x && mouse.x <= sv_end.x && mouse.y >= sv_pos.y && mouse.y <= sv_end.y );
        if ( sv_hov && mouse_down ) {
            _cp::sat = ImClamp( ( mouse.x - sv_pos.x ) / sv_size, 0.0f, 1.0f );
            _cp::val = 1.0f - ImClamp( ( mouse.y - sv_pos.y ) / sv_size, 0.0f, 1.0f );
            if ( _cp::editing_color ) {
                ImGui::ColorConvertHSVtoRGB( _cp::hue, _cp::sat, _cp::val, _cp::editing_color->x, _cp::editing_color->y, _cp::editing_color->z );
                _cp::editing_color->w = _cp::alpha;
            }
        }

        const float cursor_x = sv_pos.x + _cp::sat * sv_size;
        const float cursor_y = sv_pos.y + ( 1.0f - _cp::val ) * sv_size;
        fdl->AddCircleFilled( ImVec2( cursor_x, cursor_y ), 5.0f, IM_COL32( 255, 255, 255, 255 ), 12 );
        fdl->AddCircle( ImVec2( cursor_x, cursor_y ), 5.0f, IM_COL32( 0, 0, 0, 255 ), 12, 1.5f );

        const ImVec2 hue_pos = ImVec2( sv_end.x + gap, sv_pos.y );
        const ImVec2 hue_end = ImVec2( hue_pos.x + hue_w, sv_end.y );

        for ( int i = 0; i < 6; i++ ) {
            const float y0 = hue_pos.y + ( sv_size / 6.0f ) * static_cast<float>( i );
            const float y1 = hue_pos.y + ( sv_size / 6.0f ) * static_cast<float>( i + 1 );
            ImVec4 c0, c1;
            ImGui::ColorConvertHSVtoRGB( static_cast<float>( i ) / 6.0f, 1.0f, 1.0f, c0.x, c0.y, c0.z );
            ImGui::ColorConvertHSVtoRGB( static_cast<float>( i + 1 ) / 6.0f, 1.0f, 1.0f, c1.x, c1.y, c1.z );
            fdl->AddRectFilledMultiColor( ImVec2( hue_pos.x, y0 ), ImVec2( hue_end.x, y1 ),
                ImGui::ColorConvertFloat4ToU32( ImVec4( c0.x, c0.y, c0.z, 1.0f ) ),
                ImGui::ColorConvertFloat4ToU32( ImVec4( c0.x, c0.y, c0.z, 1.0f ) ),
                ImGui::ColorConvertFloat4ToU32( ImVec4( c1.x, c1.y, c1.z, 1.0f ) ),
                ImGui::ColorConvertFloat4ToU32( ImVec4( c1.x, c1.y, c1.z, 1.0f ) ) );
        }
        fdl->AddRect( hue_pos, hue_end, IM_COL32( 40, 40, 60, 100 ), 2.0f );

        const bool hue_hov = ( mouse.x >= hue_pos.x && mouse.x <= hue_end.x && mouse.y >= hue_pos.y && mouse.y <= hue_end.y );
        if ( hue_hov && mouse_down ) {
            _cp::hue = ImClamp( ( mouse.y - hue_pos.y ) / sv_size, 0.0f, 1.0f );
            if ( _cp::editing_color ) {
                ImGui::ColorConvertHSVtoRGB( _cp::hue, _cp::sat, _cp::val, _cp::editing_color->x, _cp::editing_color->y, _cp::editing_color->z );
                _cp::editing_color->w = _cp::alpha;
            }
        }

        const float hue_cursor_y = hue_pos.y + _cp::hue * sv_size;
        fdl->AddRectFilled( ImVec2( hue_pos.x - 2, hue_cursor_y - 2 ), ImVec2( hue_end.x + 2, hue_cursor_y + 2 ), IM_COL32( 255, 255, 255, 255 ), 1.0f );
        fdl->AddRect( ImVec2( hue_pos.x - 2, hue_cursor_y - 2 ), ImVec2( hue_end.x + 2, hue_cursor_y + 2 ), IM_COL32( 0, 0, 0, 255 ), 1.0f, 0, 1.5f );

        const ImVec2 alpha_pos = ImVec2( sv_pos.x, sv_end.y + gap );
        const ImVec2 alpha_end = ImVec2( sv_end.x, alpha_pos.y + alpha_h );

        ImVec4 current_rgb;
        ImGui::ColorConvertHSVtoRGB( _cp::hue, _cp::sat, _cp::val, current_rgb.x, current_rgb.y, current_rgb.z );
        const ImU32 col_opaque = ImGui::ColorConvertFloat4ToU32( ImVec4( current_rgb.x, current_rgb.y, current_rgb.z, 1.0f ) );
        fdl->AddRectFilledMultiColor( alpha_pos, alpha_end,
            IM_COL32( 0, 0, 0, 0 ), col_opaque, col_opaque, IM_COL32( 0, 0, 0, 0 ) );
        fdl->AddRect( alpha_pos, alpha_end, IM_COL32( 40, 40, 60, 100 ), 2.0f );

        const bool alpha_hov = ( mouse.x >= alpha_pos.x && mouse.x <= alpha_end.x && mouse.y >= alpha_pos.y && mouse.y <= alpha_end.y );
        if ( alpha_hov && mouse_down ) {
            _cp::alpha = ImClamp( ( mouse.x - alpha_pos.x ) / sv_size, 0.0f, 1.0f );
            if ( _cp::editing_color ) _cp::editing_color->w = _cp::alpha;
        }

        const float alpha_cursor_x = alpha_pos.x + _cp::alpha * sv_size;
        fdl->AddRectFilled( ImVec2( alpha_cursor_x - 2, alpha_pos.y - 2 ), ImVec2( alpha_cursor_x + 2, alpha_end.y + 2 ), IM_COL32( 255, 255, 255, 255 ), 1.0f );
        fdl->AddRect( ImVec2( alpha_cursor_x - 2, alpha_pos.y - 2 ), ImVec2( alpha_cursor_x + 2, alpha_end.y + 2 ), IM_COL32( 0, 0, 0, 255 ), 1.0f, 0, 1.5f );
    }

    // ==================================================================
    // reset all transient popup/focus state (called when the menu closes
    // so nothing leaks into the next open: no stuck dropdowns, pickers,
    // text focus, or half-finished bind captures)
    // ==================================================================

    inline void close_transient_ui( ) {
        _dd::open_id = 0;
        _cp::open_id = 0;
        _cp::dragging_inside = false;
        _ti::focused_id = 0;
        _tip::candidate = nullptr;
        _tip::current = nullptr;
        _tip::timer = 0.f;
        for ( auto& [ id, e ] : g_binds ) e.waiting = false;
    }

}
