#pragma once

#include <imgui.h>
#include <cmath>
#include <algorithm>

namespace ui::theme {

    // ------------------------------------------------------------------
    // metrics
    // ------------------------------------------------------------------
    inline constexpr float panel_rounding = 4.0f;
    inline constexpr float control_rounding = 2.0f;
    inline constexpr float compact_row_height = 20.0f;

    // ------------------------------------------------------------------
    // fonts (loaded in c_overlay::create; every pointer falls back to the
    // atlas default if the ttf is missing so drawing code never null-checks)
    // ------------------------------------------------------------------
    inline ImFont* font_base = nullptr;   // 16px regular   — labels, values
    inline ImFont* font_bold = nullptr;   // 16px semibold  — page titles
    inline ImFont* font_small = nullptr;  // 12px regular   — nav labels, help
    inline ImFont* font_tiny = nullptr;   // 11px bold      — section headers
    inline ImFont* font_brand = nullptr;  // 23px bold      — the lamev2 brand
    inline ImFont* font_mono = nullptr;   // 13px mono      — diagnostics

    // ------------------------------------------------------------------
    // global fade — every custom widget multiplies its alpha with this so
    // the whole menu (chrome + content + popups) fades as one surface.
    // menu_alpha : open/close transition, content_mul : tab transition.
    // ------------------------------------------------------------------
    inline float menu_alpha = 1.0f;
    inline float content_mul = 1.0f;

    inline ImU32 fade( ImU32 c ) {
        const float m = menu_alpha * content_mul;
        if ( m >= 0.999f ) return c;
        const ImU32 a = static_cast<ImU32>( static_cast<float>( ( c >> 24 ) & 0xFF ) * std::clamp( m, 0.f, 1.f ) );
        return ( c & 0x00FFFFFF ) | ( a << 24 );
    }

    // ------------------------------------------------------------------
    // palette — layered charcoal + violet accent
    // ------------------------------------------------------------------
    inline ImVec4 accent_v4() { return ImVec4( 0.62f, 0.20f, 0.93f, 1.0f ); }
    inline ImU32 accent() { return IM_COL32( 158, 52, 238, 255 ); }
    inline ImU32 accent_hot() { return IM_COL32( 186, 98, 255, 255 ); }
    inline ImU32 accent_dim() { return IM_COL32( 104, 34, 158, 255 ); }
    inline ImU32 accent_alpha( float a ) {
        const float alpha = a <= 1.f ? a * 255.f : a;
        return IM_COL32( 158, 52, 238, static_cast<int>( std::clamp( alpha, 0.f, 255.f ) ) );
    }

    inline ImU32 window_bg() { return IM_COL32( 13, 13, 16, 252 ); }
    inline ImU32 sidebar_bg() { return IM_COL32( 9, 9, 12, 255 ); }
    inline ImU32 header_bg() { return IM_COL32( 11, 11, 14, 255 ); }
    inline ImU32 panel_bg() { return IM_COL32( 17, 17, 21, 255 ); }
    inline ImU32 panel_bg_deep() { return IM_COL32( 14, 14, 18, 255 ); }
    inline ImU32 control_bg() { return IM_COL32( 24, 24, 29, 255 ); }
    inline ImU32 control_hover() { return IM_COL32( 33, 32, 40, 255 ); }
    inline ImU32 border() { return IM_COL32( 42, 41, 50, 255 ); }
    inline ImU32 border_bright() { return IM_COL32( 66, 64, 78, 255 ); }

    inline ImU32 text() { return IM_COL32( 188, 187, 196, 255 ); }
    inline ImU32 text_bright() { return IM_COL32( 240, 239, 244, 255 ); }
    inline ImU32 text_dim() { return IM_COL32( 112, 110, 122, 255 ); }
    inline ImU32 text_faint() { return IM_COL32( 74, 72, 84, 255 ); }
    inline ImU32 text_accent() { return IM_COL32( 206, 168, 244, 255 ); }

    inline ImU32 good() { return IM_COL32( 104, 226, 160, 255 ); }
    inline ImU32 warn() { return IM_COL32( 232, 200, 108, 255 ); }
    inline ImU32 bad() { return IM_COL32( 244, 116, 116, 255 ); }

    inline ImU32 track_bg() { return IM_COL32( 30, 30, 36, 255 ); }
    inline ImU32 track_fill() { return IM_COL32( 148, 44, 226, 255 ); }
    inline ImU32 handle() { return IM_COL32( 208, 206, 216, 255 ); }

    inline ImU32 tab_active_bg() { return IM_COL32( 19, 18, 24, 255 ); }
    inline ImU32 tab_hover_bg() { return IM_COL32( 15, 15, 19, 255 ); }

    // ------------------------------------------------------------------
    // easing
    // ------------------------------------------------------------------
    inline float ease_out_cubic( float t ) {
        t = std::clamp( t, 0.f, 1.f );
        const float f = t - 1.f;
        return f * f * f + 1.f;
    }

    inline float ease_in_out( float t ) {
        t = std::clamp( t, 0.f, 1.f );
        return t < 0.5f ? 2.f * t * t : 1.f - std::pow( -2.f * t + 2.f, 2.f ) * 0.5f;
    }

    inline float ease_out_back( float t ) {
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.f;
        t = std::clamp( t, 0.f, 1.f );
        return 1.f + c3 * std::pow( t - 1.f, 3.f ) + c1 * std::pow( t - 1.f, 2.f );
    }

    // frame-rate independent exponential approach
    inline float approach( float current, float target, float dt, float speed ) {
        const float next = current + ( target - current ) * std::min( dt * speed, 1.f );
        return std::abs( next - target ) < 0.0005f ? target : next;
    }

    inline ImU32 lerp_color( ImU32 a, ImU32 b, float t ) {
        const float ta = static_cast<float>( ( a >> 24 ) & 0xFF );
        const float tr = static_cast<float>( ( a >> 16 ) & 0xFF );
        const float tg = static_cast<float>( ( a >> 8  ) & 0xFF );
        const float tb = static_cast<float>( ( a       ) & 0xFF );
        const float ba = static_cast<float>( ( b >> 24 ) & 0xFF );
        const float br = static_cast<float>( ( b >> 16 ) & 0xFF );
        const float bg = static_cast<float>( ( b >> 8  ) & 0xFF );
        const float bb = static_cast<float>( ( b       ) & 0xFF );
        return IM_COL32(
            static_cast<int>( tr + ( br - tr ) * t ),
            static_cast<int>( tg + ( bg - tg ) * t ),
            static_cast<int>( tb + ( bb - tb ) * t ),
            static_cast<int>( ta + ( ba - ta ) * t ) );
    }

}
