#include "doctest.h"

#include "studio.h"

#include <cmath>


// The browser app's session object, driven natively: everything the web page can ask for,
// without a browser.

namespace {

terrain_params small_terrain() {
    terrain_params p;
    p.seed = 3;
    p.octaves = 5;
    return p;
}

size_t lit_pixels(const std::vector<uint8_t>& rgba) {
    size_t n = 0;
    for (size_t k = 0; k + 3 < rgba.size(); k += 4)
        if (rgba[k] + rgba[k + 1] + rgba[k + 2] > 0) n++;
    return n;
}

}  // namespace


TEST_CASE("Studio renders progressively and restarts when the view changes") {
    studio s(2);
    CHECK_FALSE(s.has_terrain());
    s.render_pass(1);  // Nothing to render yet: must not crash
    CHECK(s.samples() == 0);

    s.generate(small_terrain(), 32, 3.2);
    CHECK(s.has_terrain());
    CHECK_FALSE(s.is_real());
    CHECK(s.statistics().triangles == 2 * 31 * 31);

    s.set_size(48, 27);
    s.render_pass(1);
    CHECK(s.width() == 48);
    CHECK(s.height() == 27);
    CHECK(s.samples() == 1);
    REQUIRE(s.rgba().size() == size_t(48) * 27 * 4);
    CHECK(lit_pixels(s.rgba()) > size_t(48) * 27 * 9 / 10);  // One sample can leave a deep shadow black
    CHECK(s.rgba()[3] == 255);  // Opaque

    s.render_pass(2);
    CHECK(s.samples() == 3);  // Passes accumulate

    s.set_orbit(40, 20, 1.6);
    s.render_pass(1);
    CHECK(s.samples() == 1);  // A new view starts over

    s.set_sun_time(172, 2);  // 2 am: the sun is down
    CHECK(s.sun_now().elevation < 0);
    s.render_pass(1);
    CHECK(s.samples() == 1);
}

TEST_CASE("Studio images don't depend on the thread count") {
    studio one(1), four(4);
    for (studio* s : {&one, &four}) {
        s->generate(small_terrain(), 24, 3.2);
        s->set_size(40, 24);
        s->render_pass(2);
    }
    CHECK(one.rgba() == four.rgba());
}

TEST_CASE("Studio picking finds the terrain under the cursor") {
    studio s(2);
    s.generate(small_terrain(), 32, 3.2);
    s.set_size(64, 36);
    s.set_orbit(152, 10, 1.0);

    // The middle of the image looks at a point above the middle of the terrain, and the ray
    // carries on down to the ground behind it.
    const pick_result middle = s.pick(0.5, 0.5);
    CHECK(middle.hit);
    CHECK(middle.on_terrain);
    CHECK(middle.u > 0);
    CHECK(middle.u < 1);
    CHECK_FALSE(middle.geo);
    CHECK(std::isnan(middle.value));  // Render mode has no analysis value
    CHECK(middle.elevation_m == doctest::Approx(1000 * middle.y));

    // The top of a low view is sky.
    CHECK_FALSE(s.pick(0.5, 0.02).hit);
}

TEST_CASE("Studio sun-hours match the engine and reuse the horizon map") {
    studio s(3);
    const terrain_params params = small_terrain();
    s.generate(params, 32, 3.2);
    s.set_grid_size(24);
    s.set_mode(studio_mode::sun_hours);
    CHECK(s.analysis_pending());
    s.run_analysis();
    CHECK_FALSE(s.analysis_pending());
    REQUIRE(s.has_result());

    // The same computation done by hand, the way the CLI does it.
    terrain_options opts;
    opts.resolution = 32;
    opts.terrain = params;
    terrain_model model = build_terrain(opts);
    bvh_node mesh(model.triangles);
    analysis_grid grid(mesh, opts.size, opts.size, 24, 24, 1);
    horizon_map horizons(grid, mesh, horizon_map::default_directions, 1);
    sun_schedule schedule;
    schedule.latitude = 46.5;
    const scalar_map expected = sun_hours_from_horizons(grid, horizons, sun_samples(schedule),
                                                        sun_quantity::hours, 1);
    CHECK(s.result_range().min == doctest::Approx(expected.range().min));
    CHECK(s.result_range().max == doctest::Approx(expected.range().max));
    CHECK(s.result_mean() == doctest::Approx(expected.mean()));

    // A new latitude is only lookups: the horizon map is kept.
    const uint64_t rays = s.statistics().horizon_rays;
    const double mean_alps = s.result_mean();
    s.set_latitude(65);
    CHECK(s.analysis_pending());
    s.run_analysis();
    CHECK(s.statistics().horizon_rays == rays);
    CHECK(s.result_mean() != doctest::Approx(mean_alps));

    // The hover read-out reports the analysis value on the terrain.
    s.set_size(64, 36);
    s.set_orbit(152, 10, 1.0);
    const pick_result p = s.pick(0.5, 0.5);
    REQUIRE(p.on_terrain);
    CHECK(p.value >= s.result_range().min - 1e-9);
    CHECK(p.value <= s.result_range().max + 1e-9);

    s.set_mode(studio_mode::irradiance);
    s.run_analysis();
    CHECK(s.result_colors().kind == overlay_kind::irradiance);
    CHECK(s.result_range().max < 3000);  // kWh/m^2 per year: well under the hours
}

