#include "dem_source.h"

#include "image_io.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#ifndef RT_DATA_DIR
#define RT_DATA_DIR "data"
#endif

namespace fs = std::filesystem;

const char* dem_attribution =
    "Elevation: AWS Terrain Tiles (Mapzen/Tilezen) from USGS 3DEP/SRTM/GMTED2010, EU-DEM, "
    "NOAA ETOPO1 and other sources; full credits in the README";

namespace {

constexpr const char* tile_url = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium";
constexpr size_t max_tiles = 400;  // About 40 MB; a guard against a mistyped --extent

std::string tile_path(const std::string& cache, const tile_id& t) {
    return cache + "/terrarium/" + std::to_string(t.z) + "/" + std::to_string(t.x) + "/" +
           std::to_string(t.y) + ".png";
}

// Downloads one tile with curl. It ships with Windows 10+, macOS and every Linux, which keeps an
// HTTP library out of the build. The file is written under a temporary name and renamed when
// complete, so an interrupted download never leaves a broken tile in the cache.
bool download_tile(const std::string& cache, const tile_id& t, std::string& error) {
    const std::string path = tile_path(cache, t);
    const std::string part = path + ".part";
    const std::string url = std::string(tile_url) + "/" + std::to_string(t.z) + "/" +
                            std::to_string(t.x) + "/" + std::to_string(t.y) + ".png";

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    // The URL is built from integers, and the cache path was checked for quotes.
    const std::string command = "curl -fsSL --retry 2 --connect-timeout 15 -o \"" + part + "\" \"" + url + "\"";
    if (std::system(command.c_str()) != 0) {
        fs::remove(part, ec);
        error = "download failed: " + url + " (is curl installed and the network up?)";
        return false;
    }
    fs::rename(part, path, ec);
    if (ec) {
        error = "could not save " + path + ": " + ec.message();
        return false;
    }
    return true;
}

bool fetch_region(dem_options& dem, int resolution, int thread_count, std::string& error) {
    if (dem.cache_dir.find('"') != std::string::npos) {
        error = "the tile cache path can't contain quotes";
        return false;
    }

    const geo_region& region = dem.region;
    const double spacing = region.extent_m / (resolution - 1);
    const int zoom = dem.zoom >= 0 ? dem.zoom : choose_zoom(region.lat, spacing);
    const std::vector<tile_id> tiles = tiles_covering(region, zoom);
    if (tiles.size() > max_tiles) {
        error = "the region needs " + std::to_string(tiles.size()) + " tiles at zoom " +
                std::to_string(zoom) + "; use a smaller --extent, --res or --zoom";
        return false;
    }

    std::vector<tile_id> missing;
    for (const tile_id& t : tiles)
        if (!fs::exists(tile_path(dem.cache_dir, t))) missing.push_back(t);

    std::fprintf(stderr, "DEM:     zoom %d (%.1f m/pixel), %zu tiles, %zu to download into %s/\n",
                 zoom, metres_per_pixel(region.lat, zoom), tiles.size(), missing.size(),
                 dem.cache_dir.c_str());

    // A handful of downloads at once; the server is fast and each tile is only ~100 KB.
    if (!missing.empty()) {
        std::atomic<size_t> next{0}, done{0};
        std::atomic<bool> failed{false};
        std::string first_error;
        std::atomic_flag error_taken = ATOMIC_FLAG_INIT;
        auto worker = [&] {
            for (size_t k; !failed && (k = next++) < missing.size();) {
                std::string e;
                if (!download_tile(dem.cache_dir, missing[k], e)) {
                    if (!error_taken.test_and_set()) first_error = e;
                    failed = true;
                }
                std::fprintf(stderr, "\rDEM:     downloaded %zu/%zu ", ++done, missing.size());
            }
        };
        std::vector<std::thread> pool;
        for (size_t k = 0; k < std::min<size_t>(8, missing.size()); k++) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
        std::fprintf(stderr, "\n");
        if (failed) {
            error = first_error;
            return false;
        }
    }

    tile_mosaic mosaic(zoom);
    for (const tile_id& t : tiles) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgb;
        const std::string path = tile_path(dem.cache_dir, t);
        if (!read_rgb_image(path, w, h, rgb, error))
            return false;
        if (w != dem_tile_size || h != dem_tile_size) {
            error = path + " is not a 256x256 tile; delete it to download it again";
            return false;
        }
        mosaic.add(t.x, t.y, decode_terrarium(rgb.data(), w, h));
    }

