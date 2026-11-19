#include "analyze.h"

#include "rtheader.h"

#include "analysis/analysis_grid.h"
#include "analysis/horizon_map.h"
#include "analysis/scalar_map.h"
#include "analysis/sun_hours.h"
#include "analysis/viewshed.h"
#include "geometry/bvh.h"
#include "geometry/sphere.h"
#include "math/colormap.h"
#include "render/camera.h"
#include "render/framebuffer.h"
#include "render/renderer.h"

#include "cli_args.h"
#include "dem_source.h"
#include "image_io.h"
#include "scenes.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>


namespace {

struct analyze_options {
    std::string mode     = "sun-hours";  // sun-hours | viewshed
    std::string method   = "horizon";    // horizon | brute | compare
    std::string quantity = "hours";      // hours | irradiance

    sun_schedule schedule;
    bool   latitude_given = false;  // Otherwise real terrain uses its own latitude
    int    grid    = 256;   // Analysis cells per side
    int    threads = 0;

    double observer_u      = -1;     // Viewshed observer in 0..1 surface coordinates; <0: highest point
    double observer_v      = -1;
    double observer_height = 0.002;  // Eye height above the ground, in world units (2 m at 1 unit = 1 km)
    double target_height   = 0;

    bool        render = true;
    int         image_width       = 1200;
    int         samples_per_pixel = 64;
    uint64_t    seed              = 1;
    std::string output;              // Default: <mode>.png

