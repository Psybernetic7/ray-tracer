#include "doctest.h"

#include "rtheader.h"
#include "analysis/analysis_grid.h"
#include "analysis/horizon_map.h"
#include "analysis/scalar_map.h"
#include "analysis/sun_hours.h"
#include "analysis/viewshed.h"
#include "geometry/bvh.h"
#include "geometry/heightfield.h"
#include "lighting/solar.h"
#include "math/colormap.h"
#include "parallel.h"
#include "render/material.h"
#include "terrain/terrain.h"

#include <atomic>
#include <functional>
#include <vector>


// A size x size heightfield with `res` vertices per side, its heights set by `height(x, z)`.
static heightfield make_field(int res, double size, const std::function<double(double, double)>& height) {
    heightfield field(res, res, size, size);
    for (int j = 0; j < res; j++) {
        for (int i = 0; i < res; i++) {
            point3 p = field.vertex_position(i, j);
            field.at(i, j) = height(p.x(), p.z());
        }
    }
    return field;
}

static bvh_node mesh_of(const heightfield& field) {
    return bvh_node(field.to_triangles(make_shared<lambertian>(color(0.5, 0.5, 0.5))));
}

static double total_hours(const std::vector<sun_sample>& samples) {
    double sum = 0;
    for (const auto& s : samples) sum += s.hours;
    return sum;
}


TEST_CASE("solar position matches known values") {
    // London at the June solstice: noon elevation is 90 - 51.5 + 23.44 = 61.9 degrees, due south.
    solar_position london = sun_position(51.5, 172, 12);
    CHECK(std::fabs(london.elevation - 61.9) < 0.5);
    CHECK(std::fabs(london.azimuth - 180) < 3);
    CHECK(london.direction.z() > 0);  // South is +z

    // On the equator at the March equinox the noon sun is almost straight overhead.
    CHECK(sun_position(0, 80, 12).elevation > 87.5);

    // Sydney in June: a low winter sun, due north.
    solar_position sydney = sun_position(-33.87, 172, 12);
    CHECK(std::fabs(sydney.elevation - 32.7) < 0.7);
    CHECK(std::cos(degrees_to_radians(sydney.azimuth)) > 0.99);

    // An equinox sun rises close to due east and sets close to due west.
    CHECK(std::fabs(sun_position(46.5, 80, 6.5).azimuth - 90) < 10);
    CHECK(std::fabs(sun_position(46.5, 80, 17.5).azimuth - 270) < 10);

    // Polar night and midnight sun at 80 degrees north.
    for (double hour = 0; hour < 24; hour += 0.5)
        REQUIRE(sun_position(80, 355, hour).elevation < 0);
    CHECK(sun_position(80, 172, 0).elevation > 0);

    // The direction and the angles describe the same point in the sky.
    solar_position p = sun_position(46.5, 120, 9.25);
    CHECK(p.direction.length() == doctest::Approx(1));
    CHECK(std::asin(p.direction.y()) * 180 / pi == doctest::Approx(p.elevation));
}

TEST_CASE("clear-sky irradiance falls with the sun's elevation") {
    CHECK(clear_sky_dni(90) == doctest::Approx(0.947).epsilon(0.01));  // 1.353 kW/m^2 x 0.7
    CHECK(clear_sky_dni(10) < clear_sky_dni(30));
    CHECK(clear_sky_dni(30) < clear_sky_dni(90));
    CHECK(clear_sky_dni(0.5) > 0);   // Air mass stays finite near the horizon
    CHECK(clear_sky_dni(-5) == 0);
}

TEST_CASE("the sun schedule covers about half the hours of the year") {
    // Without refraction, every place on earth gets close to 4380 hours of daylight a year.
    for (double latitude : {0.0, 46.5, -33.9, 70.0}) {
        sun_schedule schedule;
        schedule.latitude = latitude;
        schedule.day_step = 5;
        schedule.minutes = 10;
        auto samples = sun_samples(schedule);
        double hours = total_hours(samples);
        CHECK(hours > 4300);
        CHECK(hours < 4480);
        for (const auto& s : samples)
            REQUIRE(s.elevation > 0);
    }
}

TEST_CASE("scalar map interpolates between cell centres and skips missing data") {
    scalar_map map(2, 2);
    map.at(0, 0) = 0; map.at(1, 0) = 4;
    map.at(0, 1) = 8; map.at(1, 1) = 12;

    CHECK(map.sample(0.25, 0.25) == doctest::Approx(0));   // A cell centre
    CHECK(map.sample(0.75, 0.75) == doctest::Approx(12));
    CHECK(map.sample(0.5, 0.25) == doctest::Approx(2));    // Halfway between two centres
    CHECK(map.sample(0.5, 0.5) == doctest::Approx(6));
    CHECK(map.sample(0.0, 0.0) == doctest::Approx(0));     // Clamped at the border

    map.at(1, 1) = std::nan("");
    CHECK(map.range().min == 0);
    CHECK(map.range().max == 8);
    CHECK(map.mean() == doctest::Approx(4));
}

