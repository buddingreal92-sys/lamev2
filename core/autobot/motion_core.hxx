#pragma once

// ============================================================================
//  motion_core.hxx  --  Autobot movement engine (pure, host-independent).
//
//  This header contains the WHOLE movement mathematics of the Autobot:
//
//      MotionState  +  MovementPlan  +  current time  =  next cursor position.
//
//  It intentionally depends only on <impl/struct/osu_types.hxx> and the C++
//  standard library -- there is no Windows, no cursor output, no Relax, no
//  playfield projection in here.  That makes the engine deterministic and
//  unit-testable off-line (see tests/motion_tests.cpp).  The Windows / Relax /
//  projection integration lives in autobot.hxx, which owns exactly one
//  motion_engine_t and feeds it snapshots.
//
//  Design invariants (enforced here and by the tests):
//    * There is ONE MotionState and ONE active MovementPlan.
//    * A new plan always begins from the CURRENT MotionState (p, v, a).
//    * A plan is only rebuilt on a meaningful identity change, never per frame.
//    * Trajectories are C2-continuous quintic Hermite curves with a fixed timed
//      arrival; slider-follow blends its incoming offset to zero so the first
//      follow frame never snaps; the spinner integrator starts from the actual
//      incoming angle / radius / direction.
//    * Every produced position is finite and its per-frame displacement is
//      bounded by the kinematic budget; a violation is reported, not hidden.
// ============================================================================

