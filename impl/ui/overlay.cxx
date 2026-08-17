#include <impl/ui/overlay.hxx>
#include <impl/ui/theme.hxx>
#include <impl/ui/animations.hxx>
#include <impl/ui/elements.hxx>
#include <impl/memory/input.hxx>
#include <impl/util/playfield.hxx>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <ShlObj.h>
#include <shobjidl.h>
#include <algorithm>
#include <array>
#include <random>
#include <cstring>

#pragma comment( lib, "d3d11.lib" )
#pragma comment( lib, "dxgi.lib" )
#pragma comment( lib, "d3dcompiler.lib" )
#pragma comment( lib, "dwmapi.lib" )
#pragma comment( lib, "shell32.lib" )
#pragma comment( lib, "ole32.lib" )
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

namespace {

    inline uint64_t get_time_ms( ) {
        static LARGE_INTEGER freq;
        static bool init = false;
        if ( !init ) {
            QueryPerformanceFrequency( &freq );
            init = true;
        }
        LARGE_INTEGER time;
        QueryPerformanceCounter( &time );
        return static_cast<uint64_t>( static_cast<double>( time.QuadPart ) / static_cast<double>( freq.QuadPart ) * 1000.0 );
    }

    bool overlay_aim_hook_transform(
        void* ctx, POINT pen, const MSLLHOOKSTRUCT& raw, POINT* out ) {
        (void)pen;
        if ( !ctx || !out ) return false;
        auto* overlay = static_cast<ui::c_overlay*>( ctx );
        const auto adjusted = overlay->aim( ).apply_hook_move( pen, raw );
        if ( !adjusted ) return false;
        *out = *adjusted;
        return true;
    }

    bool overlay_keyboard_hook_callback( void* ctx, int vk, bool is_down, int64_t press_qpc ) {
        if ( !ctx ) return false;
        auto* overlay = static_cast<ui::c_overlay*>( ctx );
        return overlay->tap( ).handle_key_event( vk, is_down, press_qpc );
    }

    enum ACCENT_STATE {
        ACCENT_DISABLED = 0,
        ACCENT_ENABLE_GRADIENT = 1,
        ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
        ACCENT_ENABLE_BLURBEHIND = 3,
        ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
        ACCENT_ENABLE_HOSTBACKDROP = 5,
        ACCENT_INVALID_STATE = 6
    };

    struct ACCENT_POLICY {
        int State;
        int Flags;
        int GradientColor;
        int AnimationId;
    };

    struct WINCOMPATTRDATA {
        int Attribute;
        ACCENT_POLICY* Data;
        SIZE_T SizeOfData;
        int Reserved;
    };

    inline void enable_backdrop_blur( HWND hwnd ) {
        HMODULE user32 = GetModuleHandleW( L"user32.dll" );
        if ( !user32 ) return;
        auto SetWindowCompositionAttribute = reinterpret_cast<BOOL (WINAPI*)( HWND, WINCOMPATTRDATA* )>(
            GetProcAddress( user32, "SetWindowCompositionAttribute" ) );
        if ( !SetWindowCompositionAttribute ) return;

        // Classic (non-acrylic) blur-behind: substantially weaker than the
        // previous ACCENT_ENABLE_ACRYLICBLURBEHIND treatment, so the game
        // behind the overlay stays recognizable and roughly readable. The
        // menu draws its own translucent dim layer for foreground contrast.
        ACCENT_POLICY policy = { ACCENT_ENABLE_BLURBEHIND, 0, 0, 0 };
        WINCOMPATTRDATA data = { 19, &policy, sizeof( policy ), 0 };
        SetWindowCompositionAttribute( hwnd, &data );

        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea( hwnd, &margins );
    }

    // 2px desaturated spectrum strip along the very top edge of the menu
    inline void draw_rgb_strip( ImDrawList* dl, const ImVec2& p0, float width, float time_seconds ) {
        constexpr int segments = 48;
        constexpr float height = 2.f;
        const float phase = std::fmod( time_seconds * 0.03f, 1.f );
        const int strip_a = static_cast<int>( 230.f * ui::theme::menu_alpha );
        for ( int i = 0; i < segments; ++i ) {
            const float t0 = static_cast<float>( i ) / static_cast<float>( segments );
            const float t1 = static_cast<float>( i + 1 ) / static_cast<float>( segments );
            ImVec4 c0{}, c1{};
            ImGui::ColorConvertHSVtoRGB( std::fmod( t0 + phase, 1.f ), 0.72f, 0.82f, c0.x, c0.y, c0.z );
            ImGui::ColorConvertHSVtoRGB( std::fmod( t1 + phase, 1.f ), 0.72f, 0.82f, c1.x, c1.y, c1.z );
            const float x0 = p0.x + width * t0;
            const float x1 = p0.x + width * t1 + 1.f;
            const ImU32 u0 = ImGui::ColorConvertFloat4ToU32( ImVec4( c0.x, c0.y, c0.z, strip_a / 255.f ) );
            const ImU32 u1 = ImGui::ColorConvertFloat4ToU32( ImVec4( c1.x, c1.y, c1.z, strip_a / 255.f ) );
            dl->AddRectFilledMultiColor( ImVec2( x0, p0.y ), ImVec2( x1, p0.y + height ), u0, u1, u1, u0 );
        }
    }

    // very quiet ambient drift behind the content: 24 dim motes, occasional
    // faint constellation line when two motes pass close to each other
    inline void draw_ambient_particles( ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float dt, float time ) {
        struct particle_t { float x, y, vx, vy, phase, neutral; };
        constexpr int N = 24;
        static std::array<particle_t, N> particles = [] {
            std::array<particle_t, N> out{};
            uint32_t seed = 0x51a7d2b9u;
            auto next = [&]{ seed = seed * 1664525u + 1013904223u; return ( seed >> 8 ) & 0xffffu; };
            for ( auto& p : out ) {
                p.x = next( ) / 65535.f;
                p.y = next( ) / 65535.f;
                p.vx = 0.004f + ( next( ) / 65535.f ) * 0.010f;
                p.vy = -0.006f + ( next( ) / 65535.f ) * 0.012f;
                p.phase = ( next( ) / 65535.f ) * 6.283f;
                p.neutral = ( next( ) & 1 ) ? 1.f : 0.f;
            }
            return out;
        }( );

        const float w = p1.x - p0.x, h = p1.y - p0.y;
        const float fade = ui::theme::menu_alpha;
        ImVec2 pts[ N ];
        float vis[ N ];
        for ( int i = 0; i < N; ++i ) {
            auto& p = particles[ i ];
            p.x += p.vx * dt; p.y += p.vy * dt;
            if ( p.x > 1.02f ) p.x = -0.02f;
            if ( p.y < -0.02f ) p.y = 1.02f;
            if ( p.y > 1.02f ) p.y = -0.02f;
            pts[ i ] = ImVec2( p0.x + p.x * w, p0.y + p.y * h );
            vis[ i ] = 0.30f + 0.70f * ( std::sin( time * 0.55f + p.phase ) * 0.5f + 0.5f );
            const int a = static_cast<int>( ( 6.f + 10.f * vis[ i ] ) * fade );
            const ImU32 col = p.neutral > 0.5f
                ? IM_COL32( 168, 164, 182, a )
                : IM_COL32( 152, 68, 224, a );
            dl->AddCircleFilled( pts[ i ], p.neutral > 0.5f ? 1.f : 1.3f, col, 6 );
        }
        constexpr float link_d = 64.f;
        for ( int i = 0; i < N; ++i ) {
            for ( int j = i + 1; j < N; ++j ) {
                const float dx = pts[ i ].x - pts[ j ].x, dy = pts[ i ].y - pts[ j ].y;
                const float d2 = dx * dx + dy * dy;
                if ( d2 > link_d * link_d ) continue;
                const float closeness = 1.f - std::sqrt( d2 ) / link_d;
                const int a = static_cast<int>( 9.f * closeness * vis[ i ] * vis[ j ] * fade );
                if ( a <= 1 ) continue;
                dl->AddLine( pts[ i ], pts[ j ], IM_COL32( 150, 90, 210, a ), 1.f );
            }
        }
    }

    // ------------------------------------------------------------------
    // navigation icon family — one language: 1.6f strokes, ~18px optical
    // box, geometric, no fills except small emphasis dots
    // ------------------------------------------------------------------
    inline void draw_nav_icon( ImDrawList* dl, int icon, ImVec2 c, ImU32 col ) {
        constexpr float sw = 1.6f;
        switch ( icon ) {
            case 0: { // aim — crosshair
                dl->AddCircle( c, 6.5f, col, 24, sw );
                dl->AddCircleFilled( c, 1.7f, col, 10 );
                dl->AddLine( ImVec2( c.x - 9.5f, c.y ), ImVec2( c.x - 5.f, c.y ), col, sw );
                dl->AddLine( ImVec2( c.x + 5.f, c.y ), ImVec2( c.x + 9.5f, c.y ), col, sw );
                dl->AddLine( ImVec2( c.x, c.y - 9.5f ), ImVec2( c.x, c.y - 5.f ), col, sw );
                dl->AddLine( ImVec2( c.x, c.y + 5.f ), ImVec2( c.x, c.y + 9.5f ), col, sw );
                break;
            }
            case 1: { // relax — two keys, left one pressed
                dl->AddRectFilled( ImVec2( c.x - 9.f, c.y - 2.5f ), ImVec2( c.x - 1.f, c.y + 5.5f ), col, 2.f );
                dl->AddRect( ImVec2( c.x + 1.f, c.y - 5.5f ), ImVec2( c.x + 9.f, c.y + 2.5f ), col, 2.f, 0, sw );
                break;
            }
            case 2: { // tap assist — timing pulse
                const ImVec2 pts[ 6 ] = {
                    ImVec2( c.x - 9.f, c.y + 1.5f ), ImVec2( c.x - 4.5f, c.y + 1.5f ),
                    ImVec2( c.x - 2.f, c.y - 5.5f ), ImVec2( c.x + 1.f, c.y + 6.f ),
                    ImVec2( c.x + 3.f, c.y + 1.5f ), ImVec2( c.x + 9.f, c.y + 1.5f ) };
                dl->AddPolyline( pts, 6, col, 0, sw );
                break;
            }
            case 3: { // replay — circular arrow + play tip
                dl->PathArcTo( c, 7.f, -2.4f, 2.6f, 20 );
                dl->PathStroke( col, 0, sw );
                const float tip_a = -2.4f;
                const ImVec2 tip( c.x + 7.f * std::cos( tip_a ), c.y + 7.f * std::sin( tip_a ) );
                dl->AddTriangleFilled(
                    ImVec2( tip.x - 3.4f, tip.y - 1.6f ),
                    ImVec2( tip.x + 1.6f, tip.y - 3.4f ),
                    ImVec2( tip.x + 1.2f, tip.y + 2.2f ), col );
                dl->AddTriangleFilled(
                    ImVec2( c.x - 1.8f, c.y - 3.f ),
                    ImVec2( c.x + 3.2f, c.y ),
                    ImVec2( c.x - 1.8f, c.y + 3.f ), col );
                break;
            }
            case 4: { // autobot — automation path: start node → curve → arrow
                dl->AddCircleFilled( ImVec2( c.x - 7.f, c.y + 6.f ), 2.f, col, 10 );
                dl->AddBezierCubic(
                    ImVec2( c.x - 7.f, c.y + 6.f ), ImVec2( c.x - 2.f, c.y - 8.f ),
                    ImVec2( c.x + 3.f, c.y + 8.f ), ImVec2( c.x + 7.5f, c.y - 4.5f ), col, sw, 16 );
                dl->AddTriangleFilled(
                    ImVec2( c.x + 4.6f, c.y - 5.2f ),
                    ImVec2( c.x + 10.f, c.y - 7.4f ),
                    ImVec2( c.x + 8.6f, c.y - 1.6f ), col );
                break;
            }
            case 5: { // system — gear
                for ( int i = 0; i < 8; ++i ) {
                    const float a = 6.2831853f * static_cast<float>( i ) / 8.f + 0.3927f;
                    dl->AddLine(
                        ImVec2( c.x + std::cos( a ) * 5.5f, c.y + std::sin( a ) * 5.5f ),
                        ImVec2( c.x + std::cos( a ) * 8.5f, c.y + std::sin( a ) * 8.5f ), col, 2.2f );
                }
                dl->AddCircle( c, 5.5f, col, 20, sw );
                dl->AddCircleFilled( c, 1.8f, col, 10 );
                break;
            }
            case 6: { // config — document with folded corner
                const float x0 = c.x - 6.f, y0 = c.y - 8.f, x1 = c.x + 6.f, y1 = c.y + 8.f;
                const float fold = 4.f;
                const ImVec2 outline[ 5 ] = {
                    ImVec2( x0, y0 ), ImVec2( x1 - fold, y0 ), ImVec2( x1, y0 + fold ),
                    ImVec2( x1, y1 ), ImVec2( x0, y1 ) };
                dl->AddPolyline( outline, 5, col, ImDrawFlags_Closed, sw );
                dl->AddLine( ImVec2( x1 - fold, y0 ), ImVec2( x1 - fold, y0 + fold ), col, 1.2f );
                dl->AddLine( ImVec2( x1 - fold, y0 + fold ), ImVec2( x1, y0 + fold ), col, 1.2f );
                dl->AddLine( ImVec2( x0 + 3.f, c.y - 1.f ), ImVec2( x1 - 3.f, c.y - 1.f ), col, 1.2f );
                dl->AddLine( ImVec2( x0 + 3.f, c.y + 3.f ), ImVec2( x1 - 5.f, c.y + 3.f ), col, 1.2f );
                break;
            }
        }
    }

}