TEST_CASE("colormaps hit their published endpoints") {
    auto close = [](const color& c, double r, double g, double b) {
        return std::fabs(c.x() - r/255) < 0.03 && std::fabs(c.y() - g/255) < 0.03
            && std::fabs(c.z() - b/255) < 0.03;
    };
    CHECK(close(viridis(0), 68, 1, 84));
    CHECK(close(viridis(0.5), 33, 145, 140));
    CHECK(close(viridis(1), 253, 231, 37));
    CHECK(close(inferno(0), 0, 0, 4));
    CHECK(close(inferno(0.5), 188, 55, 84));
    CHECK(close(inferno(1), 252, 255, 164));
    CHECK(close(viridis(-1), 68, 1, 84));  // Clamped outside [0,1]
    CHECK(close(viridis(2), 253, 231, 37));
}

TEST_CASE("parallel_for visits every index exactly once") {
    for (int threads : {1, 3, 16}) {
        std::vector<std::atomic<int>> visits(1000);
        parallel_for(1000, threads, [&](int k) { visits[k]++; });
        for (auto& v : visits)
            REQUIRE(v == 1);
    }
    int calls = 0;
    parallel_for(0, 4, [&](int) { calls++; });
    CHECK(calls == 0);
}

TEST_CASE("analysis grid puts its points on the surface") {
    // A plane tilted along x: y = 0.5 x.
    heightfield field = make_field(33, 4.0, [](double x, double) { return 0.5 * x; });
    bvh_node world = mesh_of(field);
    analysis_grid grid(world, 4.0, 4.0, 10, 10);

    for (int y = 0; y < grid.height(); y++) {
        for (int x = 0; x < grid.width(); x++) {
            REQUIRE(grid.valid(x, y));
            const point3& p = grid.ground(x, y);
            CHECK(p.x() == doctest::Approx(grid.world_x(x)));
            CHECK(p.y() == doctest::Approx(0.5 * p.x()));
            CHECK(grid.normal(x, y).y() == doctest::Approx(2 / std::sqrt(5.0)));
            CHECK((grid.origin(x, y) - p).length() == doctest::Approx(grid.lift()));
        }
    }

    int hx = -1, hy = -1;
    REQUIRE(grid.highest_cell(hx, hy));
    CHECK(hx == grid.width() - 1);  // The plane rises toward +x
}

TEST_CASE("on open flat ground every method sees the whole sky") {
    heightfield field = make_field(17, 4.0, [](double, double) { return 0.0; });
    bvh_node world = mesh_of(field);
    analysis_grid grid(world, 4.0, 4.0, 6, 6);

    sun_schedule schedule;
    schedule.day_step = 30;
    schedule.minutes = 30;
    auto samples = sun_samples(schedule);
    const double all_hours = total_hours(samples);

    scalar_map brute = sun_hours_brute_force(grid, world, samples, sun_quantity::hours);
    horizon_map horizons(grid, world);
    scalar_map fast = sun_hours_from_horizons(grid, horizons, samples, sun_quantity::hours);

    for (int y = 0; y < grid.height(); y++) {
        for (int x = 0; x < grid.width(); x++) {
            CHECK(brute.at(x, y) == doctest::Approx(all_hours));
            CHECK(fast.at(x, y) == doctest::Approx(all_hours));
            for (int d = 0; d < horizons.directions(); d++)
                REQUIRE(horizons.elevation(x, y, d) == 0);
        }
    }
    // One ray per direction per cell: every direction is clear at the first try.
    CHECK(horizons.rays() == uint64_t(grid.width() * grid.height() * horizons.directions()));

    // Irradiance on flat ground: each hour is weighted by the sine of the sun's elevation.
    double expected = 0;
    for (const auto& s : samples) expected += s.hours * s.dni * s.direction.y();
    CHECK(sun_hours_brute_force(grid, world, samples, sun_quantity::irradiance).at(2, 3)
          == doctest::Approx(expected));
}