#include <impl/struct/osu_types.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace autobot {

    constexpr float k_pi = 3.14159265358979323846f;

    // ---- 2D vector --------------------------------------------------------
    struct point_t { float x = 0.f; float y = 0.f; };
    inline point_t operator+( point_t a, point_t b ) { return { a.x + b.x, a.y + b.y }; }
    inline point_t operator-( point_t a, point_t b ) { return { a.x - b.x, a.y - b.y }; }
    inline point_t operator*( point_t a, float s ) { return { a.x * s, a.y * s }; }
    inline point_t operator/( point_t a, float s ) { return s != 0.f ? point_t{ a.x / s, a.y / s } : point_t{}; }
    inline point_t& operator+=( point_t& a, point_t b ) { a.x += b.x; a.y += b.y; return a; }
    inline float dot( point_t a, point_t b ) { return a.x * b.x + a.y * b.y; }
    inline float length_sq( point_t a ) { return dot( a, a ); }
    inline float length( point_t a ) { return std::sqrt( length_sq( a ) ); }
    inline point_t normalized( point_t a ) { const float n = length( a ); return n > 1e-4f ? a / n : point_t{}; }
    inline point_t limit( point_t a, float m ) { const float n = length( a ); return ( n > m && n > 1e-4f ) ? a * ( m / n ) : a; }
    inline bool finite( point_t a ) { return std::isfinite( a.x ) && std::isfinite( a.y ); }
    inline float saturate( float v ) { return std::clamp( v, 0.f, 1.f ); }
    inline float smoothstep( float a, float b, float x ) {
        if ( b <= a ) return x >= b ? 1.f : 0.f;
        const float t = saturate( ( x - a ) / ( b - a ) );
        return t * t * ( 3.f - 2.f * t );
    }

    // ---- MotionState : the single authoritative cursor state --------------
    struct motion_state_t {
        point_t position{};
        point_t velocity{};
        point_t acceleration{};
        bool initialized = false;
    };

    // ---- MovementPlan : the single authoritative movement intent ----------
    //
    // Exactly one plan owns the cursor at a time.  A trajectory plan (gameplay
    // / recovery / decorative segment / slider approach) is a timed quintic
    // from (p0,v0,a0) to (p1,v1,0).  Slider-follow and spinner plans are
    // evaluated procedurally against geometry but still carry their captured
    // entry state so the transition in is continuous.
    enum class plan_type_t : uint8_t {
        none, gameplay, recovery, slider, spinner, startup, break_decor
    };

    inline const char* plan_type_name( plan_type_t t ) {
        switch ( t ) {
        case plan_type_t::gameplay:    return "GAMEPLAY";
        case plan_type_t::recovery:    return "RECOVERY";
        case plan_type_t::slider:      return "SLIDER";
        case plan_type_t::spinner:     return "SPINNER";
        case plan_type_t::startup:     return "STARTUP";
        case plan_type_t::break_decor: return "BREAK";
        default:                       return "NONE";
        }
    }

    struct plan_t {
        uint64_t id = 0;
        plan_type_t type = plan_type_t::none;
        int object_index = -1;      // gameplay/slider/spinner target; -1 for decorative
        bool valid = false;

        // control-time window (game milliseconds)
        double t_start = 0.0;
        double t_end = 0.0;         // scheduled arrival / segment end

        // captured initial state (taken from the CURRENT MotionState)
        point_t p0{}, v0{}, a0{};
        // trajectory endpoint (p1 = target, v1 = desired arrival velocity)
        point_t p1{}, v1{};

        // slider-follow blend: offset between the incoming cursor and the ball
        point_t entry_offset{};

        float max_speed = 1800.f;
        float max_accel = 14000.f;

        // plan identity used for reconciliation (see needs_replan).
        double arrival_key = 0.0;   // quantised scheduled arrival
    };

    // ---- quintic (minimum-jerk-style) Hermite evaluation ------------------
    //
    // Given endpoints and a duration, produces a C2-continuous curve with an
    // exact timed arrival.  `u` is normalised time in [0,1]; the duration is
    // needed to scale the velocity / acceleration terms back to real units.
    struct kinematic_t { point_t position, velocity, acceleration; };

    inline kinematic_t eval_quintic( const point_t& p0, const point_t& v0, const point_t& a0,
                                     const point_t& p1, const point_t& v1, const point_t& a1,
                                     float duration, float u ) {
        kinematic_t k{ p0, v0, a0 };
        if ( duration <= 1e-5f ) { k.position = p1; k.velocity = v1; k.acceleration = a1; return k; }
        u = saturate( u );
        const point_t c0 = p0;
        const point_t c1 = v0 * duration;
        const point_t c2 = a0 * ( 0.5f * duration * duration );
        const point_t d  = p1 - c0 - c1 - c2;
        const point_t dv = v1 * duration - c1 - c2 * 2.f;
        const point_t da = a1 * ( duration * duration ) - c2 * 2.f;
        const point_t c3 = d * 10.f - dv * 4.f + da * 0.5f;
        const point_t c4 = d * -15.f + dv * 7.f - da;
        const point_t c5 = d * 6.f - dv * 3.f + da * 0.5f;
        const float u2 = u * u, u3 = u2 * u, u4 = u3 * u, u5 = u4 * u;
        k.position = c0 + c1 * u + c2 * u2 + c3 * u3 + c4 * u4 + c5 * u5;
        k.velocity = ( c1 + c2 * ( 2.f * u ) + c3 * ( 3.f * u2 ) + c4 * ( 4.f * u3 ) + c5 * ( 5.f * u4 ) ) / duration;
        k.acceleration = ( c2 * 2.f + c3 * ( 6.f * u ) + c4 * ( 12.f * u2 ) + c5 * ( 20.f * u3 ) ) / ( duration * duration );
        return k;
    }

    // ---- object classification (shared with the schedule) -----------------
    inline bool is_slider( const osu::hit_object_t& o ) {
        return ( o.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) != 0;
    }
    inline bool is_spinner( const osu::hit_object_t& o ) {
        if ( o.type & static_cast<uint8_t>( osu::hit_object_type_t::spinner ) ) return true;
        // Some maps encode a spinner as a zero-length slider near the centre.
        if ( !is_slider( o ) || !o.slider_curve_str.empty( ) || o.slider_length > 0.f ) return false;
        const float dx = o.x - 256.f, dy = o.y - 192.f;
        return o.end_time - o.start_time >= 400 && dx * dx + dy * dy < 48.f * 48.f;
    }

    // ---- deterministic per-object interior target point -------------------
    //
    // One point per object, chosen once and cached by the caller.  Depends only
    // on the object, a session seed, and the aim-spread / arrival-depth style,
    // so it is stable for the whole session (never rerolls on replan).
    inline point_t object_center( const osu::hit_object_t& o, bool hr_flip ) {
        return { o.x, hr_flip ? 384.f - o.y : o.y };
    }
    inline float hit_radius( float cs, bool hr_flip ) {
        if ( hr_flip ) cs = std::min( cs * 1.3f, 10.f );
        return std::max( 8.f, 54.4f - 4.48f * cs );
    }
    inline point_t select_target_point( int index, const osu::hit_object_t& o, bool hr_flip,
                                         float cs, uint32_t seed, float aim_spread,
                                         float arrival_depth, float phase, float edge_bias ) {
        const point_t center = object_center( o, hr_flip );
        if ( is_spinner( o ) )
            return center;      // spinner target handled by the orbit; centre is a safe stand-in
        if ( is_slider( o ) )
            return center;      // slider head is the object centre
        const uint32_t h = static_cast<uint32_t>( index + 1 ) * 747796405u ^ seed;
        const float u = static_cast<float>( h & 0xffffu ) / 65535.f;
        const float v = static_cast<float>( ( h >> 16 ) & 0xffffu ) / 65535.f;
        const float frac = edge_bias > 0.f
            ? edge_bias * ( 0.92f + 0.08f * std::sqrt( u ) )
            : ( 0.08f + 0.38f * std::clamp( aim_spread, 0.f, 1.f ) ) * ( 0.82f + 0.32f * arrival_depth ) * std::sqrt( u );
        const float r = hit_radius( cs, hr_flip ) * std::clamp( frac, 0.f, 0.95f );
        const float ang = v * 2.f * k_pi + phase;
        return center + point_t{ std::cos( ang ), std::sin( ang ) } * r;
    }

    // ---- acquisition lead : ms needed to travel to a target and settle -----
    inline float acquisition_lead_ms( float distance, float accel_style ) {
        const float accel_time = 2000.f * std::sqrt( std::max( distance, 0.f ) / ( 18000.f * std::max( accel_style, 0.2f ) ) );
        const float travel_time = distance * 1000.f / 1500.f;
        return std::clamp( std::max( accel_time, travel_time ) + 130.f, 220.f, 1400.f );
    }

    // ---- slider geometry (bezier / catmull / perfect arc / linear) --------
    //
    // Pure geometry over the object's control-point string.  Built once and
    // cached by the caller; identical to the historical (well-tested) path
    // sampler, kept because it is correct and cleanly separable.
    struct slider_path_t {
        int start_time = -1;
        std::vector<point_t> path;
        std::vector<float> cumulative;
        float total = 0.f;
        bool empty( ) const { return path.size( ) < 2 || total <= 0.f; }
    };

    inline point_t eval_bezier( const std::vector<point_t>& c, float t ) {
        if ( c.empty( ) ) return {};
        if ( c.size( ) == 2 ) return c[ 0 ] + ( c[ 1 ] - c[ 0 ] ) * t;
        std::vector<point_t> w = c;
        for ( size_t step = 1; step < w.size( ); ++step )
            for ( size_t i = 0; i + step < w.size( ); ++i )
                w[ i ] = w[ i ] + ( w[ i + 1 ] - w[ i ] ) * t;
        return w[ 0 ];
    }
    inline bool same_point( point_t a, point_t b ) { return length_sq( a - b ) < 1e-4f; }
    inline void append_bezier( std::vector<point_t>& path, const std::vector<point_t>& c ) {
        if ( c.size( ) < 2 ) return;
        const int n = std::clamp( static_cast<int>( c.size( ) * 18 ), 18, 96 );
        for ( int i = path.empty( ) ? 0 : 1; i <= n; ++i )
            path.push_back( eval_bezier( c, static_cast<float>( i ) / n ) );
    }
    inline bool append_perfect( std::vector<point_t>& path, const std::vector<point_t>& c ) {
        if ( c.size( ) != 3 ) return false;
        const point_t a = c[ 0 ], b = c[ 1 ], d = c[ 2 ];
        const float det = 2.f * ( a.x * ( b.y - d.y ) + b.x * ( d.y - a.y ) + d.x * ( a.y - b.y ) );
        if ( std::abs( det ) < 1e-3f ) return false;
        const float aa = length_sq( a ), bb = length_sq( b ), cc = length_sq( d );
        const point_t ctr{ ( aa * ( b.y - d.y ) + bb * ( d.y - a.y ) + cc * ( a.y - b.y ) ) / det,
                           ( aa * ( d.x - b.x ) + bb * ( a.x - d.x ) + cc * ( b.x - a.x ) ) / det };
        const float s = std::atan2( a.y - ctr.y, a.x - ctr.x );
        const float m = std::atan2( b.y - ctr.y, b.x - ctr.x );
        const float f = std::atan2( d.y - ctr.y, d.x - ctr.x );
        auto pos = []( float x ) { while ( x < 0.f ) x += 2.f * k_pi; while ( x >= 2.f * k_pi ) x -= 2.f * k_pi; return x; };
        const float cf = pos( f - s ), cm = pos( m - s );
        const float sweep = cm <= cf ? cf : cf - 2.f * k_pi;
        const float r = length( a - ctr );
        const int n = std::clamp( static_cast<int>( std::abs( sweep ) * r / 5.f ), 18, 128 );
        for ( int i = 0; i <= n; ++i ) {
            const float ang = s + sweep * static_cast<float>( i ) / n;
            path.push_back( ctr + point_t{ std::cos( ang ), std::sin( ang ) } * r );
        }
        return true;
    }
    inline void append_catmull( std::vector<point_t>& path, const std::vector<point_t>& c ) {
        if ( c.size( ) < 2 ) return;
        for ( size_t s = 0; s + 1 < c.size( ); ++s ) {
            const point_t p0 = s > 0 ? c[ s - 1 ] : c[ s ];
            const point_t p1 = c[ s ], p2 = c[ s + 1 ];
            const point_t p3 = s + 2 < c.size( ) ? c[ s + 2 ] : p2;
            constexpr int n = 20;
            for ( int i = ( path.empty( ) ? 0 : 1 ); i <= n; ++i ) {
                const float t = static_cast<float>( i ) / n, t2 = t * t, t3 = t2 * t;
                path.push_back( ( p1 * 2.f + ( p2 - p0 ) * t + ( p0 * 2.f - p1 * 5.f + p2 * 4.f - p3 ) * t2 +
                                  ( p0 * -1.f + p1 * 3.f - p2 * 3.f + p3 ) * t3 ) * 0.5f );
            }
        }
    }
    inline std::vector<point_t> build_slider_controls_path( const osu::hit_object_t& o, bool hr_flip ) {
        std::vector<point_t> c;
        c.push_back( object_center( o, hr_flip ) );
        char type = 'L';
        if ( !o.slider_curve_str.empty( ) ) {
            std::stringstream ss( o.slider_curve_str );
            std::string item;
            bool first = true;
            while ( std::getline( ss, item, '|' ) ) {
                if ( first ) { if ( !item.empty( ) ) type = item[ 0 ]; first = false; continue; }
                const size_t colon = item.find( ':' );
                if ( colon == std::string::npos ) continue;
                try {
                    point_t p{ std::stof( item.substr( 0, colon ) ), std::stof( item.substr( colon + 1 ) ) };
                    if ( hr_flip ) p.y = 384.f - p.y;
                    c.push_back( p );
                } catch ( ... ) {}
            }
        }
        if ( c.size( ) < 2 || type == 'L' ) return c;
        std::vector<point_t> path; path.reserve( 128 );
        if ( type == 'P' && append_perfect( path, c ) ) return path;
        if ( type == 'C' ) { append_catmull( path, c ); return path; }
        size_t seg = 0;
        for ( size_t i = 1; i + 1 < c.size( ); ++i )
            if ( same_point( c[ i ], c[ i + 1 ] ) ) {
                append_bezier( path, std::vector<point_t>( c.begin( ) + seg, c.begin( ) + i + 1 ) );
                seg = i + 1;
            }
        append_bezier( path, std::vector<point_t>( c.begin( ) + seg, c.end( ) ) );
        return path;
    }
    inline slider_path_t build_slider_path( const osu::hit_object_t& o, bool hr_flip ) {
        slider_path_t s;
        s.start_time = o.start_time;
        s.path = build_slider_controls_path( o, hr_flip );
        if ( s.path.size( ) >= 2 && o.slider_length > 0.f ) {
            std::vector<point_t> trimmed;
            trimmed.push_back( s.path.front( ) );
            float covered = 0.f;
            for ( size_t i = 1; i < s.path.size( ) && covered < o.slider_length; ++i ) {
                const float seg = length( s.path[ i ] - s.path[ i - 1 ] );
                if ( covered + seg >= o.slider_length ) {
                    const float u = seg > 0.f ? ( o.slider_length - covered ) / seg : 0.f;
                    trimmed.push_back( s.path[ i - 1 ] + ( s.path[ i ] - s.path[ i - 1 ] ) * u );
                    covered = o.slider_length; break;
                }
                trimmed.push_back( s.path[ i ] ); covered += seg;
            }
            if ( covered < o.slider_length && trimmed.size( ) >= 2 ) {
                const point_t dir = normalized( trimmed.back( ) - trimmed[ trimmed.size( ) - 2 ] );
                trimmed.push_back( trimmed.back( ) + dir * ( o.slider_length - covered ) );
            }
            s.path = std::move( trimmed );
        }
        s.cumulative.push_back( 0.f );
        for ( size_t i = 1; i < s.path.size( ); ++i ) {
            s.total += length( s.path[ i ] - s.path[ i - 1 ] );
            s.cumulative.push_back( s.total );
        }
        return s;
    }
    // Ball position at game time gt (handles repeats / reversals).
    inline point_t eval_slider_ball( const slider_path_t& s, const osu::hit_object_t& o, int gt ) {
        if ( s.empty( ) ) return object_center( o, false );
        const float dur = static_cast<float>( std::max( o.end_time - o.start_time, 1 ) );
        const float t = saturate( static_cast<float>( gt - o.start_time ) / dur );
        const int spans = std::max( o.slider_repeat, 1 );
        const float sp = t * spans;
        int span = static_cast<int>( sp );
        float local = sp - span;
        if ( span >= spans ) local = ( spans % 2 == 1 ) ? 1.f : 0.f;
        else if ( span % 2 == 1 ) local = 1.f - local;
        const float target = local * s.total;
        for ( size_t i = 1; i < s.path.size( ); ++i )
            if ( target <= s.cumulative[ i ] ) {
                const float seg = s.cumulative[ i ] - s.cumulative[ i - 1 ];
                const float u = seg > 0.f ? ( target - s.cumulative[ i - 1 ] ) / seg : 0.f;
                return s.path[ i - 1 ] + ( s.path[ i ] - s.path[ i - 1 ] ) * u;
            }
        return s.path.back( );
    }

    // ---- spinner integrator ----------------------------------------------
    //
    // Angle / angular-velocity / radius state that begins from the incoming
    // motion (tangent direction + current radius) and ramps toward the target
    // RPM.  Never snaps to the centre or to full speed.
    struct spinner_state_t {
        bool active = false;
        int object_index = -1;
        point_t center{};
        float angle = 0.f, angular_velocity = 0.f, radius = 70.f, radius_velocity = 0.f, direction = 1.f;
    };

    inline void spinner_begin( spinner_state_t& sp, int index, const osu::hit_object_t& o, bool hr_flip,
                               const motion_state_t& m, uint32_t seed ) {
        sp.active = true; sp.object_index = index; sp.center = object_center( o, hr_flip );
        point_t radial = m.position - sp.center;
        sp.radius = std::clamp( length( radial ), 52.f, 82.f );
        if ( length_sq( radial ) < 1.f ) radial = { 1.f, 0.f };
        // start from the ACTUAL incoming radius (no hard clamp up) so the first
        // orbit frame equals the current cursor position -- the radius integrator
        // then ramps smoothly toward the nominal orbit radius.
        sp.radius = std::max( length( radial ), 1.f );
        sp.angle = std::atan2( radial.y, radial.x );
        const point_t tangent{ -std::sin( sp.angle ), std::cos( sp.angle ) };
        sp.direction = dot( m.velocity, tangent ) >= 0.f ? 1.f : -1.f;
        if ( length_sq( m.velocity ) < 100.f )
            sp.direction = ( seed & 1u ) ? 1.f : -1.f;
        sp.angular_velocity = std::abs( dot( m.velocity, tangent ) / std::max( sp.radius, 1.f ) ) * sp.direction;
        sp.radius_velocity = 0.f;
    }
    // Advance one step; returns desired position (velocity is derived by the caller).
    inline point_t spinner_step( spinner_state_t& sp, const osu::hit_object_t& o, int gt, float dt,
                                 float target_rpm, float phase ) {
        const float dur = std::max( static_cast<float>( o.end_time - o.start_time ), 1.f );
        const float progress = saturate( static_cast<float>( gt - o.start_time ) / dur );
        const float entry = smoothstep( 0.f, 0.16f, progress );
        const float exit = 1.f - smoothstep( 0.82f, 1.f, progress );
        const float rpm_drift = 1.f + 0.025f * std::sin( phase + gt * 0.0013f );
        const float target = std::clamp( target_rpm, 200.f, 477.f ) * 2.f * k_pi / 60.f * rpm_drift * entry * ( 0.72f + 0.28f * exit );
        const float ang_acc = std::clamp( ( target - std::abs( sp.angular_velocity ) ) * 8.f, -42.f, 42.f );
        const float ang_spd = std::max( 0.f, std::abs( sp.angular_velocity ) + ang_acc * dt );
        sp.angular_velocity = ang_spd * sp.direction;
        sp.angle += sp.angular_velocity * dt;
        const float target_radius = 68.f + 3.f * std::sin( phase * 0.7f + gt * 0.0009f );
        const float rad_acc = ( target_radius - sp.radius ) * 20.f - sp.radius_velocity * 8.f;
        sp.radius_velocity += std::clamp( rad_acc, -180.f, 180.f ) * dt;
        // wide safety clamp only: the integrator (not a hard clamp) shapes the
        // radius, so a cursor that entered inside the orbit ramps out smoothly.
        sp.radius = std::clamp( sp.radius + sp.radius_velocity * dt, 8.f, 120.f );
        // Blend the cosmetic ellipse in by the entry factor so the first orbit
        // frame is a perfect circle matching the incoming point (no snap).
        const float ellipse = 1.f + ( ( 0.96f + 0.025f * std::sin( phase + gt * 0.0007f ) ) - 1.f ) * entry;
        return sp.center + point_t{ std::cos( sp.angle ) * sp.radius, std::sin( sp.angle ) * sp.radius * ellipse };
    }

    // ---- discontinuity budget --------------------------------------------
    //
    // The largest plausible one-frame displacement given the velocity /
    // acceleration at both ends of the step, plus a small slack.  A produced
    // displacement above this is a discontinuity (reported, never hidden).
    inline float kinematic_budget( point_t v_old, point_t v_new, point_t a_old, point_t a_new, float dt ) {
        return std::max( length( v_old ), length( v_new ) ) * dt
             + 0.5f * std::max( length( a_old ), length( a_new ) ) * dt * dt + 2.f;
    }

    inline point_t clamp_to_playfield( point_t p ) {
        p.x = std::clamp( p.x, 32.f, 480.f );
        p.y = std::clamp( p.y, 28.f, 356.f );
        return p;
    }

    // ---- tiny deterministic xorshift (session style / decorative shapes) --
    struct rng_t {
        uint32_t s = 0x9e3779b9u;
        uint32_t next( ) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        float unit( ) { return static_cast<float>( next( ) & 0x00ffffffu ) / 16777216.f; }
    };

    // ---- tunables the host feeds the engine each frame --------------------
    struct settings_t {
        float aim_spread = 0.10f;
        float curve_strength = 0.36f;
        float momentum = 0.82f;
        float slider_laziness = 0.08f;
        float spinner_rpm = 420.f;
        bool  gameplay_flow = true;
        bool  startup_motion = true;
        bool  break_motion = true;
        bool  energetic_dances = true;
        float startup_energy = 0.82f;
        float break_energy = 0.68f;
    };

    // ---- per-object schedule the host supplies (press time + edge bias) ---
    //
    // Filled from the shared Relax scheduler by the host adapter; for off-line
    // tests it is populated directly.  press() falls back to the object's own
    // start_time when the object has not been scheduled yet.
    struct schedule_view_t {
        std::vector<int>   press_time;
        std::vector<float> edge_bias;
        int press( int index, const osu::hit_object_t& o ) const {
            return ( index >= 0 && index < static_cast<int>( press_time.size( ) ) )
                ? press_time[ static_cast<size_t>( index ) ] : o.start_time;
        }
        float bias( int index ) const {
            return ( index >= 0 && index < static_cast<int>( edge_bias.size( ) ) )
                ? edge_bias[ static_cast<size_t>( index ) ] : 0.f;
        }
    };

    // ---- live diagnostics describing the architecture ---------------------
    struct diagnostics_t {
        plan_type_t plan_type = plan_type_t::none;
        uint64_t    plan_id = 0;
        int         object_index = -1;
        float       plan_progress = 0.f;
        float       time_to_arrival_ms = 0.f;
        point_t     motion_position{}, motion_velocity{}, motion_acceleration{};
        point_t     target_position{};
        float       speed = 0.f, acceleration = 0.f;
        // screen fields (filled by the host adapter)
        int         requested_screen_x = 0, requested_screen_y = 0;
        int         observed_screen_x = 0, observed_screen_y = 0;
        float       projection_self_check_error = 0.f;
        // counters
        uint64_t plans_created = 0, gameplay_plans = 0, slider_plans = 0, spinner_plans = 0;
        uint64_t startup_segments = 0, break_segments = 0, recovery_plans = 0;
        uint64_t plan_invalidations = 0, external_reanchors = 0, unexpected_discontinuities = 0;
        uint64_t objects_completed = 0;
        float    max_frame_displacement = 0.f;
        // Target Accuracy passthrough (owned by the Relax controller)
        float    requested_accuracy = 100.f, predicted_accuracy = 100.f, accuracy_debt = 0.f;
        uint64_t controlled_100 = 0;
    };

    // ---- flight recorder entry -------------------------------------------
    struct flight_record_t {
        uint64_t    frame = 0;
        int         game_time = 0;
        float       dt = 0.f;
        uint64_t    plan_id = 0;
        plan_type_t plan_type = plan_type_t::none;
        int         object_index = -1;
        point_t     position{}, velocity{}, acceleration{}, target{}, requested{};
        int         observed_x = 0, observed_y = 0;
        float       displacement = 0.f, bound = 0.f;
        bool        external_reanchor = false;
        const char* reason = "";
    };

    // ========================================================================
    //  motion_engine_t : the pure, host-independent movement core.
    //
    //  Everything happens in osu! playfield coordinates.  There is exactly one
    //  MotionState and one MovementPlan.  The host adapter (autobot.hxx) reads
    //  the OS cursor, projects it to playfield space, supplies the schedule,
    //  drives update(), then projects position() back to the screen to emit.
    //
    //  Nothing in here touches Windows, so the whole planner + evaluator is
    //  exercised by the off-line tests in tests/motion_tests.cpp.
    // ========================================================================
    class motion_engine_t {
    public:
        settings_t settings{};

        void configure( const settings_t& s ) { settings = s; }
        [[nodiscard]] bool initialized( ) const { return m_motion.initialized; }
        [[nodiscard]] const motion_state_t& motion( ) const { return m_motion; }
        [[nodiscard]] point_t position( ) const { return m_motion.position; }
        [[nodiscard]] const plan_t& plan( ) const { return m_plan; }
        [[nodiscard]] plan_type_t plan_type( ) const { return m_plan.type; }
        [[nodiscard]] const diagnostics_t& diagnostics( ) const { return m_diag; }
        [[nodiscard]] uint64_t record_count( ) const { return m_rec_count; }
        [[nodiscard]] bool last_record( flight_record_t& out ) const {
            if ( m_rec_count == 0 ) return false;
            out = m_rec[ ( m_rec_head + k_rec_capacity - 1 ) % k_rec_capacity ];
            return true;
        }

        // Start (or restart) a session.  MotionState is seeded from the observed
        // cursor -- the ONLY direct position assignment allowed.
        void begin_session( point_t observed, bool hr_flip, float game_rate, size_t object_count, uint32_t seed ) {
            m_motion = {}; m_plan = {}; m_spinner = {}; m_decor = {}; m_diag = {};
            m_slider_cache.assign( object_count, {} );
            m_target_points.assign( object_count, {} );
            m_target_valid.assign( object_count, 0 );
            m_map_object_count = object_count;
            m_last_completed_index = -1;
            m_hr_flip = hr_flip;
            m_game_rate = std::clamp( game_rate, 0.5f, 2.f );
            m_seed = seed ? seed : 0x9e3779b9u;
            m_rng.s = m_seed;
            m_style.directness    = 0.80f + m_rng.unit( ) * 0.16f;
            m_style.curvature     = 0.75f + m_rng.unit( ) * 0.50f;
            m_style.accel         = 0.90f + m_rng.unit( ) * 0.20f;
            m_style.momentum_ret  = 0.82f + m_rng.unit( ) * 0.14f;
            m_style.arrival_depth = 0.45f + m_rng.unit( ) * 0.20f;
            m_style.idle_energy   = 0.35f + m_rng.unit( ) * 0.45f;
            m_style.slider_tight  = 0.82f + m_rng.unit( ) * 0.16f;
            m_style.phase         = m_rng.unit( ) * 2.f * k_pi;
            m_motion.position = observed;
            m_motion.initialized = true;
            m_next_plan_id = 1;
        }

        // Rebase on the observed cursor after a persistent external mismatch or
        // a resume; the NEXT plan begins from the observed state.
        void reanchor( point_t observed, const char* reason ) {
            m_motion.position = observed;
            m_motion.velocity = {}; m_motion.acceleration = {};
            m_plan = {}; m_spinner.active = false; m_decor.active = false;
            m_diag.external_reanchors++;
            m_reanchor_flag = true;
            capture_record( observed, observed, 0.f, 0.f, reason );
        }

        // One frame.  control_time in game ms, dt in real seconds (sanitized).
        void update( const osu::beatmap_data_t& map, const schedule_view_t& sched, int control_time, float dt ) {
            if ( !m_motion.initialized ) return;
            ++m_frame; m_frame_ctrl = control_time; m_frame_dt = dt;
            m_reanchor_flag = false;

            plan_and_reconcile( control_time, map, sched );
            eval_result_t r = evaluate_plan( control_time, dt, map );
            commit_motion( r, dt );
            advance_completed( control_time, map, sched );
            update_live_diagnostics( control_time );
        }

        // Host hooks for screen-space events it detects (geometry / emit).
        void note_reanchor_pending( ) { m_reanchor_flag = true; }
        void record_external( point_t at, float disp, float bound, const char* reason ) {
            capture_record( at, at, disp, bound, reason );
            m_diag.unexpected_discontinuities++;
        }
        void bump_discontinuity( ) { m_diag.unexpected_discontinuities++; }
        void set_screen_diag( int rx, int ry, int ox, int oy ) {
            m_diag.requested_screen_x = rx; m_diag.requested_screen_y = ry;
            m_diag.observed_screen_x = ox; m_diag.observed_screen_y = oy;
        }
        void set_accuracy_diag( float requested, float predicted, float debt, uint64_t controlled ) {
            m_diag.requested_accuracy = requested; m_diag.predicted_accuracy = predicted;
            m_diag.accuracy_debt = debt; m_diag.controlled_100 = controlled;
        }
        void set_projection_error( float e ) { m_diag.projection_self_check_error = e; }
        void invalidate_plan( ) { m_plan.valid = false; }

    private:
        struct style_t {
            float directness = 0.85f, curvature = 1.f, accel = 1.f, momentum_ret = 0.85f;
            float arrival_depth = 0.55f, idle_energy = 0.5f, slider_tight = 0.85f, phase = 0.f;
        };
        struct decor_state_t {
            bool active = false, startup = false;
            point_t anchor{};
            int segments = 0;
        };
        struct eval_result_t {
            point_t position{}, velocity{}, acceleration{};
            bool dynamic = false;
            const char* reason = "plan";
        };

        motion_state_t m_motion{};
        plan_t         m_plan{};
        spinner_state_t m_spinner{};
        style_t        m_style{};
        decor_state_t  m_decor{};
        diagnostics_t  m_diag{};
        rng_t          m_rng{};

        std::vector<slider_path_t> m_slider_cache;
        std::vector<point_t>       m_target_points;
        std::vector<uint8_t>       m_target_valid;

        bool     m_hr_flip = false;
        size_t   m_map_object_count = 0;
        int      m_last_completed_index = -1;
        uint32_t m_seed = 0x9e3779b9u;
        float    m_game_rate = 1.f;
        uint64_t m_next_plan_id = 1;
        bool     m_reanchor_flag = false;

        uint64_t m_frame = 0;
        int      m_frame_ctrl = 0;
        float    m_frame_dt = 0.f;

        static constexpr size_t k_rec_capacity = 24;
        std::array<flight_record_t, k_rec_capacity> m_rec{};
        size_t   m_rec_head = 0;
        uint64_t m_rec_count = 0;

        // ---- schedule helpers --------------------------------------------
        int next_object_index( ) const {
            const int n = static_cast<int>( m_map_object_count );
            const int i = m_last_completed_index + 1;
            return i < n ? i : -1;
        }
        int find_active_long( int control_time, const osu::beatmap_data_t& map, const schedule_view_t& sched ) const {
            const size_t start = static_cast<size_t>( std::max( m_last_completed_index - 1, 0 ) );
            for ( size_t i = start; i < map.objects.size( ); ++i ) {
                const auto& o = map.objects[ i ];
                if ( !is_slider( o ) && !is_spinner( o ) ) continue;
                const int press = sched.press( static_cast<int>( i ), o );
                if ( control_time >= press && control_time <= o.end_time + 8 ) return static_cast<int>( i );
                if ( press > control_time ) break;
            }
            return -1;
        }
        void advance_completed( int control_time, const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            while ( m_last_completed_index + 1 < static_cast<int>( map.objects.size( ) ) ) {
                const int n = m_last_completed_index + 1;
                if ( control_time < sched.press( n, map.objects[ static_cast<size_t>( n ) ] ) ) break;
                m_last_completed_index = n;
                m_diag.objects_completed++;
            }
        }
        point_t target_point( int index, const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            if ( index < 0 || index >= static_cast<int>( map.objects.size( ) ) ) return m_motion.position;
            const size_t slot = static_cast<size_t>( index );
            if ( slot < m_target_valid.size( ) && m_target_valid[ slot ] ) return m_target_points[ slot ];
            const osu::hit_object_t& o = map.objects[ slot ];
            point_t p;
            if ( is_spinner( o ) ) {
                // Approach the ORBIT, not the centre: a radial entry point at the
                // orbit radius so the spinner plan can take over with (almost) no
                // first-frame snap.  Radial is taken from the current cursor so
                // the incoming direction is respected; cached write-once.
                const point_t center = object_center( o, m_hr_flip );
                point_t radial = normalized( m_motion.position - center );
                if ( length_sq( radial ) < 0.1f ) radial = { 1.f, 0.f };
                p = center + radial * 70.f;
            } else {
                p = select_target_point( index, o, m_hr_flip, map.cs, m_seed, settings.aim_spread,
                                         m_style.arrival_depth, m_style.phase, sched.bias( index ) );
            }
            if ( slot < m_target_points.size( ) ) { m_target_points[ slot ] = p; m_target_valid[ slot ] = 1; }
            return p;
        }
        const slider_path_t& slider_path( int index, const osu::hit_object_t& o ) {
            static slider_path_t empty{};
            if ( index < 0 || index >= static_cast<int>( m_slider_cache.size( ) ) ) return empty;
            auto& slot = m_slider_cache[ static_cast<size_t>( index ) ];
            if ( slot.start_time == o.start_time ) return slot;
            slot = build_slider_path( o, m_hr_flip );
            return slot;
        }

        // ---- planner ------------------------------------------------------
        void plan_and_reconcile( int control_time, const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            const int active = find_active_long( control_time, map, sched );
            if ( active >= 0 ) {
                const auto& o = map.objects[ static_cast<size_t>( active ) ];
                if ( is_spinner( o ) ) make_or_keep_spinner( active, o, control_time );
                else                   make_or_keep_slider( active, o, control_time );
                m_decor.active = false;
                return;
            }
            m_spinner.active = false;

            const int target = next_object_index( );
            if ( target < 0 ) { make_or_keep_decor( false, control_time ); return; }

            const auto& o = map.objects[ static_cast<size_t>( target ) ];
            const int arrival = sched.press( target, o );
            const point_t tp = target_point( target, map, sched );
            const float time_left = static_cast<float>( arrival - control_time );

            if ( time_left <= 0.f ) {
                make_or_keep_gameplay( plan_type_t::recovery, target, arrival, tp, control_time, map, sched );
                m_decor.active = false;
                return;
            }
            const float dist = length( tp - m_motion.position );
            const float lead = acquisition_lead_ms( dist, m_style.accel );
            const bool startup = m_last_completed_index < 0 && settings.startup_motion;
            const bool brk = m_last_completed_index >= 0 && settings.break_motion;
            const float safety = startup ? 260.f : 320.f;
            const float slack = startup ? 120.f : 600.f;
            const bool decor_ok = ( startup || brk ) && time_left > lead + safety + slack;
            if ( decor_ok ) { make_or_keep_decor( startup, control_time ); return; }

            make_or_keep_gameplay( plan_type_t::gameplay, target, arrival, tp, control_time, map, sched );
            m_decor.active = false;
        }

        void make_or_keep_gameplay( plan_type_t type, int index, int arrival, point_t target,
                                    int control_time, const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            const bool same = m_plan.valid && m_plan.type == type && m_plan.object_index == index &&
                              std::abs( m_plan.arrival_key - static_cast<double>( arrival ) ) <= 12.0;
            if ( same ) return;

            plan_t p{};
            p.id = m_next_plan_id++;
            p.type = type;
            p.object_index = index;
            p.valid = true;
            p.t_start = static_cast<double>( control_time );
            p.t_end = static_cast<double>( std::max( arrival, control_time + 1 ) );
            p.arrival_key = static_cast<double>( arrival );
            p.p0 = m_motion.position; p.v0 = m_motion.velocity; p.a0 = m_motion.acceleration;
            p.p1 = target;
            p.v1 = arrival_velocity( index, target, type, map, sched );
            const float seconds = std::max( ( arrival - control_time ) * 0.001f, 0.02f );
            const float required = length( target - m_motion.position ) / seconds;
            if ( type == plan_type_t::recovery ) {
                p.max_speed = std::clamp( required * 2.4f + 900.f, 1600.f, 7000.f );
                p.max_accel = std::clamp( required * 10.f + 14000.f, 16000.f, 60000.f );
                p.v1 = p.v1 * 0.4f;
            } else {
                p.max_speed = std::clamp( required * 2.1f + 500.f, 1200.f, 6200.f );
                p.max_accel = std::clamp( required * 8.f + 9000.f, 12000.f, 52000.f );
            }
            register_new_plan( p );
            if ( type == plan_type_t::recovery ) m_diag.recovery_plans++;
            else m_diag.gameplay_plans++;
        }

        point_t arrival_velocity( int index, point_t target, plan_type_t type,
                                  const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            if ( type == plan_type_t::recovery || !settings.gameplay_flow ) return {};
            const int next = index + 1;
            if ( next >= static_cast<int>( map.objects.size( ) ) ) return {};
            const point_t ntp = target_point( next, map, sched );
            const int a = sched.press( index, map.objects[ static_cast<size_t>( index ) ] );
            const int b = sched.press( next, map.objects[ static_cast<size_t>( next ) ] );
            const float seconds = std::max( ( b - a ) * 0.001f, 0.035f );
            const point_t delta = ntp - target;
            const float dist = length( delta );
            const float radius = hit_radius( map.cs, m_hr_flip );
            float gain = 1.f;
            if ( dist < radius * 0.45f ) gain = 0.18f;
            const point_t dir = normalized( delta );
            const float carry = std::clamp( settings.momentum, 0.f, 0.95f ) * m_style.momentum_ret;
            const float curve = 0.72f + 0.28f * std::clamp( settings.curve_strength, 0.f, 1.f ) * m_style.curvature;
            return limit( dir * ( dist / seconds ) * carry * gain * curve, 2200.f );
        }

        void make_or_keep_slider( int index, const osu::hit_object_t& o, int control_time ) {
            if ( m_plan.valid && m_plan.type == plan_type_t::slider && m_plan.object_index == index ) return;
            const slider_path_t& sp = slider_path( index, o );
            const point_t ball = sp.empty( ) ? object_center( o, m_hr_flip ) : eval_slider_ball( sp, o, control_time );
            plan_t p{};
            p.id = m_next_plan_id++;
            p.type = plan_type_t::slider;
            p.object_index = index;
            p.valid = true;
            p.t_start = static_cast<double>( control_time );
            p.t_end = static_cast<double>( o.end_time );
            p.p0 = m_motion.position; p.v0 = m_motion.velocity; p.a0 = m_motion.acceleration;
            p.entry_offset = m_motion.position - ball;
            register_new_plan( p );
            m_diag.slider_plans++;
        }

        void make_or_keep_spinner( int index, const osu::hit_object_t& o, int control_time ) {
            if ( m_plan.valid && m_plan.type == plan_type_t::spinner && m_plan.object_index == index &&
                 m_spinner.active && m_spinner.object_index == index )
                return;
            spinner_begin( m_spinner, index, o, m_hr_flip, m_motion, m_seed ^ static_cast<uint32_t>( index ) );
            plan_t p{};
            p.id = m_next_plan_id++;
            p.type = plan_type_t::spinner;
            p.object_index = index;
            p.valid = true;
            p.t_start = static_cast<double>( control_time );
            p.t_end = static_cast<double>( o.end_time );
            p.p0 = m_motion.position; p.v0 = m_motion.velocity; p.a0 = m_motion.acceleration;
            register_new_plan( p );
            m_diag.spinner_plans++;
        }

        void make_or_keep_decor( bool startup, int control_time ) {
            const plan_type_t type = startup ? plan_type_t::startup : plan_type_t::break_decor;
            if ( !m_decor.active || m_plan.type != type ) {
                m_decor = {}; m_decor.active = true; m_decor.startup = startup; m_decor.anchor = m_motion.position;
                start_decor_segment( control_time );
                return;
            }
            if ( control_time >= m_plan.t_end ) start_decor_segment( control_time );
        }
        void start_decor_segment( int control_time ) {
            const bool startup = m_decor.startup;
            const float energy = std::clamp( startup ? settings.startup_energy : settings.break_energy, 0.f, 1.f );
            const bool energetic = settings.energetic_dances &&
                m_rng.unit( ) < ( startup ? 0.18f + 0.4f * energy : 0.12f + 0.5f * energy );
            const float base = ( startup ? 14.f : 10.f ) + energy * ( startup ? 24.f : 30.f );
            const float radius = base * ( energetic ? 1.8f + m_rng.unit( ) * 1.6f : 1.f ) * ( 0.6f + m_rng.unit( ) );
            const bool pause = m_rng.unit( ) < ( startup ? 0.10f : 0.22f );
            const float ang = m_rng.unit( ) * 2.f * k_pi;
            if ( energetic && m_rng.unit( ) < 0.35f ) {
                const float da = m_rng.unit( ) * 2.f * k_pi;
                m_decor.anchor = clamp_to_playfield( m_decor.anchor + point_t{ std::cos( da ), std::sin( da ) } * ( base * 0.8f ) );
            }
            const point_t dest = pause ? m_motion.position
                : clamp_to_playfield( m_decor.anchor + point_t{ std::cos( ang ), std::sin( ang ) } * radius );
            const int duration = pause ? static_cast<int>( 80.f + m_rng.unit( ) * 160.f )
                : static_cast<int>( ( startup ? 130.f : 260.f ) + m_rng.unit( ) * ( startup ? 260.f : 440.f ) );
            plan_t p{};
            p.id = m_next_plan_id++;
            p.type = type_of_decor( startup );
            p.object_index = -1;
            p.valid = true;
            p.t_start = static_cast<double>( control_time );
            p.t_end = static_cast<double>( control_time + duration );
            p.arrival_key = p.t_end;
            // decorative motion is calm: damp any inherited speed so a fast
            // exit (e.g. off a spinner) settles into local movement instead of
            // flinging.  Position is unchanged, so this stays continuous.
            p.p0 = m_motion.position; p.v0 = limit( m_motion.velocity, 700.f ); p.a0 = {};
            p.p1 = dest;
            const point_t radial = normalized( dest - m_motion.position );
            p.v1 = pause ? point_t{} : point_t{ -radial.y, radial.x } * ( startup ? 70.f + energy * 80.f : 35.f );
            p.max_speed = 1400.f; p.max_accel = 16000.f;
            register_new_plan( p );
            m_decor.active = true;
            m_decor.segments++;
            if ( startup ) m_diag.startup_segments++;
            else m_diag.break_segments++;
        }
        static plan_type_t type_of_decor( bool startup ) { return startup ? plan_type_t::startup : plan_type_t::break_decor; }

        void register_new_plan( const plan_t& p ) {
            const bool was_valid = m_plan.valid;
            m_plan = p;
            m_diag.plans_created++;
            m_diag.plan_id = p.id;
            if ( was_valid ) m_diag.plan_invalidations++;
        }

        // ---- evaluate -----------------------------------------------------
        eval_result_t evaluate_plan( int control_time, float dt, const osu::beatmap_data_t& map ) {
            eval_result_t r{};
            r.position = m_motion.position; r.velocity = m_motion.velocity; r.acceleration = m_motion.acceleration;
            if ( !m_plan.valid ) { r.reason = "no-plan"; return r; }
            switch ( m_plan.type ) {
            case plan_type_t::slider:  return eval_slider( control_time, map );
            case plan_type_t::spinner: return eval_spinner( control_time, dt, map );
            default:                   return eval_trajectory( control_time );
            }
        }
        eval_result_t eval_trajectory( int control_time ) {
            eval_result_t r{};
            const double dur_ms = std::max( m_plan.t_end - m_plan.t_start, 1.0 );
            const float u = saturate( static_cast<float>( ( control_time - m_plan.t_start ) / dur_ms ) );
            const float dur_sec = std::max( static_cast<float>( dur_ms ) * 0.001f / std::max( m_game_rate, 0.5f ), 0.02f );
            const kinematic_t k = eval_quintic( m_plan.p0, m_plan.v0, m_plan.a0, m_plan.p1, m_plan.v1, {}, dur_sec, u );
            r.position = k.position;
            // Store the TRUE analytic derivatives so the continuity budget in
            // commit_motion is self-consistent with the actual position delta.
            // (Speed is shaped by the trajectory endpoints/duration, not by a
            // post-hoc clamp that would desync velocity from position.)
            r.velocity = k.velocity;
            r.acceleration = k.acceleration;
            r.dynamic = false;
            r.reason = plan_type_name( m_plan.type );
            return r;
        }
        eval_result_t eval_slider( int control_time, const osu::beatmap_data_t& map ) {
            eval_result_t r{};
            const auto& o = map.objects[ static_cast<size_t>( m_plan.object_index ) ];
            const slider_path_t& sp = slider_path( m_plan.object_index, o );
            const point_t ball = sp.empty( ) ? object_center( o, m_hr_flip )
                                             : eval_slider_ball( sp, o, std::min( control_time, o.end_time ) );
            const float age = static_cast<float>( control_time - m_plan.t_start );
            const float decay = std::exp( -std::max( age, 0.f ) / 45.f );
            const float lazy = std::clamp( settings.slider_laziness, 0.f, 1.f ) * ( 2.f - m_style.slider_tight );
            point_t desired = ball + m_plan.entry_offset * decay;
            desired = desired + ( m_motion.position - desired ) * ( lazy * 0.045f );
            r.position = desired;
            r.dynamic = true;
            r.reason = "SLIDER";
            return r;
        }
        eval_result_t eval_spinner( int control_time, float dt, const osu::beatmap_data_t& map ) {
            eval_result_t r{};
            const auto& o = map.objects[ static_cast<size_t>( m_plan.object_index ) ];
            if ( !m_spinner.active || m_spinner.object_index != m_plan.object_index )
                spinner_begin( m_spinner, m_plan.object_index, o, m_hr_flip, m_motion, m_seed );
            r.position = spinner_step( m_spinner, o, control_time, dt, settings.spinner_rpm, m_style.phase );
            r.dynamic = true;
            r.reason = "SPINNER";
            return r;
        }

        // ---- commit -------------------------------------------------------
        void commit_motion( eval_result_t r, float dt ) {
            const point_t old_p = m_motion.position, old_v = m_motion.velocity, old_a = m_motion.acceleration;
            if ( !finite( r.position ) ) {
                m_diag.unexpected_discontinuities++;
                capture_record( old_p, r.position, 0.f, 0.f, "non-finite" );
                return;
            }
            point_t new_v, new_a;
            if ( r.dynamic ) {
                new_v = ( r.position - old_p ) / std::max( dt, 0.001f );
                new_a = ( new_v - old_v ) / std::max( dt, 0.001f );
            } else {
                new_v = finite( r.velocity ) ? r.velocity : ( r.position - old_p ) / std::max( dt, 0.001f );
                new_a = finite( r.acceleration ) ? r.acceleration : point_t{};
            }
            const float disp = length( r.position - old_p );
            const float bound = kinematic_budget( old_v, new_v, old_a, new_a, dt );
            if ( disp > bound + 0.01f ) {
                m_diag.unexpected_discontinuities++;
                capture_record( old_p, r.position, disp, bound, r.reason );
            }
            m_motion.position = r.position;
            m_motion.velocity = new_v;
            m_motion.acceleration = new_a;
            m_diag.max_frame_displacement = std::max( m_diag.max_frame_displacement, disp );
        }

        void update_live_diagnostics( int control_time ) {
            m_diag.plan_type = m_plan.type;
            m_diag.plan_id = m_plan.id;
            m_diag.object_index = m_plan.object_index;
            const double dur = std::max( m_plan.t_end - m_plan.t_start, 1.0 );
            m_diag.plan_progress = m_plan.valid ? saturate( static_cast<float>( ( control_time - m_plan.t_start ) / dur ) ) : 0.f;
            m_diag.time_to_arrival_ms = m_plan.valid ? static_cast<float>( m_plan.t_end - control_time ) : 0.f;
            m_diag.motion_position = m_motion.position;
            m_diag.motion_velocity = m_motion.velocity;
            m_diag.motion_acceleration = m_motion.acceleration;
            m_diag.speed = length( m_motion.velocity );
            m_diag.acceleration = length( m_motion.acceleration );
            m_diag.target_position = ( m_plan.valid && m_plan.type != plan_type_t::slider && m_plan.type != plan_type_t::spinner )
                ? m_plan.p1 : m_motion.position;
        }

        void capture_record( point_t before, point_t desired, float disp, float bound, const char* reason ) {
            flight_record_t t{};
            t.frame = m_frame; t.game_time = m_frame_ctrl; t.dt = m_frame_dt;
            t.plan_id = m_plan.id; t.plan_type = m_plan.type; t.object_index = m_plan.object_index;
            t.position = before; t.velocity = m_motion.velocity; t.acceleration = m_motion.acceleration;
            t.target = m_plan.p1; t.requested = desired;
            t.observed_x = m_diag.observed_screen_x; t.observed_y = m_diag.observed_screen_y;
            t.displacement = disp; t.bound = bound; t.external_reanchor = m_reanchor_flag; t.reason = reason;
            m_rec[ m_rec_head ] = t;
            m_rec_head = ( m_rec_head + 1 ) % k_rec_capacity;
            ++m_rec_count;
        }
    };

} // namespace autobot