    auto grid = make_shared<dem_grid>();
    if (!sample_region(mosaic, region, resolution, *grid, thread_count)) {
        error = "the downloaded tiles don't cover the region";
        return false;
    }
    dem.grid = grid;
    return true;
}

bool load_bundled(dem_options& dem, const std::string& path, int resolution, std::string& error) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    if (!read_rgb_image(path, w, h, rgb, error))
        return false;
    if (w != h || w < 2) {
        error = path + " should be a square Terrarium image";
        return false;
    }
    dem.grid = make_shared<dem_grid>(decode_terrarium(rgb.data(), w, h));
    std::fprintf(stderr, "DEM:     bundled %dx%d grid from %s\n", w, h, path.c_str());
    if (resolution > w)
        std::fprintf(stderr, "DEM:     note: --res %d is finer than the bundled data; add --download "
                             "for full detail\n", resolution);
    return true;
}

}  // namespace


bool load_dem(terrain_options& opts, int thread_count, std::string& error) {
    dem_options& dem = opts.dem;
    if (!dem.requested())
        return true;

    std::string bundled;
    if (!dem.place.empty()) {
        const place* p = find_place(dem.place.c_str());
        if (!p) {
            error = "unknown place: " + dem.place + " (known:";
            for (const place& k : known_places) error += std::string(" ") + k.name;
            error += ")";
            return false;
        }
        dem.label       = p->label;
        dem.region      = geo_region{p->lat, p->lon, p->extent_km * 1000};
        dem.snow_line_m = p->snow_line_m;
        dem.palette     = p->palette;
        bundled = std::string(RT_DATA_DIR) + "/places/" + p->name + ".png";
    } else {
        char label[96];
        std::snprintf(label, sizeof label, "%.4f, %.4f", dem.lat, dem.lon);
        dem.label  = label;
        dem.region = geo_region{dem.lat, dem.lon, dem.extent_km * 1000};
        dem.snow_line_m = 0;  // No way to know; the terrain gets no snow
    }

    // Kilometre world units; the plain is left out because real terrain doesn't fade to a flat
    // base at its edges.
    opts.size  = dem.region.extent_m / 1000;
    opts.plain = false;

    const bool use_bundled = !bundled.empty() && !dem.download && fs::exists(bundled);
    if (use_bundled ? !load_bundled(dem, bundled, opts.resolution, error)
                    : !fetch_region(dem, opts.resolution, thread_count, error))
        return false;

    const auto [lo, hi] = dem.grid->range();
    std::fprintf(stderr, "DEM:     %s, %.1f km square, elevation %.0f to %.0f m\n",
                 dem.label.c_str(), opts.size, lo, hi);

    if (!dem.save_path.empty()) {
        // Rounded to 0.25 m, far below the source data's metre-scale accuracy. Resampling makes
        // the encoding's lowest byte pure noise, and dropping it shrinks the PNG by a third.
        dem_grid rounded = *dem.grid;
        for (float& m : rounded.metres) m = std::round(m * 4.0f) / 4.0f;
        const std::vector<uint8_t> rgb = encode_terrarium(rounded);
        std::vector<rgb8> pixels(rgb.size() / 3);
        for (size_t k = 0; k < pixels.size(); k++)
            pixels[k] = rgb8{rgb[3*k], rgb[3*k + 1], rgb[3*k + 2]};
        if (!write_image(pixels, dem.grid->width, dem.grid->height, dem.save_path, error))
            return false;
        std::fprintf(stderr, "DEM:     saved the region as Terrarium PNG to %s\n", dem.save_path.c_str());
    }
    return true;
}