TEST_CASE("a wall to the south raises the skyline and steals sun") {
    // A 2-unit wall filling the southern edge of the field (south is +z), from z = 1 onwards.
    heightfield field = make_field(81, 4.0, [](double, double z) { return z >= 1.0 ? 2.0 : 0.0; });
    bvh_node world = mesh_of(field);
    analysis_grid grid(world, 4.0, 4.0, 16, 16);
    horizon_map horizons(grid, world);

    // A cell in the open ground north of the wall: looking south, the skyline is the top of
    // the wall; looking north, there is nothing.
    const int x = 8, y = 6;
    const point3 p = grid.origin(x, y);
    REQUIRE(p.z() < 0.5);
    const double expected = std::atan2(2.0 - p.y(), 1.0 - p.z()) * 180 / pi;
    CHECK(std::fabs(horizons.elevation_toward(x, y, 180) - expected) < 1.5);
    CHECK(horizons.elevation_toward(x, y, 0) == 0);

    // The wall is symmetric about this cell, so south-east and south-west must agree. South-west
    // runs exactly along the mesh's diagonal edges from here, and once read 0 because rays
    // slipped between the triangles on either side of those edges.
    CHECK(horizons.elevation_toward(x, y, 225) == doctest::Approx(horizons.elevation_toward(x, y, 135)));

    // In the northern hemisphere the sun stands in the south, so ground close under the wall
    // gets less of it than ground further north.
    sun_schedule schedule;
    schedule.day_step = 15;
    schedule.minutes = 20;
    auto samples = sun_samples(schedule);
    scalar_map hours = sun_hours_from_horizons(grid, horizons, samples, sun_quantity::hours);
    CHECK(hours.at(x, 9) < hours.at(x, 2));
    // Even the far cell loses some: from there the wall still stands 40 degrees above the
    // southern horizon, and the winter noon sun at 46.5 N only climbs to 20.
    CHECK(hours.at(x, 2) < total_hours(samples));

    scalar_map brute = sun_hours_brute_force(grid, world, samples, sun_quantity::hours);
    CHECK(hours.at(x, 9) == doctest::Approx(brute.at(x, 9)).epsilon(0.03));
    CHECK(hours.at(x, 2) == doctest::Approx(brute.at(x, 2)).epsilon(0.03));
}

TEST_CASE("horizon-map sun-hours agree with brute force on fractal terrain") {
    terrain_params params;
    params.seed = 21;
    heightfield field = generate_heightfield(65, 8.0, 2.5, params);
    bvh_node world = mesh_of(field);
    analysis_grid grid(world, 8.0, 8.0, 20, 20);

    sun_schedule schedule;
    schedule.day_step = 30;
    schedule.minutes = 30;
    auto samples = sun_samples(schedule);

    for (sun_quantity quantity : {sun_quantity::hours, sun_quantity::irradiance}) {
        scalar_map brute = sun_hours_brute_force(grid, world, samples, quantity);
        horizon_map horizons(grid, world);
        scalar_map fast = sun_hours_from_horizons(grid, horizons, samples, quantity);

        const double scale = brute.range().max;
        REQUIRE(scale > 0);
        double total_error = 0;
        int cells = 0, close = 0;
        for (int y = 0; y < grid.height(); y++) {
            for (int x = 0; x < grid.width(); x++) {
                double error = std::fabs(brute.at(x, y) - fast.at(x, y));
                total_error += error;
                cells++;
                if (error <= 0.05 * scale) close++;
            }
        }
        CHECK(total_error / cells < 0.02 * scale);  // Mean error under 2% of the range
        CHECK(close >= 0.9 * cells);                // 90% of cells within 5%
    }
}

TEST_CASE("viewshed: open ground is all visible, a ridge hides what is behind it") {
    heightfield flat = make_field(17, 4.0, [](double, double) { return 0.0; });
    bvh_node flat_world = mesh_of(flat);
    analysis_grid flat_grid(flat_world, 4.0, 4.0, 8, 8);
    scalar_map open = viewshed(flat_grid, flat_world, point3(0, 0.1, 0));
    CHECK(open.range().min == 1);

    // An east-west ridge, 1 unit high, across the middle of the field.
    heightfield ridge = make_field(81, 4.0, [](double, double z) {
        return std::fabs(z) <= 0.1 ? 1.0 : 0.0;
    });
    bvh_node world = mesh_of(ridge);
    analysis_grid grid(world, 4.0, 4.0, 16, 16);

    const point3 observer(0, 0.05, -1.5);  // Low down, north of the ridge
    uint64_t rays = 0;
    scalar_map seen = viewshed(grid, world, observer, 0, 0, &rays);
    CHECK(rays == uint64_t(grid.width() * grid.height()));

    CHECK(seen.at(8, 1) == 1);    // Open ground on the observer's side
    CHECK(seen.at(8, 15) == 0);   // Far side of the ridge, in its shadow
    CHECK(seen.at(2, 14) == 0);

    // A tall enough target pokes up over the ridge and becomes visible.
    scalar_map masts = viewshed(grid, world, observer, 3.0);
    CHECK(masts.at(8, 15) == 1);
}
