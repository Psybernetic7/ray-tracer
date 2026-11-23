#ifndef STUDIO_H
#define STUDIO_H

#include "rtheader.h"

#include "analysis/analysis_grid.h"
#include "analysis/horizon_map.h"
#include "analysis/overlay.h"
#include "analysis/scalar_map.h"
#include "analysis/sun_hours.h"
#include "analysis/viewshed.h"
#include "geometry/bvh.h"
#include "geometry/sphere.h"
#include "lighting/solar.h"
#include "render/camera.h"
#include "render/framebuffer.h"
#include "render/renderer.h"
#include "scenes.h"
#include "terrain/dem.h"
#include "terrain/places.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>


// One interactive session of the browser app: a terrain, a camera orbiting it, a sun, and the
// current analysis, all kept between calls so that moving a slider only redoes the work that
// slider affects.
//
// Nothing here knows about the browser. The JavaScript bindings (bindings.cc) are a thin layer
// over this class, and the unit tests drive it natively, so the logic is tested without a
// browser. Work happens on the calling thread plus `threads - 1` helpers, exactly as in the CLI.
//
// What depends on what, and so what each change throws away:
//   terrain   -> mesh BVH -> analysis grid -> horizon map -> sun-hours / irradiance map
//                                           -> viewshed map (also depends on the observer)
//   terrain, mode, observer -> scene BVH (mesh + skirt + plain + observer marker)
//   anything visible        -> the accumulated image
enum class studio_mode { render, sun_hours, irradiance, viewshed };

// What lies under a point of the image.
struct pick_result {
    bool   hit        = false;  // Anything at all, sky excluded
    bool   on_terrain = false;  // The terrain surface itself, not its walls or the plain
    double u = 0, v = 0;        // Surface coordinates, 0..1 west to east and north to south
    double x = 0, y = 0, z = 0; // World position, in kilometres
    double elevation_m = 0;     // Height above sea level (real terrain) or above the base
    bool   geo = false;         // Real terrain: lat and lon are set
    double lat = 0, lon = 0;
    double value = std::nan("");  // The current analysis there, NaN in render mode
};

// Timings and sizes, for the app's stats panel.
struct studio_stats {
    size_t   triangles        = 0;
    double   terrain_seconds  = 0;  // Noise or DEM resampling, plus meshing
    double   bvh_seconds      = 0;
    double   grid_seconds     = 0;  // Dropping the analysis points onto the surface
    double   horizon_seconds  = 0;  // Building the horizon map
    uint64_t horizon_rays     = 0;
    double   analysis_seconds = 0;  // The last sun-hours lookup or viewshed
    uint64_t analysis_rays    = 0;
    size_t   sun_positions    = 0;
    double   pass_seconds     = 0;  // The last render pass
};

class studio {
  public:
    explicit studio(int threads) : thread_count(threads < 1 ? 1 : threads) {
        render.thread_count = thread_count;
        render.seed = 1;
    }

    // The render overlay holds a pointer back to this object, so it must stay where it is.
    studio(const studio&) = delete;
    studio& operator=(const studio&) = delete;

    // --- Terrain ------------------------------------------------------------------------

    // A fractal mountain range from a seed.
    void generate(const terrain_params& params, int resolution, double peak_height) {
        terrain_options opts;
        opts.resolution  = clamp_resolution(resolution);
        opts.peak_height = peak_height;
        opts.terrain     = params;
        // No plain: from an orbiting camera it fills the whole background. The terrain gets
        // walls instead, like real terrain (see build), and floats as a relief block.
        opts.plain       = false;
        build(opts);
    }

    // One of the bundled places, from its decoded Terrarium grid.
    void load_place(const place& p, shared_ptr<const dem_grid> grid, int resolution) {
        terrain_options opts = dem_terrain(p.label, geo_region{p.lat, p.lon, p.extent_km * 1000},
                                           std::move(grid), resolution);
        opts.dem.snow_line_m = p.snow_line_m;
        opts.dem.palette     = p.palette;
        build(opts);
    }

