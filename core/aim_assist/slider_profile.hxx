#pragma once

#include <core/autobot/motion_core.hxx>
#include <algorithm>
#include <cmath>

namespace aim_assist {

    struct slider_profile_t {
        float geometry_demand = 0.f;
        float excursion_radii = 0.f;
        float bounds_radii = 0.f;
        float duration_ms = 0.f;
        int32_t repeat_count = 1;
        bool compact = false;
        bool compact_repeat = false;
    };

    inline float profile_smoothstep( float edge0, float edge1, float x ) {
        if ( edge1 <= edge0 )
            return x >= edge1 ? 1.f : 0.f;
        const float t = std::clamp( ( x - edge0 ) / ( edge1 - edge0 ), 0.f, 1.f );
        return t * t * ( 3.f - 2.f * t );
    }

    // Build once per slider/map snapshot. The existing Autobot path sampler already
    // handles linear, Bezier, Catmull and perfect-arc curves, so Aim Assist only derives
    // normalized geometry characteristics instead of maintaining a second curve parser.
    inline slider_profile_t build_slider_profile(
        const osu::hit_object_t& object,
        bool hr_flip,
        float hit_radius,
        float speed_mult ) {

        slider_profile_t profile{};
        profile.repeat_count = std::max( object.slider_repeat, 1 );
        profile.duration_ms = static_cast<float>(
            std::max( object.end_time - object.start_time, 0 ) ) /
            std::max( speed_mult, 0.25f );

        const autobot::slider_path_t path = autobot::build_slider_path( object, hr_flip );
        if ( path.path.empty( ) )
            return profile;

        const autobot::point_t head = path.path.front( );
        float min_x = head.x;
        float max_x = head.x;
        float min_y = head.y;
        float max_y = head.y;
        float maximum_excursion = 0.f;

        for ( const auto& point : path.path ) {
            min_x = std::min( min_x, point.x );
            max_x = std::max( max_x, point.x );
            min_y = std::min( min_y, point.y );
            max_y = std::max( max_y, point.y );
            maximum_excursion = std::max(
                maximum_excursion, autobot::length( point - head ) );
        }

        const float radius = std::max( hit_radius, 1.f );
        const float bounds_diagonal = std::sqrt(
            ( max_x - min_x ) * ( max_x - min_x ) +
            ( max_y - min_y ) * ( max_y - min_y ) );
        profile.excursion_radii = maximum_excursion / radius;
        profile.bounds_radii = bounds_diagonal / radius;
        const float one_span_radii = path.total / radius;

        const float excursion_score = profile_smoothstep(
            0.80f, 3.40f, profile.excursion_radii );
        const float bounds_score = profile_smoothstep(
            1.00f, 4.20f, profile.bounds_radii );
        const float span_score = profile_smoothstep(
            1.20f, 6.00f, one_span_radii );
        const float duration_score = profile_smoothstep(
            320.f, 1100.f, profile.duration_ms );
        const float spatial_presence = std::max( excursion_score, bounds_score );

        profile.geometry_demand = std::clamp(
            0.52f * excursion_score + 0.25f * bounds_score +
            0.15f * span_score + 0.08f * duration_score * spatial_presence,
            0.f, 1.f );

        profile.compact = profile.excursion_radii <= 1.20f &&
            profile.bounds_radii <= 1.75f;
        profile.compact_repeat = profile.compact && profile.repeat_count > 1;

        // Repeat count never increases demand. A compact back-and-forth slider may have
        // substantial total travel while requiring very little hand excursion.
        if ( profile.compact ) {
            const float compact_scale = profile.compact_repeat ? 0.30f : 0.45f;
            profile.geometry_demand *= compact_scale;
        }

        return profile;
    }
}
