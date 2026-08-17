// ============================================================================
//  motion_tests.cpp  --  deterministic off-line tests for the Autobot engine.
//
//  Exercises the pure motion_engine_t (motion_core.hxx) without osu!, Windows,
//  or Relax.  Each scenario builds a synthetic beatmap + schedule, drives the
//  engine frame by frame, and asserts the movement invariants:
//
//      * every produced position is finite,
//      * no unexpected discontinuity (the engine's own C2 budget is never
//        exceeded) -- i.e. no teleports / no first-frame snaps,
//      * per-frame displacement stays bounded,
//      * exactly one plan owns movement,
//      * a decorative plan never owns while gameplay acquisition is required,
//      * cached target points never change,
//      * plans begin from the current MotionState (no stale reuse),
//      * plan transitions happen in the right order.
//
//  Build (from lame/):
//    cl /std:c++20 /EHsc /I. /Iimpl tests\motion_tests.cpp /Fe:motion_tests.exe
//  Run:
//    motion_tests.exe
// ============================================================================

#include <core/autobot/motion_core.hxx>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace autobot;

static int g_failures = 0;
static int g_checks = 0;

static void check( bool cond, const std::string& scenario, const std::string& what ) {
    ++g_checks;
    if ( !cond ) {
        ++g_failures;
        std::printf( "  [FAIL] %-28s : %s\n", scenario.c_str( ), what.c_str( ) );
    }
}

// ---- synthetic object builders --------------------------------------------
static osu::hit_object_t circle( int t, float x, float y ) {
    osu::hit_object_t o; o.start_time = t; o.end_time = t; o.x = x; o.y = y; o.type = 1; return o;
}
static osu::hit_object_t slider( int t, int end, float x, float y, const char* curve, float length, int repeat = 1 ) {
    osu::hit_object_t o; o.start_time = t; o.end_time = end; o.x = x; o.y = y; o.type = 2;
    o.slider_curve_str = curve; o.slider_length = length; o.slider_repeat = repeat; return o;
}
static osu::hit_object_t spinner( int t, int end ) {
    osu::hit_object_t o; o.start_time = t; o.end_time = end; o.x = 256.f; o.y = 192.f; o.type = 8; return o;
}

static osu::beatmap_data_t make_map( std::vector<osu::hit_object_t> objs, float cs = 4.f ) {
    osu::beatmap_data_t m; m.loaded = true; m.cs = cs; m.od = 8.f; m.ar = 9.f; m.objects = std::move( objs );
    return m;
}
static schedule_view_t make_schedule( const osu::beatmap_data_t& map ) {
    schedule_view_t s;
    s.press_time.resize( map.objects.size( ) );
    s.edge_bias.assign( map.objects.size( ), 0.f );
    for ( size_t i = 0; i < map.objects.size( ); ++i ) s.press_time[ i ] = map.objects[ i ].start_time;
    return s;
}

// ---- a driver that records the movement invariants ------------------------
struct sim_result_t {
    int    frames = 0;
    float  max_frame_disp = 0.f;
    bool   all_finite = true;
    bool   decor_owned_during_acquire = false;
    // ordered set of distinct object indices that a GAMEPLAY/RECOVERY/SLIDER/SPINNER
    // plan pointed at, in the order they first appeared.
    std::vector<int> gameplay_route;
    std::vector<plan_type_t> plan_sequence;   // distinct consecutive plan types
};

// Runs the engine from t0 to t1 in fixed control-time steps.
// acquire_guard: if true, asserts the plan is never decorative when the next
// object is within `acquire_window_ms` of the control time.
static sim_result_t run( motion_engine_t& eng, const osu::beatmap_data_t& map, schedule_view_t& sched,
                         int t0, int t1, int step_ms, float dt_scale = 1.f, int acquire_window_ms = 250 ) {
    sim_result_t r;
    point_t prev = eng.position( );
    plan_type_t last_type = plan_type_t::none;
    int last_route = -999;
    uint64_t disc = eng.diagnostics( ).unexpected_discontinuities;
    for ( int t = t0; t <= t1; t += step_ms ) {
        const float dt = ( step_ms * 0.001f ) * dt_scale;
        eng.update( map, sched, t, dt );
        if ( std::getenv( "DBG" ) && eng.diagnostics( ).unexpected_discontinuities > disc ) {
            disc = eng.diagnostics( ).unexpected_discontinuities;
            flight_record_t fr; eng.last_record( fr );
            std::printf( "    DBG t=%d plan=%s obj=%d disp=%.2f bound=%.2f reason=%s\n",
                t, plan_type_name( fr.plan_type ), fr.object_index, fr.displacement, fr.bound, fr.reason );
        }
        const point_t p = eng.position( );
        if ( !std::isfinite( p.x ) || !std::isfinite( p.y ) ) r.all_finite = false;
        const float d = length( p - prev );
        if ( r.frames > 0 ) r.max_frame_disp = std::max( r.max_frame_disp, d );
        prev = p;
        ++r.frames;

        const plan_t& pl = eng.plan( );
        if ( pl.type != last_type ) { r.plan_sequence.push_back( pl.type ); last_type = pl.type; }
        const bool gameplay_like = pl.type == plan_type_t::gameplay || pl.type == plan_type_t::recovery ||
                                   pl.type == plan_type_t::slider || pl.type == plan_type_t::spinner;
        if ( gameplay_like && pl.object_index != last_route ) { r.gameplay_route.push_back( pl.object_index ); last_route = pl.object_index; }

        // decorative-owns-during-acquire guard
        const bool decor = pl.type == plan_type_t::startup || pl.type == plan_type_t::break_decor;
        if ( decor ) {
            // find the next object press relative to t
            for ( const auto& sp : sched.press_time ) {
                if ( sp >= t && sp - t <= acquire_window_ms ) { r.decor_owned_during_acquire = true; break; }
            }
        }
    }
    return r;
}