    terrain_options terrain;
};

void print_usage() {
    std::cerr <<
        "Usage: rt-cli analyze [options]\n"
        "  --mode NAME        sun-hours | viewshed (default sun-hours)\n"
        "  --out PATH         rendered overlay, .png or .ppm (default <mode>.png). Also writes\n"
        "                     <name>-map.png, a flat top-down map, and <name>.csv, the raw values\n"
        "  --grid N           analysis cells per side (default 256)\n"
        "  --threads N        worker threads, 0 = all hardware threads (default 0)\n"
        "  --seed N           terrain seed (default 1)\n"
        "  --no-render        skip the 3D render; write only the map and the CSV\n"
        "  --width N          render width in pixels (default 1200)\n"
        "  --spp N            render samples per pixel (default 64)\n"
        "\n"
        "Sun-hours:\n"
        "  --lat D            latitude in degrees north (default: the real terrain's own, or\n"
        "                     46.5 for generated terrain)\n"
        "  --day-step N       sample every Nth day of the year (default 7)\n"
        "  --minutes N        time step through each day (default 15)\n"
        "  --quantity NAME    hours | irradiance: sun-hours per year, or direct-beam kWh/m^2 per\n"
        "                     year on the sloped ground (default hours)\n"
        "  --method NAME      horizon | brute | compare (default horizon). compare runs both,\n"
        "                     reports their agreement and speed, and keeps the horizon result\n"
        "\n"
        "Viewshed:\n"
        "  --observer U,V     observer position in 0..1 terrain coordinates, U west to east and\n"
        "                     V north to south (default: the highest point)\n"
        "  --eye F            observer height above the ground (default 0.002)\n"
        "  --target F         height above the ground that must be visible (default 0)\n"
        "\n"
        "Terrain (as for rendering):\n" << terrain_usage;
}

// Reads "U,V", both in 0..1.
bool read_observer(arg_cursor& args, analyze_options& opts) {
    std::string text;
    if (!args.value(text)) return false;
    const auto comma = text.find(',');
    if (comma == std::string::npos) return false;
    try {
        const double u = std::stod(text.substr(0, comma));
        const double v = std::stod(text.substr(comma + 1));
        if (u < 0 || u > 1 || v < 0 || v > 1) return false;
        opts.observer_u = u;
        opts.observer_v = v;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_options(int argc, char* argv[], analyze_options& opts) {
    // Analysis renders want a high sun behind the camera, so the overlay reads clearly instead
    // of being cut up by long shadows. --sun-elev and --sun-azim still override this.
    opts.terrain.sun_elevation = 58;
    opts.terrain.sun_azimuth   = 165;

    // From the high overview camera the plain is a flat grey slab filling the background. A
    // terrain tile against the sky reads more clearly, and nothing is analysed on the plain.
    opts.terrain.plain = false;

    arg_cursor args(argc, argv, 2);
    while (!args.done()) {
        const std::string arg = args.next();
        bool ok = true;

        const parse_result terrain = parse_terrain_option(arg, args, opts.terrain);
        if (terrain == parse_result::ok) continue;
        if (terrain == parse_result::bad_value) ok = false;
        else if (arg == "--mode")       ok = args.value(opts.mode);
        else if (arg == "--method")     ok = args.value(opts.method);
        else if (arg == "--quantity")   ok = args.value(opts.quantity);
        else if (arg == "--out")        ok = args.value(opts.output);
        else if (arg == "--lat")        ok = (opts.latitude_given = args.read_real(opts.schedule.latitude));
        else if (arg == "--day-step")   ok = args.read_int(opts.schedule.day_step, 1);
        else if (arg == "--minutes")    ok = args.read_real(opts.schedule.minutes, true);
        else if (arg == "--grid")       ok = args.read_int(opts.grid, 2);
        else if (arg == "--threads")    ok = args.read_int(opts.threads, 0);
        else if (arg == "--width")      ok = args.read_int(opts.image_width, 1);
        else if (arg == "--spp")        ok = args.read_int(opts.samples_per_pixel, 1);
        else if (arg == "--eye")        ok = args.read_real(opts.observer_height);
        else if (arg == "--target")     ok = args.read_real(opts.target_height);
        else if (arg == "--observer")   ok = read_observer(args, opts);
        else if (arg == "--no-render")  opts.render = false;
        else if (arg == "--seed") {
            long long n = 0;
            ok = args.value(n);
            opts.seed = uint64_t(n);
        }
        else if (arg == "--help") {
            print_usage();
            std::exit(0);
        }
        else {
            std::cerr << "Invalid argument: " << arg << "\n";
            return false;
        }

        if (!ok) {
            std::cerr << "Invalid value for " << arg << "\n";
            return false;
        }
    }

    if (opts.mode != "sun-hours" && opts.mode != "viewshed") {
        std::cerr << "Unknown mode: " << opts.mode << "\n";
        return false;
    }
    if (opts.method != "horizon" && opts.method != "brute" && opts.method != "compare") {
        std::cerr << "Unknown method: " << opts.method << "\n";
        return false;
    }
    if (opts.quantity != "hours" && opts.quantity != "irradiance") {
        std::cerr << "Unknown quantity: " << opts.quantity << "\n";
        return false;
    }

    if (opts.output.empty())
        opts.output = opts.mode + ".png";
    const auto dot = opts.output.rfind('.');
    const std::string extension = dot == std::string::npos ? "" : opts.output.substr(dot);
    if (extension != ".png" && extension != ".ppm") {
        std::cerr << "Output must be a .png or .ppm file: " << opts.output << "\n";
        return false;
    }

    opts.terrain.terrain.seed = opts.seed;
    return true;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string with_commas(uint64_t n) {
    std::string digits = std::to_string(n), out;
    for (size_t k = 0; k < digits.size(); k++) {
        if (k > 0 && (digits.size() - k) % 3 == 0) out += ',';
        out += digits[k];
    }
    return out;
}

// The output path without its extension: "out/sun.png" -> "out/sun".
std::string stem_of(const std::string& path) {
    return path.substr(0, path.rfind('.'));
}

// Lit from the north-west, high up: the cartographic convention, so slopes read as hills and
// not as hollows. Returns a brightness factor for a flat map pixel.
double hillshade(const vec3& normal) {
    static const vec3 light = sun_light::direction_from_elevation_azimuth(45, 315);
    return 0.55 + 0.45 * std::fmax(0.0, dot(normal, light));
}

rgb8 to_byte_color(const color& c) {
    auto byte = [](double x) { return uint8_t(255.0 * std::fmin(std::fmax(x, 0.0), 1.0) + 0.5); };
    return rgb8{byte(c.x()), byte(c.y()), byte(c.z())};
}

const color viewshed_seen   = color(1.00, 0.62, 0.20);  // sRGB
const color no_data         = color(0.35, 0.35, 0.35);

// Turns a value into an sRGB colour for the chosen analysis.
struct value_colors {
    std::string mode;
    bool irradiance = false;
    interval range;

    color operator()(double value) const {
        if (mode == "viewshed")
            return lerp_color(color(0.16, 0.17, 0.20), viewshed_seen, value);
        const double t = range.size() > 0 ? (value - range.min) / range.size() : 0.5;
        return irradiance ? inferno(t) : viridis(t);
    }

    static color lerp_color(const color& a, const color& b, double t) { return (1 - t)*a + t*b; }
};

// A top-down map, north up, one pixel per analysis cell, shaded with the terrain's relief.
bool write_map(const analysis_grid& grid, const scalar_map& map, const value_colors& colors,
               int marker_x, int marker_y, const std::string& path, std::string& error) {
    std::vector<rgb8> pixels;
    pixels.reserve(size_t(map.width()) * size_t(map.height()));

    // The marker is a square about 1% of the map across, so it stays visible on large grids.
    const int marker_radius = std::max(1, map.width() / 100);

    for (int y = 0; y < map.height(); y++) {
        for (int x = 0; x < map.width(); x++) {
            const double value = map.at(x, y);
            color c = std::isnan(value) ? no_data : colors(value);
            if (grid.valid(x, y))
                c = c * hillshade(grid.normal(x, y));
            if (std::abs(x - marker_x) <= marker_radius && std::abs(y - marker_y) <= marker_radius)
                c = color(0.9, 0.1, 0.1);
            pixels.push_back(to_byte_color(c));
        }
    }
    return write_image(pixels, map.width(), map.height(), path, error);
}

// Every valid cell as a row: world position, ground height and value. Long format, so it loads
// straight into a spreadsheet, pandas or a GIS. Real terrain also gets each cell's latitude and
// longitude, and its elevation in metres above sea level instead of world units.
bool write_csv(const analysis_grid& grid, const scalar_map& map, const std::string& column,
               const dem_options& dem, const std::string& path) {
    std::ofstream out(path);
    if (!out) return false;
    const bool geo = dem.loaded();
    out << (geo ? "lat,lon,x_km,z_km,elevation_m," : "x,z,elevation,") << column << "\n";
    char line[160];
    for (int y = 0; y < map.height(); y++) {
        for (int x = 0; x < map.width(); x++) {
            if (!grid.valid(x, y)) continue;
            const point3& p = grid.ground(x, y);
            if (geo) {
                double lat, lon;
                dem.region.offset(1000 * p.x(), -1000 * p.z(), lat, lon);  // North is -z
                std::snprintf(line, sizeof line, "%.6f,%.6f,%.4f,%.4f,%.1f,%.6g\n", lat, lon, p.x(),
                              p.z(), 1000 * p.y() / dem.exaggeration, map.at(x, y));
            } else {
                std::snprintf(line, sizeof line, "%.5f,%.5f,%.5f,%.6g\n", p.x(), p.z(), p.y(), map.at(x, y));
            }
            out << line;
        }
    }
    return bool(out);
}

}  // namespace


int run_analyze(int argc, char* argv[]) {
    analyze_options opts;
    if (!parse_options(argc, argv, opts)) {
        print_usage();
        return 1;
    }
    const bool irradiance = opts.quantity == "irradiance";
    const sun_quantity quantity = irradiance ? sun_quantity::irradiance : sun_quantity::hours;

    // Real elevation data, if asked for. Its latitude is where the sun is computed for, unless
    // --lat says otherwise.
    std::string error;
    if (!load_dem(opts.terrain, opts.threads, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    const dem_options& dem = opts.terrain.dem;
    if (dem.loaded() && !opts.latitude_given)
        opts.schedule.latitude = dem.region.lat;

    // Terrain, mesh and BVH. The same BVH serves the analysis and, later, the render.
    auto start = std::chrono::steady_clock::now();
    terrain_model model = build_terrain(opts.terrain);
    auto mesh = make_shared<bvh_node>(model.triangles);
    if (dem.loaded())
        std::fprintf(stderr, "Terrain: %s, %zu triangles, built in %.2f s\n",
                     dem.label.c_str(), model.triangles.objects.size(), seconds_since(start));
    else
        std::fprintf(stderr, "Terrain: seed %llu, %zu triangles, built in %.2f s\n",
                     (unsigned long long)opts.seed, model.triangles.objects.size(), seconds_since(start));

    start = std::chrono::steady_clock::now();
    analysis_grid grid(*mesh, opts.terrain.size, opts.terrain.size, opts.grid, opts.grid, opts.threads);
    std::fprintf(stderr, "Grid:    %dx%d cells placed on the surface in %.2f s\n",
                 grid.width(), grid.height(), seconds_since(start));

    scalar_map result(grid.width(), grid.height());
    std::string column;
    int marker_x = -1, marker_y = -1;
    point3 observer;

    if (opts.mode == "sun-hours") {
        const auto samples = sun_samples(opts.schedule);
        column = irradiance ? "irradiance_kwh_per_m2_year" : "sun_hours_per_year";
        std::fprintf(stderr, "Sun:     latitude %.2f, every %d days at %g-minute steps: %zu daytime sun positions\n",
                     opts.schedule.latitude, opts.schedule.day_step, opts.schedule.minutes, samples.size());

        scalar_map brute(1, 1);
        double brute_seconds = 0;
        uint64_t brute_rays = 0;
        if (opts.method != "horizon") {
            start = std::chrono::steady_clock::now();
            brute = sun_hours_brute_force(grid, *mesh, samples, quantity, opts.threads, &brute_rays);
            brute_seconds = seconds_since(start);
            std::fprintf(stderr, "Brute force:  %s rays in %.2f s\n", with_commas(brute_rays).c_str(), brute_seconds);
            result = brute;
        }

        if (opts.method != "brute") {
            start = std::chrono::steady_clock::now();
            horizon_map horizons(grid, *mesh, horizon_map::default_directions, opts.threads);
            const double build_seconds = seconds_since(start);

            start = std::chrono::steady_clock::now();
            result = sun_hours_from_horizons(grid, horizons, samples, quantity, opts.threads);
            const double lookup_seconds = seconds_since(start);

            std::fprintf(stderr, "Horizon map:  %s rays in %.2f s (%d directions), lookups %.2f s\n",
                         with_commas(horizons.rays()).c_str(), build_seconds, horizons.directions(),
                         lookup_seconds);

            if (opts.method == "compare") {
                const double scale = brute.range().max;
                double total_error = 0, worst = 0;
                uint64_t within_1 = 0, within_5 = 0, compared = 0;
                for (int y = 0; y < grid.height(); y++) {
                    for (int x = 0; x < grid.width(); x++) {
                        if (!grid.valid(x, y)) continue;
                        const double error = std::fabs(brute.at(x, y) - result.at(x, y));
                        total_error += error;
                        worst = std::fmax(worst, error);
                        compared++;
                        if (error <= 0.01 * scale) within_1++;
                        if (error <= 0.05 * scale) within_5++;
                    }
                }
                // With few sun positions brute force wins; say so rather than "0.3x faster".
                const double speedup = brute_seconds / (build_seconds + lookup_seconds);
                const double ray_ratio = double(brute_rays) / double(horizons.rays());
                std::fprintf(stderr,
                    "Compare:      horizon map is %.1fx %s with %.1fx %s rays\n"
                    "              mean difference %.2f%% of the maximum, worst %.1f%%\n"
                    "              %.1f%% of cells within 1%%, %.1f%% within 5%%\n",
                    speedup >= 1 ? speedup : 1 / speedup, speedup >= 1 ? "faster" : "slower",
                    ray_ratio >= 1 ? ray_ratio : 1 / ray_ratio, ray_ratio >= 1 ? "fewer" : "more",
                    100 * total_error / double(compared) / scale, 100 * worst / scale,
                    100.0 * double(within_1) / double(compared), 100.0 * double(within_5) / double(compared));
            }
        }

        const interval range = result.range();
        std::fprintf(stderr, "Result:       %s from %.0f to %.0f, mean %.0f %s\n",
                     irradiance ? "irradiance" : "sun-hours", range.min, range.max, result.mean(),
                     irradiance ? "kWh/m^2 per year" : "hours per year");
    } else {
        // Viewshed: put the observer on the chosen cell, or on the highest point.
        if (opts.observer_u >= 0) {
            marker_x = std::min(int(opts.observer_u * grid.width()), grid.width() - 1);
            marker_y = std::min(int(opts.observer_v * grid.height()), grid.height() - 1);
            if (!grid.valid(marker_x, marker_y)) {
                std::cerr << "No ground under the observer position\n";
                return 1;
            }
        } else if (!grid.highest_cell(marker_x, marker_y)) {
            std::cerr << "The terrain has no ground to stand on\n";
            return 1;
        }
        observer = grid.ground(marker_x, marker_y) + vec3(0, opts.observer_height, 0);
        column = "visible";

        start = std::chrono::steady_clock::now();
        uint64_t rays = 0;
        result = viewshed(grid, *mesh, observer, opts.target_height, opts.threads, &rays);
        std::fprintf(stderr, "Viewshed:     observer at (%.3f, %.3f, %.3f), %s rays in %.2f s\n",
                     observer.x(), observer.y(), observer.z(), with_commas(rays).c_str(), seconds_since(start));
        std::fprintf(stderr, "Result:       %.1f%% of the terrain is visible\n", 100 * result.mean());
    }

    // Flat map and CSV.
    const std::string stem = stem_of(opts.output);
    const std::string map_path = stem + "-map.png", csv_path = stem + ".csv";
    value_colors colors{opts.mode, irradiance, result.range()};

    if (!write_map(grid, result, colors, marker_x, marker_y, map_path, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!write_csv(grid, result, column, opts.terrain.dem, csv_path)) {
        std::cerr << "failed to write " << csv_path << "\n";
        return 1;
    }
    std::fprintf(stderr, "Wrote %s and %s\n", map_path.c_str(), csv_path.c_str());
    if (dem.loaded())
        std::fprintf(stderr, "%s\n", dem_attribution);

    if (!opts.render)
        return 0;

    // 3D render with the result painted onto the terrain.
    const bool is_viewshed = opts.mode == "viewshed";
    model.material->overlay = [&result, &colors, is_viewshed](double u, double v, const color& base) {
        const double value = result.sample(u, v);
        if (std::isnan(value))
            return base;
        if (is_viewshed) {
            // Keep the terrain's own colours, warmed where visible and darkened where hidden.
            const color seen = 0.4*base + 0.6*srgb_to_linear(viewshed_seen);
            return (1 - value)*(0.35*base) + value*seen;
        }
        return srgb_to_linear(colors(value));
    };

    camera cam;
    hittable_list world(mesh);
    for (const auto& wall : model.skirt.objects)
        world.add(wall);
    frame_terrain(cam, opts.terrain, model.heights, world, terrain_view::overview);

    // A neutral white sun and a paler sky, so the colormap's colours come through unchanged.
    cam.sun.intensity = color(1, 1, 1) * 3.4;
    cam.sky.horizon = color(0.70, 0.74, 0.80);
    cam.sky.zenith  = color(0.34, 0.46, 0.70);
    cam.sky.glow_strength = 0;

    if (is_viewshed) {
        // Mark the observer with a red ball, sized to be visible from the overview camera.
        const double radius = 0.012 * opts.terrain.size;
        world.add(make_shared<sphere>(observer + vec3(0, radius, 0), radius,
                                      make_shared<lambertian>(color(0.8, 0.05, 0.05))));
    }

    cam.image_width = opts.image_width;
    cam.initialize();
    bvh_node scene(world);
    framebuffer fb(cam.image_width, cam.height());

    renderer r;
    r.thread_count = opts.threads;
    r.seed = opts.seed;
    r.on_progress = [](int done, int total) { std::fprintf(stderr, "\rRender: %d/%d tiles ", done, total); };

    start = std::chrono::steady_clock::now();
    r.render_pass(cam, scene, fb, opts.samples_per_pixel);
    const double render_seconds = seconds_since(start);

    if (!write_image(fb, opts.output, error)) {
        std::cerr << "\n" << error << "\n";
        return 1;
    }
    std::fprintf(stderr, "\rRendered %dx%d at %d spp in %.2f s -> %s\n", fb.width(), fb.height(),
                 fb.samples_per_pixel(), render_seconds, opts.output.c_str());
    return 0;
}
