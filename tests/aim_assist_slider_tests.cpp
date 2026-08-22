#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <core/aim_assist/a_core.hxx>
#include <core/aim_assist/slider_profile.hxx>

#include <cmath>
#include <cstdio>

static int failures = 0;
static int checks = 0;

static void check( bool condition, const char* name ) {
    ++checks;
    if ( !condition ) {
        ++failures;
        std::printf( "[FAIL] %s\n", name );
    }
}

static assist::waypoint_t slider_target( float geometry, bool compact ) {
    assist::waypoint_t target{};
    target.is_slider = true;
    target.slider_geometry_demand = geometry;
    target.slider_compact = compact;
    return target;
}

int main( ) {
    constexpr float radius = 30.f;
    constexpr float zero_inner = 18.f;
    constexpr float accepted_safe = 22.f;

    // A/B: compact repeat geometry and an already-safe trajectory produce no demand.
    auto compact = slider_target( 0.06f, true );
    const auto safe_compact = assist::compute_slider_assist_demand(
        compact, 70.f, 15.f, zero_inner, accepted_safe, radius, 180.f );
    check( safe_compact.already_safe, "compact safe trajectory is identified" );
    check( safe_compact.demand == 0.f, "compact safe trajectory has zero demand" );

    // C: the same easy slider remains rescuable when its entry is a fast, clear miss.
    const auto difficult_entry = assist::compute_slider_assist_demand(
        compact, 120.f, 46.f, zero_inner, accepted_safe, radius, 120.f );
    check( difficult_entry.entry_difficulty > 0.85f,
        "large fast slider entry is difficult" );
    check( difficult_entry.demand > 0.70f,
        "compact slider allows strong evidence-based rescue" );

    // Easy geometry plus a slight miss stays deliberately subtle.
    const auto slight_easy_miss = assist::compute_slider_assist_demand(
        compact, 48.f, 23.f, zero_inner, accepted_safe, radius, 260.f );
    check( slight_easy_miss.demand < 0.12f,
        "compact slight miss remains low demand" );

    const auto dt_entry = assist::compute_slider_assist_demand(
        compact, 78.f, 34.f, zero_inner, accepted_safe, radius, 260.f, 1.5f );
    const auto nomod_entry = assist::compute_slider_assist_demand(
        compact, 78.f, 34.f, zero_inner, accepted_safe, radius, 260.f, 1.f );
    check( dt_entry.entry_difficulty > nomod_entry.entry_difficulty,
        "faster speed mod raises real-time entry difficulty" );

    // D: difficult geometry never overrides a naturally safe raw trajectory.
    auto long_slider = slider_target( 0.90f, false );
    const auto safe_long = assist::compute_slider_assist_demand(
        long_slider, 100.f, 16.f, zero_inner, accepted_safe, radius, 140.f );
    check( safe_long.demand == 0.f,
        "long slider safe trajectory has zero demand" );

    // E: the slider-only helper is an exact identity for circle targets.
    assist::waypoint_t circle{};
    const auto circle_result = assist::compute_slider_assist_demand(
        circle, 90.f, 50.f, zero_inner, accepted_safe, radius, 120.f );
    check( circle_result.demand == 1.f,
        "circle demand remains exactly one" );

    // F: Ignore Sliders retains the original selection behavior.
    assist::waypoint_t markers[ 2 ]{};
    markers[ 0 ].is_slider = true;
    markers[ 0 ].start_time = 100;
    markers[ 1 ].is_slider = false;
    markers[ 1 ].start_time = 200;
    assist::config config{};
    config.ignore_sliders = true;
    assist::trace trace{};
    const auto* selected = assist::select_target( markers, 2, 0, config, trace );
    check( selected == &markers[ 1 ], "Ignore Sliders skips slider target" );

    // Geometry classification uses one-span spatial excursion, never length * repeats.
    osu::hit_object_t short_repeat{};
    short_repeat.type = static_cast<uint8_t>( osu::hit_object_type_t::slider );
    short_repeat.x = 100.f;
    short_repeat.y = 100.f;
    short_repeat.start_time = 1000;
    short_repeat.end_time = 1600;
    short_repeat.slider_curve_str = "L|110:100";
    short_repeat.slider_length = 10.f;
    short_repeat.slider_repeat = 5;
    const auto short_profile = aim_assist::build_slider_profile(
        short_repeat, false, radius, 1.f );
    check( short_profile.compact_repeat,
        "short multi-repeat slider is classified compact" );
    check( short_profile.geometry_demand < 0.10f,
        "repeat count does not inflate compact geometry demand" );

    osu::hit_object_t long_span = short_repeat;
    long_span.slider_curve_str = "L|300:100";
    long_span.slider_length = 200.f;
    long_span.slider_repeat = 1;
    const auto long_profile = aim_assist::build_slider_profile(
        long_span, false, radius, 1.f );
    check( !long_profile.compact && long_profile.geometry_demand > short_profile.geometry_demand,
        "meaningful spatial excursion raises geometry demand" );

    std::printf( "%d checks, %d failures\n", checks, failures );
    return failures == 0 ? 0 : 1;
}
