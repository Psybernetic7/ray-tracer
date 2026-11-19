#include "doctest.h"

#include "rtheader.h"
#include "terrain/dem.h"

#include <set>


TEST_CASE("Terrarium decoding and encoding") {
    // Sea level is the middle of the range: R=128 is 32768.
    CHECK(terrarium_decode(128, 0, 0) == doctest::Approx(0.0));
    CHECK(terrarium_decode(0, 0, 0) == doctest::Approx(-32768.0));
    // Mount Everest, 8848.86 m: 41616.86 = 162*256 + 144 + 220/256 (to the nearest 1/256 m).
    CHECK(terrarium_decode(162, 144, 220) == doctest::Approx(8848.859375));

    for (double h : {-10994.0, -0.5, 0.0, 1.0 / 256, 372.25, 3776.0, 8848.86}) {
        uint8_t r, g, b;
        terrarium_encode(h, r, g, b);
        CHECK(std::fabs(terrarium_decode(r, g, b) - h) <= 0.5 / 256 + 1e-9);
    }

    dem_grid grid(2, 1);
    grid.at(0, 0) = -12.5f;
    grid.at(1, 0) = 2961.0f;
    const auto rgb = encode_terrarium(grid);
    const dem_grid back = decode_terrarium(rgb.data(), 2, 1);
    CHECK(back.at(0, 0) == doctest::Approx(-12.5));
    CHECK(back.at(1, 0) == doctest::Approx(2961.0));
}

TEST_CASE("Web Mercator coordinates match known values") {
    // Zoom 0 is a single 256-pixel tile; (0, 0) is its centre.
    CHECK(mercator_x(0, 0) == doctest::Approx(128.0));
    CHECK(mercator_y(0, 0) == doctest::Approx(128.0));
    CHECK(mercator_x(-180, 3) == doctest::Approx(0.0));
    CHECK(mercator_y(85.0511287798, 0) == doctest::Approx(0.0).epsilon(1e-6).scale(1));

    // The standard slippy-map tiles for London at zoom 10 and Grand Canyon Village at zoom 12.
    CHECK(tile_at(51.5074, -0.1278, 10) == tile_id{10, 511, 340});
    CHECK(tile_at(36.06, -112.14, 12) == tile_id{12, 772, 1607});

    // Round trips.
    for (double lat : {-60.0, -12.3, 0.0, 35.3606, 45.9763, 70.0}) {
        CHECK(mercator_lat(mercator_y(lat, 12), 12) == doctest::Approx(lat).epsilon(1e-12));
    }
    for (double lon : {-179.9, -112.1, 0.0, 7.6586, 138.7274}) {
        CHECK(mercator_lon(mercator_x(lon, 12), 12) == doctest::Approx(lon).epsilon(1e-12));
    }

    // 156,543 m per pixel at zoom 0 on the equator, halving with each zoom level.
    CHECK(metres_per_pixel(0, 0) == doctest::Approx(156543.034).epsilon(1e-6));
    CHECK(metres_per_pixel(60, 1) == doctest::Approx(156543.034 / 4).epsilon(1e-6));

    // The coarsest zoom that resolves the spacing: z12 at 35 degrees is about 31 m per pixel,
    // z11 about 63 m.
    CHECK(choose_zoom(35.36, 58.7) == 12);
    CHECK(choose_zoom(35.36, 64.0) == 11);
    CHECK(choose_zoom(0, 0.001) == dem_max_zoom);
}

TEST_CASE("Mercator pixels are square on the ground") {
    // A conformal projection: one pixel covers the same distance east-west and north-south.
    // That is what lets the region be resampled in metres with no visible stretching.
    const geo_region region{46.0, 7.66, 20000};
    double lat_e, lon_e, lat_n, lon_n;
    region.offset(1000, 0, lat_e, lon_e);
    region.offset(0, 1000, lat_n, lon_n);
    const int z = 13;
    const double dx = mercator_x(lon_e, z) - mercator_x(region.lon, z);
    const double dy = mercator_y(region.lat, z) - mercator_y(lat_n, z);
    CHECK(dx == doctest::Approx(dy).epsilon(1e-3));
    CHECK(dx * metres_per_pixel(region.lat, z) == doctest::Approx(1000).epsilon(1e-3));
}

TEST_CASE("tiles_covering finds every tile a region samples") {
    const geo_region region{35.3606, 138.7274, 30000};
    const int z = 12;
    const auto tiles = tiles_covering(region, z);
    const std::set<tile_id> set(tiles.begin(), tiles.end());
    CHECK(set.size() == tiles.size());

    // 30 km at ~31 m per pixel is ~960 pixels: 4 or 5 tiles across, the same down.
    CHECK(tiles.size() >= 16);
    CHECK(tiles.size() <= 36);

    // Every vertex of a fine grid lands in a listed tile.
    for (int j = 0; j < 64; j++) {
        for (int i = 0; i < 64; i++) {
            double lat, lon;
            region.vertex(i, j, 64, lat, lon);
            REQUIRE(set.count(tile_at(lat, lon, z)) == 1);
        }
    }

    // Across the antimeridian the x index wraps instead of going negative.
    for (const tile_id& t : tiles_covering(geo_region{0, 179.99, 20000}, 8)) {
        CHECK(t.x >= 0);
        CHECK(t.x < 256);
    }
}