TEST_CASE("Studio viewshed starts on the highest point and follows the observer") {
    studio s(2);
    s.generate(small_terrain(), 32, 3.2);
    s.set_grid_size(32);
    s.set_mode(studio_mode::viewshed);
    s.render_pass(1);  // Computes the viewshed on the way
    REQUIRE(s.has_result());
    CHECK(s.result_range().max == 1.0);
    const double from_peak = s.result_mean();
    CHECK(from_peak > 0);

    double u, v;
    REQUIRE(s.observer(u, v));

    // Down in a corner, at the foot of the mountains, much less is in view.
    CHECK(s.set_observer(0.02, 0.02));
    s.run_analysis();
    CHECK(s.result_mean() < from_peak);

    // A 2 km mast up there sees more again.
    const double from_corner = s.result_mean();
    s.set_observer_height_m(2000);
    s.run_analysis();
    CHECK(s.result_mean() > from_corner);
}

TEST_CASE("Studio builds a real region from tiles as they arrive") {
    studio s(2);
    const auto tiles = s.begin_region(46.0, 7.5, 4.0, 32);
    REQUIRE_FALSE(tiles.empty());

    // A finish with tiles missing fails cleanly.
    std::string error;
    CHECK_FALSE(s.finish_region("Nowhere", error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(s.has_terrain());

    // Synthetic tiles: flat ground 1,500 m up, with a 100 m step along one tile column.
    s.begin_region(46.0, 7.5, 4.0, 32);
    for (const tile_id& t : tiles) {
        dem_grid tile(dem_tile_size, dem_tile_size);
        for (int y = 0; y < dem_tile_size; y++)
            for (int x = 0; x < dem_tile_size; x++)
                tile.at(x, y) = 1500.0f + (t.x == tiles.front().x ? 100.0f : 0.0f);
        s.add_tile(t.x, t.y, std::move(tile));
    }
    REQUIRE(s.finish_region("Test valley", error));
    CHECK(s.is_real());
    CHECK(s.label() == "Test valley");
    CHECK(s.size_km() == doctest::Approx(4.0));
    CHECK(s.latitude() == doctest::Approx(46.0));
    CHECK(s.min_elevation_m() >= 1499.0);
    CHECK(s.max_elevation_m() <= 1601.0);

    s.set_size(64, 36);
    const pick_result p = s.pick(0.5, 0.5);
    REQUIRE(p.on_terrain);
    CHECK(p.geo);
    CHECK(p.lat == doctest::Approx(46.0).epsilon(0.001));
    CHECK(p.lon == doctest::Approx(7.5).epsilon(0.001));
    CHECK(p.elevation_m >= 1499.0);
}

TEST_CASE("Studio guesses a snow line from the latitude") {
    CHECK(studio::typical_snow_line_m(46) == doctest::Approx(2900));   // The Alps, as bundled
    CHECK(studio::typical_snow_line_m(-25) == doctest::Approx(5500));  // Symmetric about the equator
    CHECK(studio::typical_snow_line_m(28) > 5000);                     // Everest's flanks
    CHECK(studio::typical_snow_line_m(70) < 1000);
}