    // Any square of the Earth from downloaded tiles, in three steps: ask which tiles cover
    // it, add each one as it arrives, then stitch and resample. Snow starts at a rough
    // climatic snow line for the latitude (see typical_snow_line_m).
    std::vector<tile_id> begin_region(double lat, double lon, double extent_km, int resolution) {
        pending_region = geo_region{lat, lon, extent_km * 1000};
        pending_resolution = clamp_resolution(resolution);
        const double spacing = pending_region.extent_m / (pending_resolution - 1);
        mosaic.emplace(choose_zoom(lat, spacing));
        return tiles_covering(pending_region, mosaic->zoom());
    }

    void add_tile(int x, int y, dem_grid tile) {
        if (mosaic) mosaic->add(x, y, std::move(tile));
    }

    bool finish_region(const std::string& label, std::string& error) {
        if (!mosaic) {
            error = "no region was started";
            return false;
        }
        auto grid = make_shared<dem_grid>();
        const bool ok = sample_region(*mosaic, pending_region, pending_resolution, *grid, thread_count);
        mosaic.reset();
        if (!ok) {
            error = "the tiles don't cover the region";
            return false;
        }
        terrain_options o = dem_terrain(label, pending_region, grid, pending_resolution);
        o.dem.snow_line_m = typical_snow_line_m(pending_region.lat);
        build(o);
        return true;
    }

    bool has_terrain() const { return mesh != nullptr; }
    bool is_real() const { return opts.dem.loaded(); }
    const std::string& label() const { return opts.dem.label; }
    double size_km() const { return opts.size; }

    // Lowest and highest ground, in metres.
    double min_elevation_m() const { return to_metres(heights.min); }
    double max_elevation_m() const { return to_metres(heights.max); }

    // The latitude the sun is computed for: the place's own, or the one set for fractal terrain.
    double latitude() const { return is_real() ? opts.dem.region.lat : fractal_latitude; }

    // --- View ---------------------------------------------------------------------------

    void set_size(int width, int height) {
        width = std::max(width, 1);
        height = std::max(height, 1);
        if (width == image_width && height == image_height) return;
        image_width = width;
        image_height = height;
        view_changed();
    }

    // The camera circles a point above the middle of the terrain. Azimuth is the compass
    // bearing the camera stands at (looking back toward the middle), elevation how far above
    // the horizontal it looks down from, and distance a zoom factor: at 1 the whole terrain
    // just fits the image, whatever the image's shape.
    void set_orbit(double azimuth_degrees, double elevation_degrees, double distance) {
        orbit_azimuth   = azimuth_degrees;
        orbit_elevation = std::clamp(elevation_degrees, 2.0, 89.0);
        orbit_distance  = std::clamp(distance, 0.2, 5.0);
        view_changed();
    }

    // The sun for a day of the year and a local solar time, at latitude().
    solar_position set_sun_time(double day_of_year, double hour) {
        sun_day = day_of_year;
        sun_hour = hour;
        if (current_mode == studio_mode::render) view_changed();  // Analysis views use a fixed light
        return sun_now();
    }

    solar_position sun_now() const { return sun_position(latitude(), sun_day, sun_hour); }

    // --- Analysis -----------------------------------------------------------------------

    void set_mode(studio_mode m) {
        if (m == current_mode) return;
        current_mode = m;
        scene_dirty = true;  // The observer marker comes and goes with viewshed mode
        analysis_dirty = true;
        view_changed();
    }
    studio_mode mode() const { return current_mode; }

    // Sun-hours are computed for latitude(); this sets it for fractal terrain.
    void set_latitude(double degrees) {
        degrees = std::clamp(degrees, -89.0, 89.0);
        if (degrees == fractal_latitude) return;
        fractal_latitude = degrees;
        if (is_real()) return;
        view_changed();  // The sun in the render moves too
        sun_analysis_changed();
    }

    // Analysis cells per side. A change rebuilds the grid and the horizon map.
    void set_grid_size(int cells) {
        cells = std::clamp(cells, 16, 512);
        if (cells == grid_cells) return;
        grid_cells = cells;
        grid.reset();
        horizons.reset();
        result.reset();
        observer_cell_x = observer_cell_y = -1;
        if (current_mode != studio_mode::render) {
            analysis_dirty = true;
            scene_dirty = true;
            view_changed();
        }
    }