// A synthetic mosaic whose elevation is a known linear function of global pixel position. Bilinear
// filtering reproduces a linear function exactly, so any seam or off-by-half-a-pixel shows.
static tile_mosaic ramp_mosaic(int zoom, int x0, int y0, int count) {
    tile_mosaic mosaic(zoom);
    for (int ty = y0; ty < y0 + count; ty++) {
        for (int tx = x0; tx < x0 + count; tx++) {
            dem_grid tile(dem_tile_size, dem_tile_size);
            for (int y = 0; y < dem_tile_size; y++)
                for (int x = 0; x < dem_tile_size; x++) {
                    // Value at the pixel centre.
                    const double gx = tx * dem_tile_size + x + 0.5;
                    const double gy = ty * dem_tile_size + y + 0.5;
                    tile.at(x, y) = float(0.25 * (gx - x0 * 256.0) - 0.5 * (gy - y0 * 256.0));
                }
            mosaic.add(tx, ty, std::move(tile));
        }
    }
    return mosaic;
}

TEST_CASE("tile_mosaic stitches neighbouring tiles without seams") {
    const tile_mosaic mosaic = ramp_mosaic(10, 100, 200, 2);
    CHECK(mosaic.size() == 4);

    auto expected = [](double px, double py) { return 0.25 * (px - 25600) - 0.5 * (py - 51200); };

    double h = 0;
    // Inside one tile, on the seam between two, and at the corner where four meet.
    for (auto [px, py] : {std::pair{25650.3, 51210.7}, std::pair{25856.0, 51300.2},
                          std::pair{25855.6, 51455.9}, std::pair{25856.0, 51456.0}}) {
        REQUIRE(mosaic.sample(px, py, h));
        CHECK(h == doctest::Approx(expected(px, py)).epsilon(1e-5));
    }

    // Missing data is reported, not invented: the half pixel before the first centre and
    // anything off the mosaic need tiles that aren't there.
    CHECK_FALSE(mosaic.sample(25600.2, 51300, h));
    CHECK_FALSE(mosaic.sample(26200, 51300, h));
}

TEST_CASE("sample_region and heightfield_from_dem keep north at -z") {
    // A region whose northern half is high and southern half low, built from real tiles' worth of
    // ramp: elevation falls toward the south (larger pixel y).
    const int z = 12;
    const geo_region region{46.0, 7.66, 8000};
    const auto tiles = tiles_covering(region, z);
    int min_x = 1 << z, min_y = 1 << z, max_x = 0, max_y = 0;
    for (const tile_id& t : tiles) {
        min_x = std::min(min_x, t.x); max_x = std::max(max_x, t.x);
        min_y = std::min(min_y, t.y); max_y = std::max(max_y, t.y);
    }
    REQUIRE(max_x - min_x == max_y - min_y);
    const tile_mosaic mosaic = ramp_mosaic(z, min_x, min_y, max_x - min_x + 1);

    dem_grid dem;
    REQUIRE(sample_region(mosaic, region, 33, dem, 2));
    CHECK(dem.width == 33);
    // North (row 0) is higher than south; west (column 0) lower than east.
    CHECK(dem.at(16, 0) > dem.at(16, 32));
    CHECK(dem.at(0, 16) < dem.at(32, 16));

    // Across 8 km at ~21 m per pixel (z12 at 46 N), the ramp drops 0.5 m per pixel southward.
    const double pixels = 8000 / metres_per_pixel(46.0, z);
    CHECK(dem.at(16, 0) - dem.at(16, 32) == doctest::Approx(0.5 * pixels).epsilon(0.01));

    // Kilometre world units, 2x vertical exaggeration.
    const heightfield field = heightfield_from_dem(dem, 33, 8.0, 0.001, 2.0);
    CHECK(field.at(16, 0) == doctest::Approx(0.002 * dem.at(16, 0)));
    CHECK(field.vertex_position(16, 0).z() < field.vertex_position(16, 32).z());  // row 0 is north

    // Resampling to a different resolution interpolates the same surface.
    const heightfield coarse = heightfield_from_dem(dem, 17, 8.0, 0.001);
    CHECK(coarse.at(8, 8) == doctest::Approx(0.001 * dem.at(16, 16)).epsilon(1e-6));

    // A region outside the mosaic fails instead of returning zeros.
    dem_grid missing;
    CHECK_FALSE(sample_region(mosaic, geo_region{40.0, 7.66, 8000}, 9, missing));
}
