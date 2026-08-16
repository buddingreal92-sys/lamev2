#pragma once

#include <imgui.h>
#include <impl/ui/theme.hxx>
#include <cmath>
#include <unordered_map>

// Small coherent animation layer.
//
// Persistent per-widget values live in one keyed store (keyed by ImGuiID) and
// are advanced with frame-rate independent exponential approach. Nothing here
// allocates per frame after warm-up; maps are bounded by widget count.

namespace ui {

    inline float g_delta_time = 0.0f;
    inline float g_time = 0.0f;

    inline void tick_animations( float dt ) {
        g_delta_time = dt;
        g_time += dt;
    }

    // Persistent animated float, initialized to `initial` on first sight.
    // Typical use: float t = anim( id, hovered ? 1.f : 0.f, 14.f );
    inline float anim( ImGuiID id, float target, float speed, float initial = -1.f ) {
        static std::unordered_map<ImGuiID, float> store;
        auto [ it, inserted ] = store.try_emplace( id, initial >= 0.f ? initial : target );
        it->second = theme::approach( it->second, target, g_delta_time, speed );
        return it->second;
    }

    // ------------------------------------------------------------------
    // tab / page transition state
    // ------------------------------------------------------------------
    inline float g_tab_indicator_y = 0.f;
    inline float g_page_time = 0.f;   // seconds since last tab switch
    inline int   g_last_tab = -1;

    inline void update_tab_transition( int current_tab, float dt ) {
        if ( current_tab != g_last_tab ) {
            g_page_time = 0.f;
            g_last_tab = current_tab;
        }
        g_page_time += dt;
    }

    // 0 → 1 eased entrance of the current page (~150 ms)
    inline float page_entrance( ) {
        return theme::ease_out_cubic( std::clamp( g_page_time / 0.15f, 0.f, 1.f ) );
    }

    inline float breathe( float speed = 1.5f, float min_v = 0.85f, float max_v = 1.0f ) {
        const float t = std::sin( g_time * speed ) * 0.5f + 0.5f;
        return min_v + ( max_v - min_v ) * t;
    }

}