    // The viewshed observer, at surface coordinates u,v. False if there is no ground there.
    bool set_observer(double u, double v) {
        if (!mesh) return false;
        ensure_grid();
        const int x = std::min(int(std::clamp(u, 0.0, 1.0) * grid->width()), grid->width() - 1);
        const int y = std::min(int(std::clamp(v, 0.0, 1.0) * grid->height()), grid->height() - 1);
        if (!grid->valid(x, y)) return false;
        observer_cell_x = x;
        observer_cell_y = y;
        observer_changed();
        return true;
    }

    void set_observer_height_m(double metres) {
        metres = std::clamp(metres, 0.0, 5000.0);
        if (metres == observer_height_m) return;
        observer_height_m = metres;
        observer_changed();
    }

    // The observer's surface coordinates, once a viewshed has placed one.
    bool observer(double& u, double& v) const {
        if (observer_cell_x < 0 || !grid) return false;
        u = (observer_cell_x + 0.5) / grid->width();
        v = (observer_cell_y + 0.5) / grid->height();
        return true;
    }

    // True when the next render would first have to compute an analysis: the app shows a
    // "computing" message before calling run_analysis().
    bool analysis_pending() const { return mesh && current_mode != studio_mode::render && analysis_dirty; }

    // Computes whatever the current mode needs that isn't computed yet.
    void run_analysis() {
        if (!analysis_pending()) return;
        ensure_grid();
        stats.analysis_rays = 0;

        if (current_mode == studio_mode::viewshed) {
            if (observer_cell_x < 0 && !grid->highest_cell(observer_cell_x, observer_cell_y)) {
                result.reset();
                analysis_dirty = false;
                return;
            }
            const auto start = clock::now();
            result = viewshed(*grid, *mesh, observer_point(), 0, thread_count, &stats.analysis_rays);
            stats.analysis_seconds = seconds_since(start);
        } else {
            if (!horizons) {
                const auto start = clock::now();
                horizons.emplace(*grid, *mesh, horizon_map::default_directions, thread_count);
                stats.horizon_seconds = seconds_since(start);
                stats.horizon_rays = horizons->rays();
            }
            sun_schedule schedule;
            schedule.latitude = latitude();
            const auto samples = sun_samples(schedule);
            stats.sun_positions = samples.size();

            // No rays here at all: every sun position is a lookup in the horizon map, which is
            // why moving the latitude slider is instant once the map exists.
            const auto start = clock::now();
            const sun_quantity quantity = current_mode == studio_mode::irradiance
                ? sun_quantity::irradiance : sun_quantity::hours;
            result = sun_hours_from_horizons(*grid, *horizons, samples, quantity, thread_count);
            stats.analysis_seconds = seconds_since(start);
        }

        colors = overlay_colors{overlay_kind_of(current_mode), result->range()};
        analysis_dirty = false;
        scene_dirty = true;  // Hang the new result on the material, and move the observer marker
        view_changed();
    }

    // The current analysis result, if there is one, and its summary.
    bool has_result() const { return result.has_value() && current_mode != studio_mode::render; }
    interval result_range() const { return result ? result->range() : interval(); }
    double result_mean() const { return result ? result->mean() : 0.0; }
    const overlay_colors& result_colors() const { return colors; }

    // --- Rendering ----------------------------------------------------------------------

    // Adds `samples` samples per pixel to the image, computing whatever is out of date first.
    void render_pass(int samples) {
        if (!mesh) return;
        run_analysis();
        if (scene_dirty) build_scene();
        if (camera_dirty) setup_camera();

        const auto start = clock::now();
        render.render_pass(cam, *scene, *fb, std::max(samples, 1));
        stats.pass_seconds = seconds_since(start);
        pixels_stale = true;
    }

    int width() const  { return fb ? fb->width() : 0; }
    int height() const { return fb ? fb->height() : 0; }
    int samples() const { return fb ? fb->samples_per_pixel() : 0; }