namespace ui {

    bool c_overlay::create( HINSTANCE instance ) {
        ImGui_ImplWin32_EnableDpiAwareness( );

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof( wc );
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = instance;
        wc.lpszClassName = L"  ";
        RegisterClassExW( &wc );

        m_hwnd = CreateWindowExW(
            WS_EX_APPWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            wc.lpszClassName, L"  ", WS_POPUP,
            0, 0, MENU_W, MENU_H,
            nullptr, nullptr, instance, this );

        if ( !m_hwnd ) return false;

        enable_backdrop_blur( m_hwnd );
        SetLayeredWindowAttributes( m_hwnd, 0, 255, LWA_ALPHA );

        ShowWindow( m_hwnd, SW_SHOWNA );
        UpdateWindow( m_hwnd );

        if ( !init_d3d( ) ) return false;

        IMGUI_CHECKVERSION( );
        ImGui::CreateContext( );
        ImGuiIO& io = ImGui::GetIO( );
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // ------------------------------------------------------------------
        // typography stack — regular / semibold / small / tiny-caps / brand /
        // mono, each with a safe fallback if a system ttf is missing
        // ------------------------------------------------------------------
        ImFontConfig font_cfg{};
        font_cfg.OversampleH = 3;
        font_cfg.OversampleV = 2;
        font_cfg.PixelSnapH = true;
        font_cfg.PixelSnapV = true;
        theme::font_base = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeui.ttf", 16.f, &font_cfg );
        theme::font_bold = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeuib.ttf", 16.5f, &font_cfg );
        theme::font_small = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeui.ttf", 12.f, &font_cfg );
        theme::font_tiny = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeuib.ttf", 11.f, &font_cfg );
        theme::font_brand = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeuib.ttf", 23.f, &font_cfg );
        theme::font_mono = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\consola.ttf", 13.f, &font_cfg );
        if ( !theme::font_base ) theme::font_base = io.Fonts->AddFontDefault( );
        if ( !theme::font_bold ) theme::font_bold = theme::font_base;
        if ( !theme::font_small ) theme::font_small = theme::font_base;
        if ( !theme::font_tiny ) theme::font_tiny = theme::font_small;
        if ( !theme::font_brand ) theme::font_brand = theme::font_bold;
        if ( !theme::font_mono ) theme::font_mono = theme::font_small;
        io.FontDefault = theme::font_base;

        ImGuiStyle& style = ImGui::GetStyle( );
        style.WindowRounding = 0.f;
        style.WindowBorderSize = 0.f;
        style.ChildRounding = theme::control_rounding;
        style.PopupRounding = 3.f;
        style.FrameRounding = theme::control_rounding;
        style.GrabRounding = theme::control_rounding;
        style.ScrollbarRounding = 2.f;
        style.FramePadding = ImVec2( 6.f, 3.f );
        style.ItemSpacing = ImVec2( 5.f, 4.f );
        style.ScrollbarSize = 8.f;
        style.Colors[ ImGuiCol_Text ] = ImVec4( 0.74f, 0.73f, 0.77f, 1.f );
        style.Colors[ ImGuiCol_WindowBg ] = ImVec4( 0.05f, 0.05f, 0.06f, 1.f );
        style.Colors[ ImGuiCol_ChildBg ] = ImVec4( 0.06f, 0.06f, 0.07f, 1.f );
        style.Colors[ ImGuiCol_PopupBg ] = ImVec4( 0.08f, 0.08f, 0.10f, 0.98f );
        style.Colors[ ImGuiCol_Border ] = ImVec4( 0.16f, 0.16f, 0.20f, 1.f );
        style.Colors[ ImGuiCol_FrameBg ] = ImVec4( 0.09f, 0.09f, 0.11f, 1.f );
        style.Colors[ ImGuiCol_FrameBgHovered ] = ImVec4( 0.13f, 0.13f, 0.16f, 1.f );
        style.Colors[ ImGuiCol_Header ] = ImVec4( 0.38f, 0.13f, 0.56f, 0.55f );
        style.Colors[ ImGuiCol_HeaderHovered ] = ImVec4( 0.46f, 0.17f, 0.66f, 0.65f );
        style.Colors[ ImGuiCol_HeaderActive ] = ImVec4( 0.54f, 0.20f, 0.76f, 0.75f );
        style.Colors[ ImGuiCol_ScrollbarBg ] = ImVec4( 0.04f, 0.04f, 0.05f, 1.f );
        style.Colors[ ImGuiCol_ScrollbarGrab ] = ImVec4( 0.20f, 0.20f, 0.24f, 1.f );

        ImGui_ImplWin32_Init( m_hwnd );
        ImGui_ImplDX11_Init( m_device, m_context );

        m_mouse_hook.set_filter_injected_only( true );
        m_mouse_hook.set_transform( overlay_aim_hook_transform, this );
        m_mouse_hook.install( );

        m_keyboard_hook.set_callback( overlay_keyboard_hook_callback, this );
        m_keyboard_hook.install( );

        m_aim.start();

        return true;
    }

    void c_overlay::destroy( ) {
        m_aim.stop( );
        m_mouse_hook.uninstall( );
        m_keyboard_hook.uninstall( );

        ImGui_ImplDX11_Shutdown( );
        ImGui_ImplWin32_Shutdown( );
        ImGui::DestroyContext( );
        cleanup_d3d( );
        if ( m_hwnd ) {
            DestroyWindow( m_hwnd );
            m_hwnd = nullptr;
        }
    }

    bool c_overlay::pump( ) {
        MSG msg;
        while ( PeekMessage( &msg, nullptr, 0, 0, PM_REMOVE ) ) {
            TranslateMessage( &msg );
            DispatchMessage( &msg );
            if ( msg.message == WM_QUIT )
                return false;
        }

        // The title-bar X requests a full application exit.  Returning false
        // here stops the main pump loop, which then runs the normal cleanup
        // (cache.stop() + overlay.destroy()).  One close path, no WM_CLOSE race.
        if ( m_exit_requested )
            return false;

        if ( stream_proof )
            SetWindowDisplayAffinity( m_hwnd, WDA_EXCLUDEFROMCAPTURE );
        else
            SetWindowDisplayAffinity( m_hwnd, WDA_NONE );

        handle_hotkeys( );
        update_overlay_position( );
        apply_visibility( );
        render_frame( );
        return true;
    }

    void c_overlay::handle_hotkeys( ) {
        // Strict edge detection: one physical press = exactly one toggle of
        // the single logical state. Holding the key never re-toggles, and
        // the animation below never writes m_visible.
        const bool menu_down = m_menu_keybind > 0 &&
            ( GetAsyncKeyState( m_menu_keybind ) & 0x8000 ) != 0;
        const bool pressed = menu_down && !m_menu_key_was_down;
        m_menu_key_was_down = menu_down;

        if ( !pressed ) return;
        if ( m_waiting_menu ) return; // this press is being captured as the new bind

        m_visible = !m_visible;
        if ( !m_visible )
            close_transient_ui( );
    }

    void c_overlay::update_overlay_position( ) {
        if ( !m_hwnd ) return;

        const HWND osu_hwnd = input::target_window( );
        input::invalidate_virtual_desktop( );
        input::virtual_desktop( );

        int target_x = 0, target_y = 0, target_w = 0, target_h = 0;

        if ( !osu_hwnd || !IsWindow( osu_hwnd ) ) {
            target_w = GetSystemMetrics( SM_CXSCREEN );
            target_h = GetSystemMetrics( SM_CYSCREEN );
        }
        else {
            RECT client{};
            if ( playfield::get_playfield_rect( osu_hwnd, client ) ) {
                target_x = client.left;
                target_y = client.top;
                target_w = client.right - client.left;
                target_h = client.bottom - client.top;
            }
            else {
                target_w = GetSystemMetrics( SM_CXSCREEN );
                target_h = GetSystemMetrics( SM_CYSCREEN );
            }
        }

        SetWindowPos( m_hwnd, HWND_TOPMOST, target_x, target_y, target_w, target_h, SWP_NOACTIVATE );
    }

    void c_overlay::apply_visibility( ) {
        if ( !m_hwnd ) return;

        static bool prev_stream_proof = false;
        const bool stream_proof_changed = prev_stream_proof != stream_proof;
        prev_stream_proof = stream_proof;

        // Window stays alive through the whole close animation; input is
        // released the moment the close is *requested* (WS_EX_TRANSPARENT)
        // so fading-out controls can no longer be clicked.
        const bool should_show = render_visible( );

        if ( should_show ) {
            LONG ex = GetWindowLongW( m_hwnd, GWL_EXSTYLE );
            if ( stream_proof ) {
                ex |= WS_EX_TOOLWINDOW;
                ex &= ~WS_EX_APPWINDOW;
            } else {
                ex |= WS_EX_APPWINDOW;
                ex &= ~WS_EX_TOOLWINDOW;
            }
            if ( m_visible ) {
                ex &= ~WS_EX_TRANSPARENT;
            } else {
                ex |= WS_EX_TRANSPARENT;
            }
            SetWindowLongW( m_hwnd, GWL_EXSTYLE, ex );
            ShowWindow( m_hwnd, SW_SHOWNA );

            if ( stream_proof_changed ) {
                ShowWindow( m_hwnd, SW_HIDE );
                ShowWindow( m_hwnd, SW_SHOWNA );
            }
        }
        else {
            ShowWindow( m_hwnd, SW_HIDE );
        }
    }