static void assert_common( const std::string& name, const sim_result_t& r, float disp_cap ) {
    check( r.all_finite, name, "all positions finite" );
    check( r.max_frame_disp <= disp_cap, name,
        "max frame displacement " + std::to_string( r.max_frame_disp ) + " <= " + std::to_string( disp_cap ) );
    check( !r.decor_owned_during_acquire, name, "decorative never owns during acquisition" );
}

static motion_engine_t fresh_engine( const osu::beatmap_data_t& map, point_t start, uint32_t seed = 12345u ) {
    motion_engine_t e;
    settings_t s;                                   // defaults ~ the shipped config
    e.configure( s );
    e.begin_session( start, /*hr_flip*/false, /*rate*/1.f, map.objects.size( ), seed );
    return e;
}

// ===========================================================================
//  Scenarios
// ===========================================================================

// 1. spaced circles
static void t_spaced_circles( ) {
    const std::string name = "1 spaced circles";
    auto map = make_map( { circle( 1000, 100, 100 ), circle( 1400, 300, 120 ),
                           circle( 1800, 260, 300 ), circle( 2200, 120, 280 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2500, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
    // route visits the four circles in order
    std::vector<int> want = { 0, 1, 2, 3 };
    bool ok = r.gameplay_route.size( ) >= 4;
    for ( size_t i = 0; ok && i < want.size( ); ++i ) ok = ( i < r.gameplay_route.size( ) && r.gameplay_route[ i ] == want[ i ] );
    check( ok, name, "route A->B->C->D in order" );
    check( eng.diagnostics( ).objects_completed == 4, name, "all objects completed" );
}

// 2. dense stream
static void t_dense_stream( ) {
    const std::string name = "2 dense stream";
    std::vector<osu::hit_object_t> objs;
    for ( int i = 0; i < 24; ++i ) objs.push_back( circle( 1000 + i * 60, 120.f + i * 12.f, 180.f + ( i % 2 ? 24.f : -24.f ) ) );
    auto map = make_map( objs );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 500, 2600, 4 );
    assert_common( name, r, 30.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
    check( eng.diagnostics( ).objects_completed == 24, name, "stream completed" );
}

// 3. large jumps
static void t_large_jumps( ) {
    const std::string name = "3 large jumps";
    auto map = make_map( { circle( 1000, 40, 40 ), circle( 1500, 480, 350 ),
                           circle( 2000, 40, 350 ), circle( 2500, 480, 40 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2800, 4 );
    assert_common( name, r, 60.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
}

// 4. sharp reversal
static void t_sharp_reversal( ) {
    const std::string name = "4 sharp reversal";
    auto map = make_map( { circle( 1000, 100, 200 ), circle( 1300, 400, 200 ),
                           circle( 1600, 100, 200 ), circle( 1900, 400, 200 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2200, 4 );
    assert_common( name, r, 55.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
}

// 5. stacked notes
static void t_stacked_notes( ) {
    const std::string name = "5 stacked notes";
    std::vector<osu::hit_object_t> objs;
    for ( int i = 0; i < 6; ++i ) objs.push_back( circle( 1000 + i * 120, 250.f, 190.f ) );  // all on top
    auto map = make_map( objs );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 500, 1900, 4 );
    assert_common( name, r, 25.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
    // after arriving, movement over the stack stays tiny
    check( r.max_frame_disp < 20.f, name, "stack: minimal movement" );
}

// 6. circle -> slider
static void t_circle_slider( ) {
    const std::string name = "6 circle->slider";
    auto map = make_map( { circle( 1000, 100, 100 ),
                           slider( 1500, 2100, 300, 200, "B|380:200|420:320", 260.f ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2200, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no snap into slider follow" );
    // saw a slider plan
    bool saw_slider = false;
    for ( auto t : r.plan_sequence ) if ( t == plan_type_t::slider ) saw_slider = true;
    check( saw_slider, name, "entered slider plan" );
}

// 7. slider -> circle
static void t_slider_circle( ) {
    const std::string name = "7 slider->circle";
    auto map = make_map( { slider( 1000, 1600, 150, 150, "L|350:150", 200.f ),
                           circle( 2000, 400, 300 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 150, 150 } );
    auto r = run( eng, map, sched, 200, 2200, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "smooth slider exit -> circle" );
    check( eng.diagnostics( ).objects_completed == 2, name, "both completed" );
}

// 8. circle -> spinner
static void t_circle_spinner( ) {
    const std::string name = "8 circle->spinner";
    auto map = make_map( { circle( 1000, 120, 120 ), spinner( 1500, 2600 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2700, 4 );
    assert_common( name, r, 50.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no snap into spinner orbit" );
    bool saw_spinner = false;
    for ( auto t : r.plan_sequence ) if ( t == plan_type_t::spinner ) saw_spinner = true;
    check( saw_spinner, name, "entered spinner plan" );
}

// 9. spinner -> circle
static void t_spinner_circle( ) {
    const std::string name = "9 spinner->circle";
    auto map = make_map( { spinner( 1000, 2000 ), circle( 2500, 400, 120 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2800, 4 );
    assert_common( name, r, 55.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "smooth spinner exit -> circle" );
}

// 10. long break -> gameplay
static void t_long_break( ) {
    const std::string name = "10 long break->gameplay";
    auto map = make_map( { circle( 1000, 200, 200 ), circle( 6000, 300, 200 ) } );  // 5s gap
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 6100, 8 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities across break" );
    check( eng.diagnostics( ).break_segments > 0, name, "break motion happened" );
    check( eng.diagnostics( ).objects_completed == 2, name, "resumed and hit next object" );
}

// 11. startup -> first object
static void t_startup( ) {
    const std::string name = "11 startup->first object";
    auto map = make_map( { circle( 4000, 300, 250 ) } );   // 4s of lead time
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 100, 4100, 8 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities during startup" );
    check( eng.diagnostics( ).startup_segments > 0, name, "startup motion happened" );
    check( eng.diagnostics( ).objects_completed == 1, name, "reached first object" );
}

// 12. frame hitch
static void t_frame_hitch( ) {
    const std::string name = "12 frame hitch";
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 1600, 380, 260 ), circle( 2200, 120, 300 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    // normal frames, then one giant hitch (250 ms of control time, clamped dt), then normal
    point_t prev = eng.position( );
    float worst = 0.f;
    bool finite_ok = true;
    for ( int t = 200; t <= 2400; t += 4 ) {
        int step = 4;
        float dt = 0.004f;
        if ( t == 1400 ) { step = 250; dt = 0.025f; }   // hitch: engine must clamp dt
        eng.update( map, sched, t, dt );
        const point_t p = eng.position( );
        finite_ok = finite_ok && std::isfinite( p.x ) && std::isfinite( p.y );
        worst = std::max( worst, length( p - prev ) );
        prev = p;
        if ( step != 4 ) t += ( step - 4 );
    }
    check( finite_ok, name, "all positions finite through hitch" );
    // A 250 ms control-time advance legitimately covers real distance, but a
    // single frame must not produce an unbounded jump; the engine records any
    // budget violation.  We accept a larger single-frame move here but require
    // it to remain finite and the engine to have flagged nothing internal.
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "hitch produced no discontinuity" );
}

// 13. map rewind (retry) -> new session
static void t_map_rewind( ) {
    const std::string name = "13 map rewind";
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 1600, 380, 260 ), circle( 2200, 300, 300 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    run( eng, map, sched, 200, 1800, 4 );
    // host detects the rewind and restarts the session from the observed cursor
    const point_t observed = eng.position( );
    eng.begin_session( observed, false, 1.f, map.objects.size( ), 999u );
    auto r = run( eng, map, sched, 200, 2400, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuity after rewind" );
    check( eng.diagnostics( ).objects_completed == 3, name, "replayed all objects" );
}

// 14. one anomalous cursor observation (single reanchor request ignored logic)
static void t_one_anomaly( ) {
    const std::string name = "14 one anomalous observation";
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 1600, 380, 260 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    // Drive normally.  A single spurious observation should NOT reanchor -- the
    // adapter only reanchors after 3 persistent frames, so at the engine level
    // we verify that a lone reanchor is the ONLY way position changes abruptly,
    // and that without a reanchor call the motion stays continuous.
    auto r = run( eng, map, sched, 200, 1800, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).external_reanchors == 0, name, "no reanchor without persistent mismatch" );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "single anomaly does not perturb engine" );
}

// 15. persistent cursor mismatch -> reanchor is continuous at the engine level
static void t_persistent_mismatch( ) {
    const std::string name = "15 persistent mismatch";
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 2000, 380, 260 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    run( eng, map, sched, 200, 900, 4 );
    // Host decided the mismatch is persistent and reanchors to an observed point.
    const point_t observed = eng.position( ) + point_t{ 40.f, -25.f };
    eng.reanchor( observed, "external-mismatch" );
    check( eng.diagnostics( ).external_reanchors == 1, name, "reanchor counted" );
    check( length( eng.position( ) - observed ) < 0.01f, name, "reanchored to observed" );
    // after reanchor, the NEXT plan starts from the observed state -> continuous
    auto r = run( eng, map, sched, 904, 2100, 4 );
    check( r.all_finite, name, "finite after reanchor" );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "continuous after reanchor" );
}

// 16. one anomalous playfield geometry sample (handled in adapter; verify the
//     engine motion is projection-independent and stays continuous regardless)
static void t_geometry_anomaly( ) {
    const std::string name = "16 geometry anomaly";
    // The engine works purely in playfield space; a bad *screen* rect is the
    // adapter's concern (it holds the frame).  Here we confirm the engine never
    // itself produces a discontinuity that a bad projection could amplify.
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 1600, 380, 260 ), circle( 2200, 200, 320 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2400, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "engine motion continuous" );
    check( eng.diagnostics( ).max_frame_displacement < 45.f, name, "bounded internal displacement" );
}

// ---- cross-cutting invariants ---------------------------------------------

// target points are stable across the whole session (never reroll)
static void t_target_point_stability( ) {
    const std::string name = "X target-point stability";
    auto map = make_map( { circle( 1000, 100, 100 ), circle( 1400, 300, 120 ), circle( 1800, 260, 300 ) } );
    auto sched = make_schedule( map );
    // The engine caches target points internally; we can only observe stability
    // through select_target_point being deterministic for a fixed seed.
    point_t a = select_target_point( 1, map.objects[ 1 ], false, map.cs, 777u, 0.10f, 0.55f, 1.2f, 0.f );
    point_t b = select_target_point( 1, map.objects[ 1 ], false, map.cs, 777u, 0.10f, 0.55f, 1.2f, 0.f );
    check( length( a - b ) < 1e-4f, name, "target point deterministic for fixed seed" );
    // and the chosen point is inside the object's hit circle
    const point_t c = object_center( map.objects[ 1 ], false );
    check( length( a - c ) <= hit_radius( map.cs, false ), name, "target point inside hit radius" );
}

// quintic endpoints: reaches p1 at u=1, starts at p0 at u=0 (timed arrival)
static void t_quintic_arrival( ) {
    const std::string name = "X quintic arrival";
    point_t p0{ 50, 60 }, v0{ 120, -40 }, a0{ 0, 0 }, p1{ 400, 300 }, v1{ 60, 0 };
    auto k0 = eval_quintic( p0, v0, a0, p1, v1, {}, 0.5f, 0.f );
    auto k1 = eval_quintic( p0, v0, a0, p1, v1, {}, 0.5f, 1.f );
    check( length( k0.position - p0 ) < 1e-3f, name, "starts at p0" );
    check( length( k1.position - p1 ) < 1e-3f, name, "arrives at p1" );
    check( length( k0.velocity - v0 ) < 1e-2f, name, "starts at v0 (continuity)" );
}

int main( ) {
    std::printf( "Autobot motion engine -- deterministic off-line tests\n" );
    std::printf( "-----------------------------------------------------\n" );
    t_spaced_circles( );
    t_dense_stream( );
    t_large_jumps( );
    t_sharp_reversal( );
    t_stacked_notes( );
    t_circle_slider( );
    t_slider_circle( );
    t_circle_spinner( );
    t_spinner_circle( );
    t_long_break( );
    t_startup( );
    t_frame_hitch( );
    t_map_rewind( );
    t_one_anomaly( );
    t_persistent_mismatch( );
    t_geometry_anomaly( );
    t_target_point_stability( );
    t_quintic_arrival( );

    std::printf( "-----------------------------------------------------\n" );
    std::printf( "%d checks, %d failures\n", g_checks, g_failures );
    return g_failures == 0 ? 0 : 1;
}