    // The image as 8-bit RGBA, row by row from the top: what a browser canvas takes.
    const std::vector<uint8_t>& rgba() {
        if (!fb) {
            rgba_pixels.clear();
            return rgba_pixels;
        }
        if (pixels_stale) {
            rgba_pixels.resize(size_t(fb->width()) * size_t(fb->height()) * 4);
            size_t k = 0;
            for (int j = 0; j < fb->height(); j++) {
                for (int i = 0; i < fb->width(); i++) {
                    const rgb8 p = to_rgb8(fb->pixel(i, j));
                    rgba_pixels[k++] = p.r;
                    rgba_pixels[k++] = p.g;
                    rgba_pixels[k++] = p.b;
                    rgba_pixels[k++] = 255;
                }
            }
            pixels_stale = false;
        }
        return rgba_pixels;
    }

    // What is under a point of the image; fx and fy run 0..1 across and down it.
    pick_result pick(double fx, double fy) {
        pick_result out;
        if (!mesh) return out;
        if (scene_dirty) build_scene();
        if (camera_dirty) setup_camera();

        hit_record rec;
        const ray r = cam.center_ray(fx * fb->width(), fy * fb->height());
        if (!scene->hit(r, interval(0.001, infinity), rec)) return out;

        out.hit = true;
        out.on_terrain = rec.mat == model->material.get();
        out.x = rec.p.x();
        out.y = rec.p.y();
        out.z = rec.p.z();
        out.elevation_m = to_metres(rec.p.y());
        if (!out.on_terrain) return out;

        out.u = rec.u;
        out.v = rec.v;
        if (is_real()) {
            out.geo = true;
            opts.dem.region.offset(1000 * rec.p.x(), -1000 * rec.p.z(), out.lat, out.lon);  // North is -z
        }
        if (has_result() && !analysis_dirty)
            out.value = result->sample(rec.u, rec.v);
        return out;
    }

    const studio_stats& statistics() const { return stats; }
    int threads() const { return thread_count; }

  private:
    using clock = std::chrono::steady_clock;

    int thread_count;
    terrain_options opts;
    double fractal_latitude = 46.5;

    // The terrain and everything built from it.
    std::optional<terrain_model> model;
    shared_ptr<bvh_node> mesh;   // Terrain triangles only: what analysis rays are tested against
    interval heights;
    std::optional<analysis_grid> grid;
    std::optional<horizon_map> horizons;
    int grid_cells = 160;

    // Tiles arriving for a region.
    std::optional<tile_mosaic> mosaic;
    geo_region pending_region;
    int pending_resolution = 256;

    // Analysis.
    studio_mode current_mode = studio_mode::render;
    std::optional<scalar_map> result;
    overlay_colors colors;
    bool analysis_dirty = true;
    int observer_cell_x = -1, observer_cell_y = -1;
    double observer_height_m = 2;

    // View and image.
    double orbit_azimuth = 152, orbit_elevation = 32, orbit_distance = 1.0;
    double sun_day = 172, sun_hour = 15.5;  // A midsummer afternoon
    int image_width = 640, image_height = 360;
    camera cam;
    shared_ptr<hittable> scene;    // The mesh plus everything only the render needs
    std::optional<framebuffer> fb;
    renderer render;
    std::vector<uint8_t> rgba_pixels;
    bool scene_dirty = true, camera_dirty = true, pixels_stale = true;

    studio_stats stats;

    static int clamp_resolution(int resolution) { return std::clamp(resolution, 16, 1024); }

  public:
    // A rough altitude of permanent snow for a latitude, in metres: interpolated between
    // typical values, highest in the dry subtropics (the Himalaya, the Andes), lower in the wet
    // tropics, about 2,900 m in the Alps and near sea level in the Arctic. Real snow lines vary
    // by hundreds of metres with climate and aspect; this only keeps an unknown place from
    // showing grass on an 8,000 m summit.
    static double typical_snow_line_m(double latitude) {
        static const double lat[] = {0, 25, 46, 60, 75, 90};
        static const double m[]   = {4800, 5500, 2900, 1500, 400, 0};
        const double a = std::fabs(latitude);
        for (int k = 1; k < 6; k++)
            if (a <= lat[k]) return lerp(m[k - 1], m[k], (a - lat[k - 1]) / (lat[k] - lat[k - 1]));
        return 0;
    }