    LRESULT CALLBACK c_overlay::wnd_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) {
        // The menu receives mouse messages but must never take foreground focus
        // from osu!.  Lazer changes its top screen when it loses focus, which
        // otherwise makes its existing player-screen state test report menu.
        if ( msg == WM_MOUSEACTIVATE )
            return MA_NOACTIVATE;
        if ( ImGui_ImplWin32_WndProcHandler( hwnd, msg, wp, lp ) )
            return true;

        if ( msg == WM_NCCREATE ) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>( lp );
            SetWindowLongPtrW( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( cs->lpCreateParams ) );
        }

        auto* self = reinterpret_cast<c_overlay*>( GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );
        if ( msg == WM_DESTROY ) {
            PostQuitMessage( 0 );
            return 0;
        }
        if ( msg == WM_SETCURSOR ) {
            if ( self && self->stream_proof && self->m_streamproof_hide_cursor ) {
                SetCursor( nullptr );
                return 1;
            }
            SetCursor( LoadCursorW( nullptr, IDC_ARROW ) );
            return 1;
        }
        if ( msg == WM_SIZE && self && self->m_swap_chain ) {
            UINT w = LOWORD( lp ), h = HIWORD( lp );
            if ( w == 0 || h == 0 ) return 0;
            if ( self->m_rtv ) {
                self->m_rtv->Release( );
                self->m_rtv = nullptr;
            }
            self->m_swap_chain->ResizeBuffers( 0, w, h, DXGI_FORMAT_UNKNOWN, 0 );
            ID3D11Texture2D* back = nullptr;
            if ( SUCCEEDED( self->m_swap_chain->GetBuffer( 0, IID_PPV_ARGS( &back ) ) ) && back ) {
                self->m_device->CreateRenderTargetView( back, nullptr, &self->m_rtv );
                back->Release( );
            }
        }
        return DefWindowProcW( hwnd, msg, wp, lp );
    }

    bool c_overlay::init_d3d( ) {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = m_hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL level{};
        if ( D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &sd, &m_swap_chain, &m_device, &level, &m_context ) != S_OK )
            return false;

        ID3D11Texture2D* back = nullptr;
        m_swap_chain->GetBuffer( 0, IID_PPV_ARGS( &back ) );
        if ( !back ) return false;
        m_device->CreateRenderTargetView( back, nullptr, &m_rtv );
        back->Release( );
        return m_rtv != nullptr;
    }

    void c_overlay::cleanup_d3d( ) {
        if ( m_rtv ) { m_rtv->Release( ); m_rtv = nullptr; }
        if ( m_swap_chain ) { m_swap_chain->Release( ); m_swap_chain = nullptr; }
        if ( m_context ) { m_context->Release( ); m_context = nullptr; }
        if ( m_device ) { m_device->Release( ); m_device = nullptr; }
    }

    void c_overlay::render_frame( ) {
        osu::full_snapshot_t snap;
        if ( m_snapshot_fn )
            snap = m_snapshot_fn( );

        if ( !render_visible( ) ) {
            m_streamproof_hide_cursor = false;
            if ( m_context && m_rtv ) {
                const float clear[ 4 ]{ 0.0f, 0.0f, 0.0f, 0.0f };
                m_context->OMSetRenderTargets( 1, &m_rtv, nullptr );
                m_context->ClearRenderTargetView( m_rtv, clear );
            }
            if ( m_swap_chain ) m_swap_chain->Present( 0, 0 );
            return;
        }

        ImGui_ImplDX11_NewFrame( );
        ImGui_ImplWin32_NewFrame( );
        ImGui::NewFrame( );

        // Clamp dt for animation purposes: after the window was hidden for a
        // while, the first frame's DeltaTime spans the whole hidden period and
        // would otherwise snap every transition to its end state.
        const float dt = std::min( ImGui::GetIO( ).DeltaTime, 1.f / 30.f );
        tick_animations( dt );
        m_anim_time = g_time;

        // Visual transition follows the logical state; it never writes it.
        // ~150 ms open, ~120 ms close, frame-rate independent.
        {
            const float target = m_visible ? 1.f : 0.f;
            const float rate = m_visible ? 13.f : 17.f;
            m_menu_open_anim += ( target - m_menu_open_anim ) * std::min( dt * rate, 1.f );
            if ( !m_visible && m_menu_open_anim <= 0.012f )
                m_menu_open_anim = 0.f;
            else if ( m_visible && m_menu_open_anim >= 0.998f )
                m_menu_open_anim = 1.f;
        }

        theme::menu_alpha = std::min( m_menu_open_anim * 1.2f, 1.f );
        theme::content_mul = 1.f;

        {
            ImDrawList* bg_dl = ImGui::GetBackgroundDrawList( );
            const ImVec2 disp = ImGui::GetIO( ).DisplaySize;
            const float bg_a = m_menu_open_anim * 0.12f;
            if ( bg_a > 0.005f ) {
                const int a = static_cast<int>( bg_a * 255 );
                bg_dl->AddRectFilledMultiColor(
                    ImVec2( 0, 0 ), disp,
                    IM_COL32( 2, 2, 12, a ), IM_COL32( 2, 2, 12, a ),
                    IM_COL32( 4, 2, 16, a ), IM_COL32( 4, 2, 16, a ) );
            }
        }

        draw_menu( snap );
        m_streamproof_hide_cursor = m_visible && stream_proof && ( ImGui::GetIO( ).WantCaptureMouse );

        ImGui::Render( );

        if ( m_context && m_rtv ) {
            const float clear_color[ 4 ]{ 0.0f, 0.0f, 0.0f, 0.0f };
            m_context->OMSetRenderTargets( 1, &m_rtv, nullptr );
            m_context->ClearRenderTargetView( m_rtv, clear_color );
        }

        ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData( ) );
        if ( m_swap_chain ) m_swap_chain->Present( 0, 0 );
    }

    void c_overlay::apply_custom_keys( osu::game_snapshot_t& game ) const {
        game.left_key = m_custom_left_key;
        game.right_key = m_custom_right_key;
    }

    config::settings_t c_overlay::capture_settings( ) const {
        config::settings_t s{};
        s.aim_enabled = m_aim.enabled;
        s.aim_ignore_sliders = m_aim.ignore_sliders;
        s.aim_tablet_mode = m_aim.tablet_mode;

        s.aim_strength = m_aim.assist_strength;
        s.aim_radius = m_aim.assist_radius;
        s.aim_smoothing_ms = m_aim.smoothing_ms;
        s.aim_max_correction = m_aim.max_correction;
        s.aim_adaptive = m_aim.adaptive_aim;
        s.aim_adaptation_strength = m_aim.adaptation_strength;

        s.relax_enabled = m_relax.enabled;
        s.relax_ur = m_relax.ur;
        s.relax_tap_style = m_relax.tap_style;
        s.relax_primary_key = m_relax.primary_key;
        s.relax_singletap_speed_bpm = m_relax.singletap_speed_bpm;
        s.relax_burst_tolerance = m_relax.burst_tolerance;
        s.relax_stamina = m_relax.stamina;
        s.relax_k1_hold_center = m_relax.k1_hold_center;
        s.relax_k1_hold_spread = m_relax.k1_hold_spread;
        s.relax_k2_hold_center = m_relax.k2_hold_center;
        s.relax_k2_hold_spread = m_relax.k2_hold_spread;
        s.relax_hold_floor = m_relax.hold_floor;
        s.relax_hold_ceiling = m_relax.hold_ceiling;
        s.relax_manual_offset_ms = m_relax.manual_offset_ms;
        s.relax_timing_variation = m_relax.timing_variation;
        s.relax_early_variation_ms = m_relax.early_variation_ms;
        s.relax_late_variation_ms = m_relax.late_variation_ms;
        s.relax_timing_drift_ms = m_relax.timing_drift_ms;

        s.replay_enabled = m_replay.enabled;
        s.replay_path_utf8 = m_replay_path_utf8;
        s.replay_parse_buttons = m_replay.parse_buttons;

        s.autobot_enabled = m_autobot.enabled;
        s.autobot_target_accuracy = m_autobot.target_accuracy;
        s.autobot_aim_spread = m_autobot.aim_spread;
        s.autobot_curve_strength = m_autobot.curve_strength;
        s.autobot_drift_amount = m_autobot.drift_amount;
        s.autobot_momentum = m_autobot.momentum;
        s.autobot_slider_laziness = m_autobot.slider_laziness;
        s.autobot_spinner_rpm = m_autobot.spinner_rpm;
        s.autobot_startup_motion = m_autobot.startup_motion;
        s.autobot_break_motion = m_autobot.break_motion;
        s.autobot_energetic_dances = m_autobot.energetic_dances;
        s.autobot_gameplay_flow = m_autobot.gameplay_flow;
        s.autobot_startup_energy = m_autobot.startup_energy;
        s.autobot_break_energy = m_autobot.break_energy;

        s.tap_enabled = m_tap_assist.enabled;
        s.tap_assist_window = m_tap_assist.assist_window;
        s.tap_randomization = m_tap_assist.randomization;
        s.tap_ignore_sliders = m_tap_assist.ignore_sliders;

        s.custom_left_key = m_custom_left_key;
        s.custom_right_key = m_custom_right_key;
        s.menu_keybind = m_menu_keybind;
        s.stream_proof = stream_proof;
        s.songs_path_utf8 = m_songs_path_utf8;

        return s;
    }

    void c_overlay::apply_settings( const config::settings_t& s ) {
        m_aim.enabled = s.aim_enabled;
        m_aim.ignore_sliders = s.aim_ignore_sliders;
        m_aim.tablet_mode = s.aim_tablet_mode;

        m_aim.assist_strength = s.aim_strength;
        m_aim.assist_radius = s.aim_radius;
        m_aim.smoothing_ms = s.aim_smoothing_ms;
        m_aim.max_correction = s.aim_max_correction;
        m_aim.adaptive_aim = s.aim_adaptive;
        m_aim.adaptation_strength = s.aim_adaptation_strength;

        m_relax.enabled = s.relax_enabled;
        m_relax.ur = s.relax_ur;
        m_relax.tap_style = s.relax_tap_style;
        m_relax.primary_key = s.relax_primary_key;
        m_relax.singletap_speed_bpm = s.relax_singletap_speed_bpm;
        m_relax.burst_tolerance = s.relax_burst_tolerance;
        m_relax.stamina = s.relax_stamina;
        m_relax.k1_hold_center = s.relax_k1_hold_center;
        m_relax.k1_hold_spread = s.relax_k1_hold_spread;
        m_relax.k2_hold_center = s.relax_k2_hold_center;
        m_relax.k2_hold_spread = s.relax_k2_hold_spread;
        m_relax.hold_floor = s.relax_hold_floor;
        m_relax.hold_ceiling = s.relax_hold_ceiling;
        m_relax.manual_offset_ms = s.relax_manual_offset_ms;
        m_relax.timing_variation = s.relax_timing_variation;
        m_relax.early_variation_ms = s.relax_early_variation_ms;
        m_relax.late_variation_ms = s.relax_late_variation_ms;
        m_relax.timing_drift_ms = s.relax_timing_drift_ms;

        m_replay.enabled = s.replay_enabled;
        if ( !s.replay_path_utf8.empty( ) ) {
            strncpy_s( m_replay_path_utf8, s.replay_path_utf8.c_str( ), _TRUNCATE );
            int wlen = MultiByteToWideChar( CP_UTF8, 0, m_replay_path_utf8, -1, nullptr, 0 );
            if ( wlen > 1 ) {
                std::wstring wide( static_cast<size_t>( wlen - 1 ), L'\0' );
                MultiByteToWideChar( CP_UTF8, 0, m_replay_path_utf8, -1, wide.data( ), wlen );
                m_replay.replay_path = wide;
                m_replay.load_replay( );
                m_replay.reset_sync( );
            }
        }

        m_replay.parse_buttons = s.replay_parse_buttons;

        m_autobot.enabled = s.autobot_enabled;
        m_autobot.target_accuracy = s.autobot_target_accuracy;
        m_autobot.aim_spread = s.autobot_aim_spread;
        m_autobot.curve_strength = s.autobot_curve_strength;
        m_autobot.drift_amount = s.autobot_drift_amount;
        m_autobot.momentum = s.autobot_momentum;
        m_autobot.slider_laziness = s.autobot_slider_laziness;
        m_autobot.spinner_rpm = s.autobot_spinner_rpm;
        m_autobot.startup_motion = s.autobot_startup_motion;
        m_autobot.break_motion = s.autobot_break_motion;
        m_autobot.energetic_dances = s.autobot_energetic_dances;
        m_autobot.gameplay_flow = s.autobot_gameplay_flow;
        m_autobot.startup_energy = s.autobot_startup_energy;
        m_autobot.break_energy = s.autobot_break_energy;

        m_tap_assist.enabled = s.tap_enabled;
        m_tap_assist.assist_window = s.tap_assist_window;
        m_tap_assist.randomization = s.tap_randomization;
        m_tap_assist.ignore_sliders = s.tap_ignore_sliders;

        m_custom_left_key = s.custom_left_key;
        m_custom_right_key = s.custom_right_key;
        m_menu_keybind = ( s.menu_keybind > 0 && s.menu_keybind < 256 ) ? s.menu_keybind : VK_DELETE;
        stream_proof = s.stream_proof;



        if ( !s.songs_path_utf8.empty( ) ) {
            strncpy_s( m_songs_path_utf8, s.songs_path_utf8.c_str( ), _TRUNCATE );
            if ( m_cache ) {
                wchar_t wide[ 512 ]{};
                MultiByteToWideChar( CP_UTF8, 0, m_songs_path_utf8, -1, wide, 512 );
                m_cache->stable_parser( ).set_songs_path( wide );
                m_cache->invalidate_beatmap_cache( );
            }
        }
    }

    void c_overlay::reset_modules( const osu::game_snapshot_t& game ) {
        m_game_time_stall_start_ms = 0;
        m_aim.set_user_input_blocked( false );
        m_aim.on_leave_play( );
        osu::game_snapshot_t mod_game = game;
        apply_custom_keys( mod_game );
        m_relax.on_leave_play( mod_game );
        m_replay.on_leave_play( mod_game );
        m_autobot.on_leave_play( mod_game );
        m_tap_assist.on_leave_play( mod_game );
    }

    void c_overlay::tick_modules( const osu::game_snapshot_t& game, const osu::beatmap_data_t& beatmap ) {
        const bool in_play = game.cur_state == osu::game_state_t::play;
        const bool replay_active = game.is_replay;
        const bool was_play = m_prev_state == osu::game_state_t::play;
        const std::string map_sig =
            std::to_string( game.map_id ) + "|" + std::to_string( game.set_id ) + "|" +
            game.map_folder + "|" + game.map_file + "|" + game.beatmap_hash + "|" +
            game.beatmap_version;

        if ( replay_active ) {
            m_prev_game_time = -1;
            m_prev_map_id = -1;
            m_prev_map_sig.clear( );
            m_game_time_stall_start_ms = 0;
            m_aim.set_user_input_blocked( true );
            return;
        }

        if ( !was_play && in_play )
            m_aim.begin_play_verification( );

        if ( was_play && !in_play ) {
            reset_modules( game );
            m_prev_game_time = -1;
            m_prev_map_id = -1;
            m_prev_map_sig.clear( );
            m_game_time_stall_start_ms = 0;
        }

        if ( in_play && !map_sig.empty( ) && !m_prev_map_sig.empty( ) && map_sig != m_prev_map_sig ) {
            m_aim.begin_play_verification( );
            reset_modules( game );
        }

        if ( in_play && m_prev_map_id > 0 && game.map_id != 0 && game.map_id != m_prev_map_id ) {
            m_aim.begin_play_verification( );
            reset_modules( game );
        }

        if ( was_play && in_play && m_prev_game_time >= 0 && game.cur_time < m_prev_game_time - 200 ) {
            m_aim.begin_play_verification( );
            reset_modules( game );
        }

        m_prev_state = game.cur_state;

        if ( in_play && beatmap.loaded && !beatmap.objects.empty( ) ) {
            osu::game_snapshot_t mod_game = game;
            apply_custom_keys( mod_game );

            constexpr uint64_t k_pause_stall_ms = 120;
            const uint64_t     now_ms = get_time_ms( );
            const int32_t      cur_time = mod_game.cur_time;
            const bool         time_stalled = m_prev_game_time >= 0 && cur_time == m_prev_game_time;

            if ( time_stalled ) {
                if ( m_game_time_stall_start_ms == 0 )
                    m_game_time_stall_start_ms = now_ms;
            }
            else {
                m_game_time_stall_start_ms = 0;
            }

            const bool map_paused = m_game_time_stall_start_ms != 0
                                    && ( now_ms - m_game_time_stall_start_ms ) >= k_pause_stall_ms;

            m_aim.set_user_input_blocked( map_paused );

            if ( !map_paused )
                m_aim.update( mod_game, beatmap );

            // Autobot and standalone Relax have exactly one press/release owner.
            // When Autobot is active, prepare the shared schedule first, move the
            // cursor, then flush that same queue so the arrival precedes the press.
            if ( m_autobot.enabled )
                m_relax.prepare_for_autobot( mod_game, beatmap, m_autobot.target_accuracy );
            else
                m_relax.update( mod_game, beatmap );
            m_replay.update( mod_game, beatmap, map_paused );
            if ( m_autobot.enabled ) {
                m_autobot.update( mod_game, beatmap, m_relax, map_paused );
                m_relax.flush_for_autobot( mod_game );
            }
            else
                m_autobot.update( mod_game, beatmap, m_relax, map_paused );
            m_tap_assist.update( mod_game, beatmap );
        }
        else {
            m_game_time_stall_start_ms = 0;
            m_aim.set_user_input_blocked( false );
        }

        if ( in_play ) {
            m_prev_map_id = game.map_id;
            m_prev_game_time = game.cur_time;
            if ( !map_sig.empty( ) )
                m_prev_map_sig = map_sig;
        }
    }


    void c_overlay::draw_menu( const osu::full_snapshot_t& snap ) {
        update_bind_capture( );

        ImGuiIO& io = ImGui::GetIO( );

        constexpr float W = static_cast<float>( MENU_W );
        constexpr float H = static_cast<float>( MENU_H );
        constexpr float SB_W = 96.f;     // navigation rail
        constexpr float HDR_H = 38.f;    // header strip
        constexpr float COL_W = 344.f;   // panel width (both columns)
        constexpr float L_X = SB_W + 12.f;
        constexpr float R_X = L_X + COL_W + 12.f;
        constexpr float CW = COL_W - 28.f;   // control width inside a panel

        const float ease = theme::ease_out_cubic( m_menu_open_anim );
        const float alpha = theme::menu_alpha;

        // keep the menu reachable: never allow it to be dragged fully off-screen
        {
            const float base_x = ( io.DisplaySize.x - W ) * 0.5f;
            const float base_y = ( io.DisplaySize.y - H ) * 0.5f;
            m_menu_offset_x = std::clamp( m_menu_offset_x,
                static_cast<int>( -base_x - W + 90.f ), static_cast<int>( io.DisplaySize.x - base_x - 90.f ) );
            m_menu_offset_y = std::clamp( m_menu_offset_y,
                static_cast<int>( -base_y ), static_cast<int>( io.DisplaySize.y - base_y - 60.f ) );
        }

        const float menu_x = ( io.DisplaySize.x - W ) * 0.5f + static_cast<float>( m_menu_offset_x );
        const float menu_y = ( io.DisplaySize.y - H ) * 0.5f + static_cast<float>( m_menu_offset_y ) + ( 1.f - ease ) * 6.f;

        ImGui::SetNextWindowPos( ImVec2( menu_x, menu_y ), ImGuiCond_Always );
        ImGui::SetNextWindowSize( ImVec2( W, H ), ImGuiCond_Always );

        ImGuiWindowFlags wf =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoMove;

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );
        ImGui::Begin( "##  ", nullptr, wf );
        ImGui::PopStyleVar( 2 );
        ImGui::PushStyleVar( ImGuiStyleVar_Alpha, alpha );

        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 wpos = ImGui::GetWindowPos( );
        const ImVec2 wsize = ImGui::GetWindowSize( );
        auto S = [&]( float x, float y ) { return ImVec2( wpos.x + x, wpos.y + y ); };

        // ------------------------------------------------------------------
        // window shell
        // ------------------------------------------------------------------
        const ImVec2 br = ImVec2( wpos.x + wsize.x, wpos.y + wsize.y );
        dl->AddRectFilled( wpos, br, theme::fade( theme::window_bg() ) );
        draw_ambient_particles( dl, wpos, br, g_delta_time, m_anim_time );

        // navigation rail (drawn after particles so they live behind content only)
        dl->AddRectFilled( ImVec2( wpos.x, wpos.y + 2.f ), ImVec2( wpos.x + SB_W, br.y ), theme::fade( theme::sidebar_bg() ) );
        dl->AddLine( ImVec2( wpos.x + SB_W, wpos.y + 2.f ), ImVec2( wpos.x + SB_W, br.y ),
            theme::fade( theme::border() ), 1.0f );

        dl->AddRect( wpos, br, theme::fade( IM_COL32( 52, 51, 60, 255 ) ), 0.f, 0, 1.0f );
        draw_rgb_strip( dl, wpos, wsize.x, m_anim_time );

        // ------------------------------------------------------------------
        // brand — lame(v2)
        // ------------------------------------------------------------------
        {
            ImFont* bf = theme::font_brand;
            ImFont* vf = theme::font_bold;
            const float bw = bf->CalcTextSizeA( bf->LegacySize, FLT_MAX, 0.f, "lame" ).x;
            const float vw = vf->CalcTextSizeA( vf->LegacySize, FLT_MAX, 0.f, "v2" ).x;
            const float total = bw + 3.f + vw;
            const float bx = wpos.x + ( SB_W - total ) * 0.5f;
            const float by = wpos.y + 13.f;

            dl->AddText( bf, bf->LegacySize, ImVec2( bx + 1.f, by + 2.f ), theme::fade( IM_COL32( 0, 0, 0, 150 ) ), "lame" );
            dl->AddText( bf, bf->LegacySize, ImVec2( bx, by + 1.f ), theme::fade( theme::text_bright() ), "lame" );
            dl->AddText( vf, vf->LegacySize, ImVec2( bx + bw + 3.f + 1.f, by + 1.f ), theme::fade( theme::accent_alpha( 0.35f ) ), "v2" );
            dl->AddText( vf, vf->LegacySize, ImVec2( bx + bw + 3.f, by ), theme::fade( theme::accent_hot() ), "v2" );

            // shimmer underline: a soft accent highlight drifting along a hairline
            const float uy = by + bf->LegacySize + 4.f;
            const float ux0 = wpos.x + 16.f, ux1 = wpos.x + SB_W - 16.f;
            dl->AddLine( ImVec2( ux0, uy ), ImVec2( ux1, uy ), theme::fade( IM_COL32( 40, 39, 48, 255 ) ), 1.f );
            const float sc = ux0 + ( ux1 - ux0 ) * ( 0.5f + 0.5f * std::sin( m_anim_time * 0.9f ) );
            const float sw2 = 14.f;
            dl->AddRectFilledMultiColor( ImVec2( sc - sw2, uy - 0.5f ), ImVec2( sc, uy + 0.5f ),
                theme::fade( theme::accent_alpha( 0.0f ) ), theme::fade( theme::accent_alpha( 0.8f ) ),
                theme::fade( theme::accent_alpha( 0.8f ) ), theme::fade( theme::accent_alpha( 0.0f ) ) );
            dl->AddRectFilledMultiColor( ImVec2( sc, uy - 0.5f ), ImVec2( sc + sw2, uy + 0.5f ),
                theme::fade( theme::accent_alpha( 0.8f ) ), theme::fade( theme::accent_alpha( 0.0f ) ),
                theme::fade( theme::accent_alpha( 0.0f ) ), theme::fade( theme::accent_alpha( 0.8f ) ) );
        }

        // ------------------------------------------------------------------
        // navigation
        // ------------------------------------------------------------------
        {
            update_tab_transition( m_tab, g_delta_time );

            static const char* tab_names[ ] = { "Aim Assist", "Relax", "Tap Assist", "Replay", "Autobot", "System", "Config" };
            const bool module_on[ 7 ] = {
                m_aim.enabled, m_relax.enabled, m_tap_assist.enabled,
                m_replay.enabled, m_autobot.enabled, false, false };
            const bool module_dot[ 7 ] = { true, true, true, true, true, false, false };

            const float nav_y0 = 64.f;
            const float item_h = 52.f;
            const float item_gap = 2.f;

            const ImVec2 mouse = io.MousePos;
            const bool mouse_clicked = ImGui::IsMouseClicked( ImGuiMouseButton_Left );

            const float target_ind = wpos.y + nav_y0 + static_cast<float>( m_tab ) * ( item_h + item_gap );
            if ( g_tab_indicator_y <= 0.f ) g_tab_indicator_y = target_ind;
            g_tab_indicator_y = theme::approach( g_tab_indicator_y, target_ind, g_delta_time, 16.f );

            for ( int i = 0; i < 7; i++ ) {
                const float ty = wpos.y + nav_y0 + static_cast<float>( i ) * ( item_h + item_gap );
                const ImVec2 b0( wpos.x + 1.f, ty );
                const ImVec2 b1( wpos.x + SB_W - 1.f, ty + item_h );

                const bool hov = m_visible &&
                    mouse.x >= b0.x && mouse.x <= b1.x && mouse.y >= b0.y && mouse.y <= b1.y;
                const bool active = ( i == m_tab );
                if ( hov && mouse_clicked ) m_tab = i;

                const float hovt = anim( 0x5AB000 + static_cast<ImGuiID>( i ), ( hov || active ) ? 1.f : 0.f, 13.f, 0.f );

                if ( hovt > 0.01f ) {
                    const ImU32 bg = active ? theme::tab_active_bg() : theme::tab_hover_bg();
                    dl->AddRectFilled( b0, b1, theme::fade( theme::lerp_color( theme::sidebar_bg(), bg, hovt ) ) );
                }
                if ( active ) {
                    dl->AddRectFilled( ImVec2( b0.x, g_tab_indicator_y + 10.f ),
                        ImVec2( b0.x + 2.f, g_tab_indicator_y + item_h - 10.f ), theme::fade( theme::accent() ), 1.f );
                }

                const float shift = hovt * 1.f;
                const ImVec2 ic( wpos.x + SB_W * 0.5f + shift, ty + 19.f );
                const ImU32 ic_col = active ? theme::text_bright() : ( hov ? theme::text() : theme::text_dim() );

                if ( active ) {
                    dl->AddCircleFilled( ic, 13.f, theme::fade( theme::accent_alpha( 0.10f ) ), 20 );
                    dl->AddCircleFilled( ic, 7.f, theme::fade( theme::accent_alpha( 0.08f ) ), 16 );
                }
                draw_nav_icon( dl, i, ic, theme::fade( ic_col ) );

                if ( module_dot[ i ] )
                    status_dot( dl, ImVec2( ic.x + 11.f, ic.y - 9.f ), module_on[ i ], 2.f );

                ImFont* lf = theme::font_small;
                const ImVec2 ts = lf->CalcTextSizeA( lf->LegacySize, FLT_MAX, 0.f, tab_names[ i ] );
                const ImU32 label_col = active ? theme::text_accent() : ( hov ? theme::text() : theme::text_faint() );
                dl->AddText( lf, lf->LegacySize,
                    ImVec2( b0.x + ( SB_W - 2.f - ts.x ) * 0.5f + shift, snap_text_y( ty + 33.f ) ),
                    theme::fade( label_col ), tab_names[ i ] );
            }
        }

        // ------------------------------------------------------------------
        // rail footer — attach status + credit
        // ------------------------------------------------------------------
        {
            const bool osu_found = input::target_window( ) && IsWindow( input::target_window( ) );
            dl->AddLine( S( 10.f, H - 52.f ), S( SB_W - 10.f, H - 52.f ), theme::fade( IM_COL32( 32, 31, 39, 255 ) ), 1.f );

            ImFont* sf = theme::font_small;
            const char* att = osu_found ? "attached" : "searching";
            const ImVec2 ats = sf->CalcTextSizeA( sf->LegacySize, FLT_MAX, 0.f, att );
            const float ax = wpos.x + ( SB_W - ats.x - 9.f ) * 0.5f;
            const float ay = wpos.y + H - 42.f;
            dl->AddCircleFilled( ImVec2( ax + 2.5f, ay + ats.y * 0.5f ),
                2.5f, theme::fade( osu_found ? theme::good() : theme::text_faint() ), 10 );
            dl->AddText( sf, sf->LegacySize, ImVec2( ax + 9.f, snap_text_y( ay ) ),
                theme::fade( osu_found ? theme::text_dim() : theme::text_faint() ), att );

            const char* credit = "based on lame";
            const ImVec2 cs = sf->CalcTextSizeA( sf->LegacySize, FLT_MAX, 0.f, credit );
            dl->AddText( sf, sf->LegacySize,
                ImVec2( wpos.x + ( SB_W - cs.x ) * 0.5f, snap_text_y( wpos.y + H - 24.f ) ),
                theme::fade( theme::text_faint() ), credit );
        }

        // ------------------------------------------------------------------
        // header — page title, subtitle, animated underline, close
        // ------------------------------------------------------------------
        static const char* page_titles[ ] = { "Aim Assist", "Relax", "Tap Assist", "Replay", "Autobot", "System", "Config" };
        static const char* page_subs[ ] = {
            "cursor correction & adaptive tuning",
            "automatic tapping & timing shape",
            "keypress timing correction",
            "replay playback & parsing",
            "autonomous gameplay & motion",
            "bindings, client & diagnostics",
            "profiles & persistence" };

        {
            dl->AddRectFilled( S( SB_W + 1.f, 2.f ), S( W, HDR_H ), theme::fade( theme::header_bg() ) );
            dl->AddLine( S( SB_W + 1.f, HDR_H ), S( W, HDR_H ), theme::fade( theme::border() ), 1.f );

            ImFont* tf = theme::font_bold;
            ImFont* sf = theme::font_small;
            const float tx = SB_W + 16.f;
            dl->AddText( tf, tf->LegacySize, S( tx, 5.f ), theme::fade( theme::text_bright() ), page_titles[ m_tab ] );
            dl->AddText( sf, sf->LegacySize, S( tx + 1.f, 23.f ), theme::fade( theme::text_dim() ), page_subs[ m_tab ] );

            const float title_w = tf->CalcTextSizeA( tf->LegacySize, FLT_MAX, 0.f, page_titles[ m_tab ] ).x;
            const float uw = ( title_w + 18.f ) * page_entrance( );
            dl->AddRectFilled( S( tx, HDR_H - 2.f ), S( tx + uw, HDR_H ), theme::fade( theme::accent() ) );

            const char* build = "lamev2 \xc2\xb7 private";
            const float build_w = sf->CalcTextSizeA( sf->LegacySize, FLT_MAX, 0.f, build ).x;
            dl->AddText( sf, sf->LegacySize, S( W - build_w - 42.f, 13.f ), theme::fade( theme::text_faint() ), build );

            // drag region (excludes close button)
            ImGui::SetCursorPos( ImVec2( SB_W + 1.f, 2.f ) );
            ImGui::InvisibleButton( "##titlebar", ImVec2( W - SB_W - 44.f, HDR_H - 2.f ) );
            if ( ImGui::IsItemActive( ) ) {
                m_menu_offset_x += static_cast<int>( io.MouseDelta.x );
                m_menu_offset_y += static_cast<int>( io.MouseDelta.y );
            }

            // close button — same close request as the menu key, nothing more
            ImGui::SetCursorPos( ImVec2( W - 34.f, 6.f ) );
            ImGui::InvisibleButton( "##close", ImVec2( 26.f, 26.f ) );
            const bool xhov = ImGui::IsItemHovered( );
            const float xt = anim( ImGui::GetID( "##close_anim" ), xhov ? 1.f : 0.f, 13.f, 0.f );
            const ImVec2 xc = S( W - 21.f, 19.f );
            if ( xt > 0.01f )
                dl->AddRectFilled( ImVec2( xc.x - 11.f, xc.y - 11.f ), ImVec2( xc.x + 11.f, xc.y + 11.f ),
                    theme::fade( theme::accent_alpha( 0.14f * xt ) ), 3.f );
            const ImU32 xcol = theme::fade( theme::lerp_color( theme::text_dim(), theme::text_bright(), xt ) );
            dl->AddLine( ImVec2( xc.x - 4.f, xc.y - 4.f ), ImVec2( xc.x + 4.f, xc.y + 4.f ), xcol, 1.4f );
            dl->AddLine( ImVec2( xc.x - 4.f, xc.y + 4.f ), ImVec2( xc.x + 4.f, xc.y - 4.f ), xcol, 1.4f );
            if ( ImGui::IsItemClicked( ImGuiMouseButton_Left ) ) {
                // X exits the whole application (not just hide the menu).
                m_exit_requested = true;
                close_transient_ui( );
            }
        }

        // ------------------------------------------------------------------
        // page content — fades/slides in on tab switch
        // ------------------------------------------------------------------
        const float pe = page_entrance( );
        theme::content_mul = pe;
        const float slide = ( 1.f - pe ) * 8.f;

        float replay_banner_h = 0.f;
        if ( snap.game.is_replay ) {
            const float by0 = HDR_H + 10.f;
            dl->AddRectFilled( S( L_X, by0 ), S( R_X + COL_W, by0 + 28.f ),
                theme::fade( IM_COL32( 82, 26, 30, 200 ) ), 3.f );
            dl->AddRect( S( L_X, by0 ), S( R_X + COL_W, by0 + 28.f ),
                theme::fade( IM_COL32( 150, 60, 60, 160 ) ), 3.f, 0, 1.f );
            dl->AddText( theme::font_small, theme::font_small->LegacySize, S( L_X + 12.f, by0 + 7.f ),
                theme::fade( theme::bad() ), "replay detected - input modules paused" );
            replay_banner_h = 36.f;
        }

        const float top0 = HDR_H + 12.f + slide + replay_banner_h;

        // window-relative content helpers
        auto sect = [&]( float x, float& lyy, const char* t ) {
            float sy = wpos.y + lyy;
            section_label( dl, wpos.x + x, sy, t );
            lyy = sy - wpos.y;
        };
        auto kv = [&]( float x, float& lyy, float w, const char* label, const char* val, ImU32 c = 0 ) {
            float sy = wpos.y + lyy;
            kv_row( dl, wpos.x + x, sy, w, label, val, c );
            lyy = sy - wpos.y;
        };
        auto fold = [&]( float x, float& lyy, float w, const char* label, bool def_open = false ) {
            float sy = wpos.y + lyy;
            const bool open = collapsible( dl, wpos.x + x, sy, w, label, def_open );
            lyy = sy - wpos.y;
            return open;
        };

        // ==================================================================
        if ( m_tab == 0 ) {  // AIM ASSIST
            float ly = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Enable aim assist", &m_aim.enabled, nullptr, nullptr, CW );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Ignore sliders", &m_aim.ignore_sliders, nullptr, nullptr, CW );
            tip( "Skip correction while a slider is being followed." );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Tablet mode", &m_aim.tablet_mode, nullptr, nullptr, CW );
            tip( "Adjusts correction for absolute (tablet) input." );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "CORRECTION" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Assist Strength", &m_aim.assist_strength, 5.0f, 100.0f, " %", "%.0f", CW );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Assist Radius", &m_aim.assist_radius, 1.1f, 4.0f, " x radius", "%.2f", CW );
            tip( "How far around a hit circle the assist engages." );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Smoothing", &m_aim.smoothing_ms, 35.f, 180.f, " ms", "%.0f", CW );
            tip( "Higher = softer, more human-looking correction." );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Maximum Correction", &m_aim.max_correction, 0.10f, 1.75f, " x radius", "%.2f", CW );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "ADAPTIVE" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Adaptive Aim", &m_aim.adaptive_aim, nullptr, nullptr, CW );
            tip( "Scales strength & smoothing with live map difficulty." );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            if ( m_aim.adaptive_aim ) {
                ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
                slider_float( "Adaptation Strength", &m_aim.adaptation_strength, 0.f, 100.f, " %", "%.0f", CW );
                ly = ImGui::GetCursorPos( ).y + 2.f;
            }

            const float lbot = ly + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( L_X, top0 ), S( L_X + COL_W, lbot ), "AIM ASSIST" );
            dl->ChannelsMerge( );

            // ---------- right: diagnostics ----------
            float ry = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            const aim_assist::aim_verification_t verify = m_aim.verification( );
            const double report_count = static_cast<double>( verify.aim_reports );
            const double target_count = static_cast<double>( verify.target_bearing_reports );
            const double relevant_count = static_cast<double>( verify.relevant_approach_reports );
            const double target_percent = report_count > 0.0 ? target_count * 100.0 / report_count : 0.0;
            const double relevant_percent = target_count > 0.0 ? relevant_count * 100.0 / target_count : 0.0;
            const double engaged_percent = relevant_count > 0.0
                ? static_cast<double>( verify.engaged_reports ) * 100.0 / relevant_count : 0.0;
            const double corrected_percent = target_count > 0.0
                ? static_cast<double>( verify.corrected_reports ) * 100.0 / target_count : 0.0;
            const double non_zero_percent = report_count > 0.0
                ? static_cast<double>( verify.non_zero_corrections ) * 100.0 / report_count : 0.0;
            const double average_requested_correction = verify.requested_non_zero_samples > 0
                ? verify.requested_correction_sum / static_cast<double>( verify.requested_non_zero_samples ) : 0.0;
            const double average_relevant_predicted_miss = verify.relevant_predicted_miss_samples > 0
                ? verify.relevant_predicted_miss_sum / static_cast<double>( verify.relevant_predicted_miss_samples ) : 0.0;
            const double average_corrected_predicted_miss = verify.corrected_predicted_miss_samples > 0
                ? verify.corrected_predicted_miss_sum / static_cast<double>( verify.corrected_predicted_miss_samples ) : 0.0;
            const double average_corrected_observed = verify.corrected_observed_samples > 0
                ? verify.corrected_observed_sum / static_cast<double>( verify.corrected_observed_samples ) : 0.0;
            const double observed_requested_ratio = verify.requested_correction_sum > 0.0
                ? verify.corrected_observed_sum * 100.0 / verify.requested_correction_sum : 0.0;
            const double average_low_medium_correction = verify.low_medium_correction_samples > 0
                ? verify.low_medium_correction_sum / static_cast<double>( verify.low_medium_correction_samples ) : 0.0;
            const double average_high_extreme_correction = verify.high_extreme_correction_samples > 0
                ? verify.high_extreme_correction_sum / static_cast<double>( verify.high_extreme_correction_samples ) : 0.0;
            const double average_first_rescue_miss = verify.first_rescue_miss_samples > 0
                ? verify.first_rescue_miss_sum / static_cast<double>( verify.first_rescue_miss_samples ) : 0.0;
            const double average_final_correction_miss = verify.final_correction_miss_samples > 0
                ? verify.final_correction_miss_sum / static_cast<double>( verify.final_correction_miss_samples ) : 0.0;
            const double adaptive_seconds = verify.adaptive_sample_seconds;
            const double average_adaptive_difficulty = adaptive_seconds > 0.0
                ? verify.adaptive_difficulty_time_sum / adaptive_seconds : 0.0;
            const double average_effective_strength = adaptive_seconds > 0.0
                ? verify.effective_strength_time_sum / adaptive_seconds : 0.0;
            const double average_effective_smoothing = adaptive_seconds > 0.0
                ? verify.effective_smoothing_time_sum / adaptive_seconds : 0.0;
            const double average_effective_max_correction = adaptive_seconds > 0.0
                ? verify.effective_max_correction_time_sum / adaptive_seconds : 0.0;
            double difficulty_bucket_percent[ 4 ]{};
            if ( adaptive_seconds > 0.0 ) {
                for ( size_t bi = 0; bi < 4; ++bi )
                    difficulty_bucket_percent[ bi ] = verify.difficulty_bucket_seconds[ bi ] * 100.0 / adaptive_seconds;
            }

            char vb[ 128 ]{};
            const ImU32 zero_dim = theme::text_faint();

            sect( R_X + 14.f, ry, "SESSION" );
            sprintf_s( vb, "%llu", static_cast<unsigned long long>( verify.aim_reports ) );
            kv( R_X + 14.f, ry, CW, "Input reports", vb, verify.aim_reports ? 0 : zero_dim );
            sprintf_s( vb, "%llu  (%.1f%%)", static_cast<unsigned long long>( verify.corrected_reports ), corrected_percent );
            kv( R_X + 14.f, ry, CW, "Corrected", vb, verify.corrected_reports ? 0 : zero_dim );
            sprintf_s( vb, "%llu  (%.1f%%)", static_cast<unsigned long long>( verify.non_zero_corrections ), non_zero_percent );
            kv( R_X + 14.f, ry, CW, "Non-zero output", vb, verify.non_zero_corrections ? 0 : zero_dim );
            sprintf_s( vb, "%.2f / %.2f px", average_requested_correction, average_corrected_observed );
            kv( R_X + 14.f, ry, CW, "Avg requested / observed", vb );
            sprintf_s( vb, "%.1f%%", observed_requested_ratio );
            kv( R_X + 14.f, ry, CW, "Observed / requested", vb );
            ry += 4.f;

            if ( fold( R_X + 14.f, ry, CW, "DETAILED TELEMETRY" ) ) {
                sprintf_s( vb, "%llu  (%.1f%%)", static_cast<unsigned long long>( verify.target_bearing_reports ), target_percent );
                kv( R_X + 14.f, ry, CW, "Target-bearing", vb );
                sprintf_s( vb, "%llu  (%.1f%%)", static_cast<unsigned long long>( verify.relevant_approach_reports ), relevant_percent );
                kv( R_X + 14.f, ry, CW, "Relevant", vb );
                sprintf_s( vb, "%llu  (%.1f%%)", static_cast<unsigned long long>( verify.engaged_reports ), engaged_percent );
                kv( R_X + 14.f, ry, CW, "Engaged", vb );
                sprintf_s( vb, "%.2f / %.2f px", verify.peak_requested_correction, verify.peak_observed_output_delta );
                kv( R_X + 14.f, ry, CW, "Peak requested / observed", vb );
                sprintf_s( vb, "%llu", static_cast<unsigned long long>( verify.max_correction_clamp_hits ) );
                kv( R_X + 14.f, ry, CW, "Max-correction clamps", vb, verify.max_correction_clamp_hits ? theme::warn() : zero_dim );
                sprintf_s( vb, "%llu / %llu",
                    static_cast<unsigned long long>( verify.rejected_safe_trajectory ),
                    static_cast<unsigned long long>( verify.rejected_distance ) );
                kv( R_X + 14.f, ry, CW, "Reject safe / distance", vb );
                sprintf_s( vb, "%llu / %llu",
                    static_cast<unsigned long long>( verify.rejected_direction ),
                    static_cast<unsigned long long>( verify.rejected_timing ) );
                kv( R_X + 14.f, ry, CW, "Reject direction / timing", vb );
                sprintf_s( vb, "%.1f / %.1f px", average_relevant_predicted_miss, average_corrected_predicted_miss );
                kv( R_X + 14.f, ry, CW, "Pred. miss rel / corr", vb );
                sprintf_s( vb, "%.1f px", verify.peak_corrected_predicted_miss );
                kv( R_X + 14.f, ry, CW, "Pred. miss peak", vb );
                sprintf_s( vb, "%.2f", verify.adaptive_difficulty );
                kv( R_X + 14.f, ry, CW, "Difficulty now", vb );
                sprintf_s( vb, "%.0f%% / %.0fms / %.2fx",
                    verify.effective_strength, verify.effective_smoothing_ms, verify.effective_max_correction );
                kv( R_X + 14.f, ry, CW, "Now S / Sm / Max", vb );
                sprintf_s( vb, "%.2f / %.2f", average_adaptive_difficulty, verify.adaptive_difficulty_peak );
                kv( R_X + 14.f, ry, CW, "Difficulty avg / peak", vb );
                sprintf_s( vb, "%.0f%% / %.0f%%", difficulty_bucket_percent[ 0 ], difficulty_bucket_percent[ 1 ] );
                kv( R_X + 14.f, ry, CW, "Time low / medium", vb );
                sprintf_s( vb, "%.0f%% / %.0f%%", difficulty_bucket_percent[ 2 ], difficulty_bucket_percent[ 3 ] );
                kv( R_X + 14.f, ry, CW, "Time high / extreme", vb );
                sprintf_s( vb, "%.0f / %.0f / %.0f%%",
                    verify.effective_strength_min, average_effective_strength, verify.effective_strength_max );
                kv( R_X + 14.f, ry, CW, "Strength min/avg/max", vb );
                sprintf_s( vb, "%.0f / %.0f / %.0f ms",
                    verify.effective_smoothing_min, average_effective_smoothing, verify.effective_smoothing_max );
                kv( R_X + 14.f, ry, CW, "Smoothing min/avg/max", vb );
                sprintf_s( vb, "%.2f / %.2f / %.2fx",
                    verify.effective_max_correction_min, average_effective_max_correction, verify.effective_max_correction_max );
                kv( R_X + 14.f, ry, CW, "Max corr min/avg/max", vb );
                sprintf_s( vb, "%.0f / %.0f ms", verify.effective_anticipation_min, verify.effective_anticipation_max );
                kv( R_X + 14.f, ry, CW, "Anticipation min / max", vb );
                sprintf_s( vb, "%llu", static_cast<unsigned long long>( verify.adaptive_hard_pattern_activations ) );
                kv( R_X + 14.f, ry, CW, "Hard-pattern activations", vb, verify.adaptive_hard_pattern_activations ? 0 : zero_dim );
                sprintf_s( vb, "%llu/%llu/%llu/%llu",
                    static_cast<unsigned long long>( verify.corrected_by_difficulty[ 0 ] ),
                    static_cast<unsigned long long>( verify.corrected_by_difficulty[ 1 ] ),
                    static_cast<unsigned long long>( verify.corrected_by_difficulty[ 2 ] ),
                    static_cast<unsigned long long>( verify.corrected_by_difficulty[ 3 ] ) );
                kv( R_X + 14.f, ry, CW, "Corrected L/M/H/X", vb );
                sprintf_s( vb, "%llu / %llu",
                    static_cast<unsigned long long>( verify.high_difficulty_rescue_activations ),
                    static_cast<unsigned long long>( verify.large_jump_rescue_activations ) );
                kv( R_X + 14.f, ry, CW, "Rescues high / large", vb );
                sprintf_s( vb, "%.2f / %.2f px", average_low_medium_correction, average_high_extreme_correction );
                kv( R_X + 14.f, ry, CW, "Avg corr L-M / H-X", vb );
                sprintf_s( vb, "%.1f / %.1f px", average_first_rescue_miss, average_final_correction_miss );
                kv( R_X + 14.f, ry, CW, "Miss rescue / final", vb );
            }

            const float rbot = ry + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( R_X, top0 ), S( R_X + COL_W, rbot ), "DIAGNOSTICS" );
            dl->ChannelsMerge( );
        }
        // ==================================================================
        else if ( m_tab == 1 ) {  // RELAX
            float ly = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Enable relax", &m_relax.enabled, nullptr, nullptr, CW );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "GENERAL TIMING" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_int( "Base Offset", &m_relax.manual_offset_ms, -100, 100, " ms", CW );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Timing Consistency", &m_relax.ur, 0.0f, 300.0f, " UR", "%.0f", CW );
            tip( "Unstable-rate target. Lower = more consistent taps." );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "VARIATION" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Timing Variation", &m_relax.timing_variation, nullptr, nullptr, CW );
            tip( "Adds slow human-like drift to tap timing." );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            if ( m_relax.timing_variation ) {
                ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
                slider_int( "Early Variation", &m_relax.early_variation_ms, -25, 0, " ms", CW );
                ly = ImGui::GetCursorPos( ).y + 2.f;
                ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
                slider_int( "Late Variation", &m_relax.late_variation_ms, 0, 25, " ms", CW );
                ly = ImGui::GetCursorPos( ).y + 2.f;
                ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
                slider_int( "Timing Drift", &m_relax.timing_drift_ms, 0, 8, " ms", CW );
                ly = ImGui::GetCursorPos( ).y + 2.f;

                char effective_range[ 64 ]{};
                sprintf_s( effective_range, "effective range  %+d ms  to  %+d ms",
                    m_relax.manual_offset_ms + m_relax.early_variation_ms,
                    m_relax.manual_offset_ms + m_relax.late_variation_ms );
                dl->AddText( theme::font_small, theme::font_small->LegacySize, S( L_X + 14.f, ly ),
                    theme::fade( theme::text_dim() ), effective_range );
                ly += theme::font_small->LegacySize + 8.f;
            }
            else {
                ly += 4.f;
            }

            sect( L_X + 14.f, ly, "ADVANCED SINGLETAP" );
            { bool is_singletap = ( m_relax.tap_style == 1 );
              ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
              checkbox( "Singletap Mode", &is_singletap, nullptr, nullptr, CW );
              m_relax.tap_style = is_singletap ? 1 : 0; }
            ly = ImGui::GetCursorPos( ).y + 2.f;

            if ( m_relax.tap_style == 1 ) {
                static const char* primary_keys[ ] = { "K1", "K2" };
                static const char* burst_levels[ ] = { "Low", "Medium", "High" };

                dl->AddText( S( L_X + 14.f, ly ), theme::fade( theme::text() ), "Primary Key" );
                ly += ImGui::GetTextLineHeight( ) + 4.f;
                ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
                dropdown( "##relax_primary_key", &m_relax.primary_key, primary_keys, 2, CW );
                ly = ImGui::GetCursorPos( ).y + 2.f;

                ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
                slider_int( "Singletap Speed", &m_relax.singletap_speed_bpm, 100, 400, " bpm", CW );
                ly = ImGui::GetCursorPos( ).y + 2.f;

                dl->AddText( S( L_X + 14.f, ly ), theme::fade( theme::text() ), "Burst Tolerance" );
                ly += ImGui::GetTextLineHeight( ) + 4.f;
                ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
                dropdown( "##relax_burst_tolerance", &m_relax.burst_tolerance, burst_levels, 3, CW );
                ly = ImGui::GetCursorPos( ).y + 2.f;

                ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
                slider_float( "Stamina", &m_relax.stamina, 0.0f, 100.0f, " %", "%.0f", CW );
                tip( "Simulates fatigue: long streams gradually loosen timing." );
                ly = ImGui::GetCursorPos( ).y;
            }

            const float lbot = ly + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( L_X, top0 ), S( L_X + COL_W, lbot ), "RELAX" );
            dl->ChannelsMerge( );

            // ---------- right: hold behavior + status ----------
            float ry = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            sect( R_X + 14.f, ry, "K1 HOLD SHAPE" );
            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            slider_float( "K1 Center", &m_relax.k1_hold_center, 30.f, 120.f, " ms", "%.0f", CW );
            ry = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            slider_float( "K1 Spread", &m_relax.k1_hold_spread, 2.f, 30.f, " ms", "%.0f", CW );
            ry = ImGui::GetCursorPos( ).y + 8.f;

            sect( R_X + 14.f, ry, "K2 HOLD SHAPE" );
            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            slider_float( "K2 Center", &m_relax.k2_hold_center, 30.f, 120.f, " ms", "%.0f", CW );
            ry = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            slider_float( "K2 Spread", &m_relax.k2_hold_spread, 2.f, 30.f, " ms", "%.0f", CW );
            ry = ImGui::GetCursorPos( ).y + 8.f;

            sect( R_X + 14.f, ry, "LIMITS" );
            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            slider_float( "Hold floor", &m_relax.hold_floor, 10.f, 60.f, " ms", "%.0f", CW );
            ry = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            slider_float( "Hold ceiling", &m_relax.hold_ceiling, 60.f, 150.f, " ms", "%.0f", CW );
            ry = ImGui::GetCursorPos( ).y + 8.f;

            sect( R_X + 14.f, ry, "STATUS" );
            if ( m_relax.is_active( ) )
                kv( R_X + 14.f, ry, CW, "State", "running", theme::good() );
            else if ( m_relax.is_synced( ) && m_relax.enabled )
                kv( R_X + 14.f, ry, CW, "State", "synced", theme::warn() );
            else
                kv( R_X + 14.f, ry, CW, "State", "idle", theme::text_faint() );

            if ( m_relax.enabled ) {
                char buf[ 96 ];
                sprintf_s( buf, "%s", snap.beatmap.loaded ? "loaded" : "not loaded" );
                kv( R_X + 14.f, ry, CW, "Beatmap", buf, snap.beatmap.loaded ? 0 : theme::bad() );
                sprintf_s( buf, "%zu / %zu", snap.beatmap.objects.size( ), m_relax.queue_size( ) );
                kv( R_X + 14.f, ry, CW, "Objects / queue", buf );
                if ( snap.beatmap.loaded ) {
                    sprintf_s( buf, "%d / %zu", m_relax.last_hit_obj_idx( ), snap.beatmap.objects.size( ) );
                    kv( R_X + 14.f, ry, CW, "Hit object", buf );
                }
            }

            const float rbot = ry + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( R_X, top0 ), S( R_X + COL_W, rbot ), "HOLD BEHAVIOR" );
            dl->ChannelsMerge( );
        }
        // ==================================================================
        else if ( m_tab == 2 ) {  // TAP ASSIST
            float ly = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Enable tap assist", &m_tap_assist.enabled, nullptr, nullptr, CW );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Ignore sliders", &m_tap_assist.ignore_sliders, nullptr, nullptr, CW );
            ly = ImGui::GetCursorPos( ).y;

            const float lbot = ly + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( L_X, top0 ), S( L_X + COL_W, lbot ), "TAP ASSIST" );
            dl->ChannelsMerge( );

            float ry = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            slider_int( "Assist Window", &m_tap_assist.assist_window, 0, 250, " ms", CW );
            tip( "How far off a keypress can be and still get corrected." );
            ry = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            slider_int( "Randomization", &m_tap_assist.randomization, 0, 40, " ms", CW );
            tip( "Random jitter added to corrected presses." );
            ry = ImGui::GetCursorPos( ).y;

            const float rbot = ry + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( R_X, top0 ), S( R_X + COL_W, rbot ), "TUNING" );
            dl->ChannelsMerge( );
        }
        // ==================================================================
        else if ( m_tab == 3 ) {  // REPLAY
            float ly = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Enable replay bot", &m_replay.enabled, nullptr, nullptr, CW );
            if ( ImGui::IsItemClicked( ) && m_replay.enabled ) m_replay.reset_sync( );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "REPLAY FILE" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            text_input( "##replay_path", m_replay_path_utf8, IM_ARRAYSIZE( m_replay_path_utf8 ), CW );
            ly = ImGui::GetCursorPos( ).y + 6.f;

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            if ( button( "Browse", CW, 24.0f ) ) {
                OPENFILENAMEW ofn{};
                wchar_t file[ 512 ]{};
                ofn.lStructSize = sizeof( ofn );
                ofn.hwndOwner = m_hwnd;
                ofn.lpstrFilter = L"Replay Files\0*.osr\0All\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = 512;
                ofn.Flags = OFN_FILEMUSTEXIST;
                if ( GetOpenFileNameW( &ofn ) ) {
                    WideCharToMultiByte( CP_UTF8, 0, file, -1, m_replay_path_utf8, IM_ARRAYSIZE( m_replay_path_utf8 ), nullptr, nullptr );
                    m_replay.replay_path.assign( file );
                    m_replay.load_replay( );
                    m_replay.reset_sync( );
                }
            }
            ly = ImGui::GetCursorPos( ).y + 4.f;

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            if ( button( "Load Replay", CW, 24.0f ) ) {
                wchar_t wide[ 512 ]{};
                MultiByteToWideChar( CP_UTF8, 0, m_replay_path_utf8, -1, wide, 512 );
                m_replay.replay_path = wide;
                m_replay.load_replay( );
                m_replay.reset_sync( );
            }
            ly = ImGui::GetCursorPos( ).y + 0.f;

            const float lbot = ly + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( L_X, top0 ), S( L_X + COL_W, lbot ), "REPLAY" );
            dl->ChannelsMerge( );

            float ry = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            checkbox( "Parse buttons", &m_replay.parse_buttons, nullptr, nullptr, CW );
            tip( "Replay key presses too, not just cursor movement." );
            ry = ImGui::GetCursorPos( ).y + 8.f;

            sect( R_X + 14.f, ry, "STATUS" );
            char buf[ 96 ];
            sprintf_s( buf, "%zu", m_replay.frame_count( ) );
            kv( R_X + 14.f, ry, CW, "Frames", buf, m_replay.frame_count( ) ? 0 : theme::text_faint() );
            kv( R_X + 14.f, ry, CW, "Valid", m_replay.replay_valid( ) ? "yes" : "no",
                m_replay.replay_valid( ) ? theme::good() : theme::bad() );

            if ( !m_replay.last_load_error( ).empty( ) ) {
                ry += 4.f;
                dl->AddText( theme::font_small, theme::font_small->LegacySize, S( R_X + 14.f, ry ),
                    theme::fade( theme::bad() ), m_replay.last_load_error( ).c_str( ) );
                ry += theme::font_small->LegacySize + 4.f;
            }

            const float rbot = ry + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( R_X, top0 ), S( R_X + COL_W, rbot ), "PLAYBACK" );
            dl->ChannelsMerge( );
        }
        // ==================================================================
        else if ( m_tab == 4 ) {  // AUTOBOT
            float ly = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Enable autobot", &m_autobot.enabled, nullptr, nullptr, CW );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "PLAYBACK" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Target Accuracy", &m_autobot.target_accuracy, 85.f, 100.f, " %", "%.1f", CW );
            tip( "The bot aims for this session accuracy." );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Gameplay Flow", &m_autobot.gameplay_flow, nullptr, nullptr, CW );
            tip( "Smoother, more musical motion between objects." );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "AIM MOTION" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Aim Spread", &m_autobot.aim_spread, 0.f, 1.f, "", "%.2f", CW );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Curve Strength", &m_autobot.curve_strength, 0.f, 1.f, "", "%.2f", CW );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Momentum", &m_autobot.momentum, 0.f, .95f, "", "%.2f", CW );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "IDLE MOTION" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Startup Motion", &m_autobot.startup_motion, nullptr, nullptr, CW );
            ly = ImGui::GetCursorPos( ).y + 1.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Break Motion", &m_autobot.break_motion, nullptr, nullptr, CW );
            ly = ImGui::GetCursorPos( ).y + 1.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Energetic Dances", &m_autobot.energetic_dances, nullptr, nullptr, CW );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Startup Energy", &m_autobot.startup_energy, 0.0f, 1.0f, "", "%.2f", CW );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Break Energy", &m_autobot.break_energy, 0.0f, 1.0f, "", "%.2f", CW );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "OBJECTS" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Slider Laziness", &m_autobot.slider_laziness, 0.f, 1.f, "", "%.2f", CW );
            tip( "Higher = stays near the slider ball's minimum path." );
            ly = ImGui::GetCursorPos( ).y + 2.f;
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            slider_float( "Spinner RPM", &m_autobot.spinner_rpm, 200.f, 477.f, " rpm", "%.0f", CW );
            ly = ImGui::GetCursorPos( ).y;

            const float lbot = ly + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( L_X, top0 ), S( L_X + COL_W, lbot ), "AUTOBOT" );
            dl->ChannelsMerge( );

            // ---------- right: diagnostics ----------
            float ry = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            const bool bot_running = m_autobot.enabled && snap.game.cur_state == osu::game_state_t::play;
            kv( R_X + 14.f, ry, CW, "State", bot_running ? "running" : "idle",
                bot_running ? theme::good() : theme::text_faint() );

            const auto ad = m_autobot.diagnostics( );
            char abuf[ 160 ]{};
            sect( R_X + 14.f, ry, "TARGET ACCURACY" );
            sprintf_s( abuf, "%.1f%%", ad.requested_accuracy );
            kv( R_X + 14.f, ry, CW, "Target", abuf, theme::text_bright() );
            sprintf_s( abuf, "%.2f%%", ad.predicted_accuracy );
            const float acc_err = std::abs( ad.predicted_accuracy - ad.requested_accuracy );
            kv( R_X + 14.f, ry, CW, "Projected final", abuf,
                acc_err <= 0.75f ? theme::good() : acc_err <= 1.5f ? theme::warn() : theme::bad() );
            sprintf_s( abuf, "%llu", static_cast<unsigned long long>( ad.controlled_100 ) );
            kv( R_X + 14.f, ry, CW, "Controlled 100s", abuf );
            sprintf_s( abuf, "%+.2f", ad.accuracy_debt );
            kv( R_X + 14.f, ry, CW, "Outcome debt", abuf,
                std::abs( ad.accuracy_debt ) < 1.2f ? theme::text_faint() : theme::warn() );
            ry += 6.f;

            // ---- active plan (the single authoritative movement intent) ----
            sect( R_X + 14.f, ry, "ACTIVE PLAN" );
            sprintf_s( abuf, "%s  #%llu", autobot::plan_type_name( ad.plan_type ),
                static_cast<unsigned long long>( ad.plan_id ) );
            kv( R_X + 14.f, ry, CW, "Plan / id", abuf, theme::text_bright() );
            sprintf_s( abuf, "%d", ad.object_index );
            kv( R_X + 14.f, ry, CW, "Object index", abuf );
            sprintf_s( abuf, "%.0f%%   %.0f ms", ad.plan_progress * 100.f, ad.time_to_arrival_ms );
            kv( R_X + 14.f, ry, CW, "Progress / to arrival", abuf );
            sprintf_s( abuf, "%.0f, %.0f", ad.motion_position.x, ad.motion_position.y );
            kv( R_X + 14.f, ry, CW, "Motion position", abuf );
            sprintf_s( abuf, "%.0f, %.0f", ad.target_position.x, ad.target_position.y );
            kv( R_X + 14.f, ry, CW, "Target position", abuf );
            sprintf_s( abuf, "%.0f px/s   %.0f px/s2", ad.speed, ad.acceleration );
            kv( R_X + 14.f, ry, CW, "Speed / accel", abuf );
            sprintf_s( abuf, "%d,%d  vs  %d,%d", ad.requested_screen_x, ad.requested_screen_y,
                ad.observed_screen_x, ad.observed_screen_y );
            kv( R_X + 14.f, ry, CW, "Requested / observed", abuf );
            ry += 6.f;

            // ---- integrity: exactly one invariant matters -> discontinuities ----
            sect( R_X + 14.f, ry, "MOVEMENT INTEGRITY" );
            sprintf_s( abuf, "%llu", static_cast<unsigned long long>( ad.unexpected_discontinuities ) );
            kv( R_X + 14.f, ry, CW, "Discontinuities", abuf,
                ad.unexpected_discontinuities == 0 ? theme::good() : theme::bad() );
            sprintf_s( abuf, "%.0f px", ad.max_frame_displacement );
            kv( R_X + 14.f, ry, CW, "Max frame move", abuf );
            sprintf_s( abuf, "%.2f px", ad.projection_self_check_error );
            kv( R_X + 14.f, ry, CW, "Projection error", abuf,
                ad.projection_self_check_error < 1.f ? theme::text_faint() : theme::warn() );
            // flight recorder: context of the most recent abnormal move
            autobot::flight_record_t fr{};
            if ( m_autobot.last_record( fr ) ) {
                sprintf_s( abuf, "%s  (%.0f>%.0f)", fr.reason, fr.displacement, fr.bound );
                kv( R_X + 14.f, ry, CW, "Last event", abuf, theme::warn() );
                sprintf_s( abuf, "%s obj %d  #%llu", autobot::plan_type_name( fr.plan_type ),
                    fr.object_index, static_cast<unsigned long long>( fr.plan_id ) );
                kv( R_X + 14.f, ry, CW, "  plan / object", abuf );
                sprintf_s( abuf, "t=%d  dt=%.1fms%s", fr.game_time, fr.dt * 1000.f,
                    fr.external_reanchor ? "  reanchor" : "" );
                kv( R_X + 14.f, ry, CW, "  when", abuf );
            }
            ry += 6.f;

            // ---- lifetime counters ----
            if ( fold( R_X + 14.f, ry, CW, "PLAN COUNTERS" ) ) {
                sprintf_s( abuf, "%llu", static_cast<unsigned long long>( ad.plans_created ) );
                kv( R_X + 14.f, ry, CW, "Plans created", abuf );
                sprintf_s( abuf, "%llu / %llu / %llu",
                    static_cast<unsigned long long>( ad.gameplay_plans ),
                    static_cast<unsigned long long>( ad.slider_plans ),
                    static_cast<unsigned long long>( ad.spinner_plans ) );
                kv( R_X + 14.f, ry, CW, "Gameplay/Slider/Spin", abuf );
                sprintf_s( abuf, "%llu / %llu / %llu",
                    static_cast<unsigned long long>( ad.startup_segments ),
                    static_cast<unsigned long long>( ad.break_segments ),
                    static_cast<unsigned long long>( ad.recovery_plans ) );
                kv( R_X + 14.f, ry, CW, "Startup/Break/Recover", abuf );
                sprintf_s( abuf, "%llu / %llu",
                    static_cast<unsigned long long>( ad.plan_invalidations ),
                    static_cast<unsigned long long>( ad.external_reanchors ) );
                kv( R_X + 14.f, ry, CW, "Invalidations/Reanchor", abuf );
                sprintf_s( abuf, "%llu", static_cast<unsigned long long>( ad.objects_completed ) );
                kv( R_X + 14.f, ry, CW, "Objects completed", abuf );
            }

            const float rbot = ry + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( R_X, top0 ), S( R_X + COL_W, rbot ), "DIAGNOSTICS" );
            dl->ChannelsMerge( );
        }
        // ==================================================================
        else if ( m_tab == 5 ) {  // SYSTEM
            float ly = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            char left_buf[ 16 ]{}, right_buf[ 16 ]{}, menu_buf[ 16 ]{};
            GetKeyNameTextA( MapVirtualKeyA( m_custom_left_key, MAPVK_VK_TO_VSC ) << 16, left_buf, sizeof( left_buf ) );
            GetKeyNameTextA( MapVirtualKeyA( m_custom_right_key, MAPVK_VK_TO_VSC ) << 16, right_buf, sizeof( right_buf ) );
            if ( m_menu_keybind == VK_DELETE )
                strcpy_s( menu_buf, "DEL" );
            else
                GetKeyNameTextA( MapVirtualKeyA( m_menu_keybind, MAPVK_VK_TO_VSC ) << 16, menu_buf, sizeof( menu_buf ) );
            if ( !left_buf[ 0 ] ) {
                if ( m_custom_left_key >= 32 && m_custom_left_key <= 126 ) sprintf_s( left_buf, "%c", m_custom_left_key );
                else sprintf_s( left_buf, "0x%02X", m_custom_left_key );
            }
            if ( !right_buf[ 0 ] ) {
                if ( m_custom_right_key >= 32 && m_custom_right_key <= 126 ) sprintf_s( right_buf, "%c", m_custom_right_key );
                else sprintf_s( right_buf, "0x%02X", m_custom_right_key );
            }
            if ( !menu_buf[ 0 ] ) {
                if ( m_menu_keybind >= 32 && m_menu_keybind <= 126 ) sprintf_s( menu_buf, "%c", m_menu_keybind );
                else sprintf_s( menu_buf, "0x%02X", m_menu_keybind );
            }

            sect( L_X + 14.f, ly, "KEYBINDS" );

            auto capture_key = [&]( int& slot, bool& waiting_flag, bool is_menu_key ) {
                for ( int k = 8; k < 256; ++k ) {
                    if ( k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON ) continue;
                    if ( GetAsyncKeyState( k ) & 0x8000 ) {
                        slot = k;
                        waiting_flag = false;
                        // swallow this press so the freshly bound key can't
                        // instantly toggle the menu on the next frame
                        if ( is_menu_key ) m_menu_key_was_down = true;
                        if ( !is_menu_key && m_relax.is_active( ) ) {
                            osu::game_snapshot_t mod = snap.game;
                            apply_custom_keys( mod );
                            m_relax.on_leave_play( mod );
                        }
                        break;
                    }
                }
            };

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            if ( key_pill( "##bind_left", "Left key", left_buf, m_waiting_left, CW ) ) {
                m_waiting_left = !m_waiting_left; m_waiting_right = false; m_waiting_menu = false;
            }
            if ( m_waiting_left ) capture_key( m_custom_left_key, m_waiting_left, false );
            ly = ImGui::GetCursorPos( ).y;

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            if ( key_pill( "##bind_right", "Right key", right_buf, m_waiting_right, CW ) ) {
                m_waiting_right = !m_waiting_right; m_waiting_left = false; m_waiting_menu = false;
            }
            if ( m_waiting_right ) capture_key( m_custom_right_key, m_waiting_right, false );
            ly = ImGui::GetCursorPos( ).y;

            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            if ( key_pill( "##bind_menu", "Menu toggle", menu_buf, m_waiting_menu, CW ) ) {
                m_waiting_menu = !m_waiting_menu; m_waiting_left = false; m_waiting_right = false;
            }
            if ( m_waiting_menu ) capture_key( m_menu_keybind, m_waiting_menu, true );
            ly = ImGui::GetCursorPos( ).y + 8.f;

            sect( L_X + 14.f, ly, "OPTIONS" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            checkbox( "Stream proof", &stream_proof, nullptr, nullptr, CW );
            tip( "Hides the overlay from screen capture and recordings." );
            ly = ImGui::GetCursorPos( ).y + 4.f;

            const float lbot = ly + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( L_X, top0 ), S( L_X + COL_W, lbot ), "BINDINGS" );
            dl->ChannelsMerge( );

            // ---------- right: client status ----------
            float ry = top0 + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            const char* client = "none";
            if ( snap.game.client == osu::client_kind_t::stable ) client = "osu!stable";
            else if ( snap.game.client == osu::client_kind_t::lazer ) client = "osu!lazer";
            const bool osu_wnd = input::target_window( ) && IsWindow( input::target_window( ) );

            char buf[ 128 ];
            sect( R_X + 14.f, ry, "CLIENT" );
            kv( R_X + 14.f, ry, CW, "Window", osu_wnd ? "found" : "not found", osu_wnd ? theme::good() : theme::text_faint() );
            kv( R_X + 14.f, ry, CW, "Client", client, snap.game.client == osu::client_kind_t::none ? theme::text_faint() : theme::text_bright() );
            sprintf_s( buf, "%d", snap.game.pid );
            kv( R_X + 14.f, ry, CW, "Attached PID", buf, snap.game.pid ? 0 : theme::text_faint() );
            if ( snap.game.cur_state == osu::game_state_t::play ) sprintf_s( buf, "%d ms", snap.game.cur_time );
            else sprintf_s( buf, "--" );
            kv( R_X + 14.f, ry, CW, "Time", buf );
            ry += 6.f;

            sect( R_X + 14.f, ry, "INPUT PATH" );
            kv( R_X + 14.f, ry, CW, "Aim mouse hook",
                m_mouse_hook.installed( ) ? "active" : "poll fallback",
                m_mouse_hook.installed( ) ? theme::good() : theme::warn() );
            kv( R_X + 14.f, ry, CW, "Mouse input", input::using_nt_input( ) ? "win32u" : "SendInput" );
            ry += 6.f;

            sect( R_X + 14.f, ry, "BEATMAP" );
            kv( R_X + 14.f, ry, CW, "State", snap.beatmap.loaded ? "loaded" : "not loaded",
                snap.beatmap.loaded ? theme::good() : theme::text_faint() );
            sprintf_s( buf, "%zu", snap.beatmap.objects.size( ) );
            kv( R_X + 14.f, ry, CW, "Objects", buf );
            if ( snap.beatmap.loaded ) {
                sprintf_s( buf, "%.1f / %.1f / %.1f", snap.beatmap.cs, snap.beatmap.od, snap.beatmap.ar );
                kv( R_X + 14.f, ry, CW, "CS / OD / AR", buf );
            }

            if ( snap.game.client == osu::client_kind_t::stable ) {
                ry += 6.f;
                sect( R_X + 14.f, ry, "SONGS PATH OVERRIDE" );
                ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
                text_input( "##songs_override", m_songs_path_utf8, IM_ARRAYSIZE( m_songs_path_utf8 ), CW );
                ry += 28.f;

                ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
                if ( button( "Browse##songs", CW * 0.5f - 3.f, 22.0f ) ) {
                    BROWSEINFOW bi{};
                    wchar_t buffer[ MAX_PATH ]{};
                    bi.lpszTitle = L"Select osu! Songs folder";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    if ( PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW( &bi ) ) {
                        if ( SHGetPathFromIDListW( pidl, buffer ) )
                            WideCharToMultiByte( CP_UTF8, 0, buffer, -1, m_songs_path_utf8, IM_ARRAYSIZE( m_songs_path_utf8 ), nullptr, nullptr );
                        CoTaskMemFree( pidl );
                    }
                }
                ImGui::SetCursorPos( ImVec2( R_X + 14.f + CW * 0.5f + 3.f, ry ) );
                if ( button( "Apply##songs", CW * 0.5f - 3.f, 22.0f ) && m_cache && m_songs_path_utf8[ 0 ] ) {
                    wchar_t wide[ 512 ]{};
                    MultiByteToWideChar( CP_UTF8, 0, m_songs_path_utf8, -1, wide, 512 );
                    m_cache->stable_parser( ).set_songs_path( wide );
                    m_cache->invalidate_beatmap_cache( );
                }
                ry += 26.f;
            }

            const float rbot = ry + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( R_X, top0 ), S( R_X + COL_W, rbot ), "SYSTEM STATUS" );
            dl->ChannelsMerge( );
        }
        // ==================================================================
        else if ( m_tab == 6 ) {  // CONFIG
            if ( m_config_profiles.empty( ) )
                m_config_profiles = config::list_profiles( );

            const float ctop = HDR_H + 12.f + slide;
            float ly = ctop + 36.f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            sect( L_X + 14.f, ly, "SAVED PROFILES" );
            ImGui::SetCursorPos( ImVec2( L_X + 14.f, ly ) );
            const float list_h = H - ly - 92.0f;
            if ( ImGui::BeginListBox( "##cfg_list", ImVec2( CW, list_h ) ) ) {
                for ( int i = 0; i < static_cast<int>( m_config_profiles.size( ) ); ++i ) {
                    const bool selected = ( m_config_selected == i );
                    if ( ImGui::Selectable( m_config_profiles[ static_cast<size_t>( i ) ].c_str( ), selected ) ) {
                        m_config_selected = i;
                        strncpy_s( m_config_name_utf8, m_config_profiles[ static_cast<size_t>( i ) ].c_str( ), _TRUNCATE );
                    }
                }
                ImGui::EndListBox( );
            }
            ly += list_h + 10.f;

            if ( !m_config_status.empty( ) ) {
                dl->AddText( theme::font_small, theme::font_small->LegacySize, S( L_X + 14.f, ly ),
                    theme::fade( theme::text_dim() ), m_config_status.c_str( ) );
                ly += theme::font_small->LegacySize + 4.f;
            }

            const float lbot = ly + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( L_X, ctop ), S( L_X + COL_W, lbot ), "PROFILES" );
            dl->ChannelsMerge( );

            float ry = ctop + 36.f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            sect( R_X + 14.f, ry, "PROFILE NAME" );
            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            text_input( "##cfg_name", m_config_name_utf8, IM_ARRAYSIZE( m_config_name_utf8 ), CW );
            ry += 30.f;

            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            if ( button( "Save", CW * 0.5f - 3.f, 24.0f ) ) {
                const std::string name = config::sanitize_name( m_config_name_utf8 );
                if ( name.empty( ) ) m_config_status = "Enter a config name first.";
                else if ( config::save_profile( name, capture_settings( ) ) ) {
                    m_config_status = "Saved \"" + name + "\".";
                    m_config_profiles = config::list_profiles( );
                    strncpy_s( m_config_name_utf8, name.c_str( ), _TRUNCATE );
                }
                else m_config_status = "Failed to save config.";
            }
            ImGui::SetCursorPos( ImVec2( R_X + 14.f + CW * 0.5f + 3.f, ry ) );
            if ( button( "Load", CW * 0.5f - 3.f, 24.0f ) ) {
                std::string name = config::sanitize_name( m_config_name_utf8 );
                if ( name.empty( ) && m_config_selected >= 0 && m_config_selected < static_cast<int>( m_config_profiles.size( ) ) )
                    name = m_config_profiles[ static_cast<size_t>( m_config_selected ) ];
                config::settings_t loaded{};
                if ( name.empty( ) )
                    m_config_status = "Select or enter a config name.";
                else if ( config::load_profile( name, loaded ) ) {
                    apply_settings( loaded );
                    strncpy_s( m_config_name_utf8, name.c_str( ), _TRUNCATE );
                    m_config_status = "Loaded \"" + name + "\".";
                }
                else m_config_status = "Config not found.";
            }
            ry += 30.f;

            ImGui::SetCursorPos( ImVec2( R_X + 14.f, ry ) );
            if ( button( "Refresh list", CW, 22.0f ) ) {
                m_config_profiles = config::list_profiles( );
                m_config_status = "Profile list refreshed.";
            }
            ry += 34.f;

            dl->AddText( theme::font_small, theme::font_small->LegacySize, S( R_X + 14.f, ry ),
                theme::fade( theme::text_dim() ), "Saves all module settings, keys,\nreplay path and system options." );
            ry += theme::font_small->LegacySize * 2.f + 12.f;

            char path_buf[ 512 ]{};
            WideCharToMultiByte( CP_UTF8, 0, config::configs_dir( ).wstring( ).c_str( ), -1, path_buf, sizeof( path_buf ), nullptr, nullptr );
            sect( R_X + 14.f, ry, "FOLDER" );
            dl->AddText( theme::font_small, theme::font_small->LegacySize, S( R_X + 14.f, ry ),
                theme::fade( theme::text() ), path_buf );
            ry += theme::font_small->LegacySize + 4.f;

            const float rbot = ry + 12.f;
            dl->ChannelsSetCurrent( 0 );
            draw_panel( dl, S( R_X, ctop ), S( R_X + COL_W, rbot ), "MANAGE" );
            dl->ChannelsMerge( );
        }

        theme::content_mul = 1.f;

        render_open_dropdown( );
        render_open_color_picker( );
        render_tooltip( );

        ImGui::SetCursorPos( ImVec2( wsize.x, wsize.y ) );
        ImGui::Dummy( ImVec2( 1.0f, 1.0f ) );

        ImGui::PopStyleVar( ); // alpha
        ImGui::End( );
    }
}