  private:

    static double seconds_since(clock::time_point start) {
        return std::chrono::duration<double>(clock::now() - start).count();
    }

    // World heights are kilometres; real terrain may be vertically exaggerated.
    double to_metres(double world_y) const {
        return 1000 * world_y / (is_real() ? opts.dem.exaggeration : 1.0);
    }

    static overlay_kind overlay_kind_of(studio_mode m) {
        if (m == studio_mode::viewshed)   return overlay_kind::viewshed;
        if (m == studio_mode::irradiance) return overlay_kind::irradiance;
        return overlay_kind::sun_hours;
    }

    // Real terrain, set up the way the CLI's load_dem does it: one world unit per kilometre,
    // and no plain, because real ground doesn't fade to a flat base at its edges.
    static terrain_options dem_terrain(const std::string& label, const geo_region& region,
                                       shared_ptr<const dem_grid> grid, int resolution) {
        terrain_options o;
        o.resolution = clamp_resolution(resolution);
        o.dem.label  = label;
        o.dem.region = region;
        o.dem.grid   = std::move(grid);
        o.size  = region.extent_m / 1000;
        o.plain = false;
        return o;
    }

    void build(const terrain_options& new_opts) {
        opts = new_opts;
        mosaic.reset();

        auto start = clock::now();
        model.emplace(build_terrain(opts));
        if (model->skirt.objects.empty()) {
            // build_terrain only walls in real terrain; the fractal range gets the same block.
            const double base = model->heights.min - std::fmax(0.03 * opts.size, 0.1 * model->heights.size());
            model->skirt = model->field.skirt(base, make_shared<lambertian>(color(0.20, 0.15, 0.11)));
        }
        stats = studio_stats{};
        stats.terrain_seconds = seconds_since(start);
        stats.triangles = model->triangles.objects.size();
        heights = model->heights;

        start = clock::now();
        mesh = make_shared<bvh_node>(model->triangles);
        stats.bvh_seconds = seconds_since(start);

        grid.reset();
        horizons.reset();
        result.reset();
        observer_cell_x = observer_cell_y = -1;
        analysis_dirty = true;
        scene_dirty = true;
        view_changed();
    }

    void ensure_grid() {
        if (grid) return;
        const auto start = clock::now();
        grid.emplace(*mesh, opts.size, opts.size, grid_cells, grid_cells, thread_count);
        stats.grid_seconds = seconds_since(start);
    }

    point3 observer_point() const {
        return grid->ground(observer_cell_x, observer_cell_y) + vec3(0, observer_height_m / 1000, 0);
    }

    void sun_analysis_changed() {
        if (current_mode == studio_mode::sun_hours || current_mode == studio_mode::irradiance) {
            analysis_dirty = true;
            view_changed();
        }
    }

    void observer_changed() {
        if (current_mode != studio_mode::viewshed) return;
        analysis_dirty = true;
        scene_dirty = true;  // The marker moves
        view_changed();
    }

    // Anything that changes the picture starts the image over.
    void view_changed() {
        camera_dirty = true;
        pixels_stale = true;
    }

    // The scene the camera sees: the mesh BVH as one object, the real terrain's walls, the
    // plain, and in viewshed mode a red ball on the observer. Rebuilding this is cheap, since
    // the half-million triangles stay inside the mesh BVH, which is reused as it is.
    void build_scene() {
        hittable_list world(mesh);
        for (const auto& wall : model->skirt.objects)
            world.add(wall);

        // frame_terrain adds the plain and sets the sky; set_lighting and setup_camera then
        // replace its fixed camera and sun with the orbit and the chosen time.
        frame_terrain(cam, opts, heights, world, terrain_view::overview);

        if (current_mode == studio_mode::viewshed && !analysis_dirty && observer_cell_x >= 0) {
            const double radius = 0.012 * opts.size;
            world.add(make_shared<sphere>(observer_point() + vec3(0, radius, 0), radius,
                                          make_shared<lambertian>(color(0.8, 0.05, 0.05))));
        }

        scene = make_shared<bvh_node>(world);

        // The overlay reads `result` from every render thread. It is only ever replaced between
        // passes, never during one.
        if (current_mode == studio_mode::render || !result) {
            model->material->overlay = nullptr;
        } else {
            model->material->overlay = [this](double u, double v, const color& base) {
                return overlay_albedo(colors, result->sample(u, v), base);
            };
        }
        scene_dirty = false;
        camera_dirty = true;
    }

    void set_lighting() {
        cam.sky.horizon = color(0.62, 0.68, 0.76);
        cam.sky.zenith  = color(0.16, 0.33, 0.72);

        if (current_mode != studio_mode::render) {
            // The CLI's analysis light: high, white and behind the camera, so the colormap's
            // colours come through unchanged instead of being cut up by long shadows.
            cam.sun.enabled   = true;
            cam.sun.direction = sun_light::direction_from_elevation_azimuth(58, orbit_azimuth + 13);
            cam.sun.intensity = color(1, 1, 1) * 3.4;
            cam.sky.horizon = color(0.70, 0.74, 0.80);
            cam.sky.zenith  = color(0.34, 0.46, 0.70);
            cam.sky.glow_strength = 0;
            return;
        }

        const solar_position sun = sun_now();
        // Twilight: the sky dims smoothly as the sun drops below the horizon, and the sun's
        // own light fades out over its last few degrees.
        const double daylight = 0.05 + 0.95 * smoothstep(-8.0, 6.0, sun.elevation);
        cam.sky.horizon = cam.sky.horizon * daylight;
        cam.sky.zenith  = cam.sky.zenith * daylight;
        cam.sky.glow_strength = 0.9 * daylight;

        cam.sun.enabled = sun.elevation > -0.5;
        cam.sun.direction = sun.direction;
        // Warm and strong near the horizon, whiter and gentler as the sun climbs. The CLI's
        // brightness suits its low sun; a high sun at that strength burns the snow out to white.
        const double high = smoothstep(5.0, 45.0, sun.elevation);
        const color tint = (1 - high) * color(1.0, 0.80, 0.58) + high * color(1.0, 0.95, 0.88);
        cam.sun.intensity = tint * 7.0 * (1 - 0.55 * high) * smoothstep(-0.5, 3.0, sun.elevation);
    }

    void setup_camera() {
        set_lighting();

        const double size = opts.size;
        const point3 target(0, heights.min + 0.3 * heights.size(), 0);
        const double el = degrees_to_radians(orbit_elevation);
        const double az = degrees_to_radians(orbit_azimuth);

        // How far back the whole terrain fits: a sphere a little smaller than the block's
        // corners (which may then be clipped slightly), inside the narrower of the two
        // fields of view.
        const double vfov = 36;
        const double aspect = double(image_width) / image_height;
        const double half_v = degrees_to_radians(vfov / 2);
        const double half_h = std::atan(std::tan(half_v) * aspect);
        const double radius = 0.62 * size;
        const double d = orbit_distance * radius / std::sin(std::fmin(half_v, half_h));
        // Compass bearing to world axes: north is -z, east is +x.
        cam.lookfrom = target + d * vec3(std::cos(el) * std::sin(az), std::sin(el), -std::cos(el) * std::cos(az));
        cam.lookat   = target;
        cam.vup      = vec3(0, 1, 0);
        cam.vfov     = vfov;
        cam.max_depth = 5;  // The sun is sampled directly; a few bounces of sky fill are enough
        cam.defocus_angle = 0;

        cam.aspect_ratio = double(image_width) / image_height;
        cam.image_width  = image_width;
        cam.initialize();

        if (!fb || fb->width() != cam.image_width || fb->height() != cam.height())
            fb.emplace(cam.image_width, cam.height());
        else
            fb->clear();
        camera_dirty = false;
        pixels_stale = true;
    }
};

#endif
