#ifndef SCENES_H
#define SCENES_H

#include "rtheader.h"

#include "geometry/heightfield.h"
#include "geometry/hittable_list.h"
#include "geometry/sphere.h"
#include "geometry/triangle.h"
#include "render/camera.h"
#include "render/material.h"
#include "terrain/dem.h"
#include "terrain/places.h"
#include "terrain/terrain.h"
#include "terrain/terrain_material.h"

#include <string>


// The final scene from "Ray Tracing in One Weekend": a ground sphere, three large feature
// spheres and a field of small spheres with random materials.
inline hittable_list showcase_scene(camera& cam) {
    hittable_list world;

    auto ground_material = make_shared<lambertian>(color(0.5, 0.5, 0.5));
    world.add(make_shared<sphere>(point3(0,-1000,0), 1000, ground_material));

    for (int a = -11; a < 11; a++) {
        for (int b = -11; b < 11; b++) {
            auto choose_mat = random_double();
            point3 center(a + 0.9*random_double(), 0.2, b + 0.9*random_double());

            if ((center - point3(4, 0.2, 0)).length() > 0.9) {
                shared_ptr<material> sphere_material;

                if (choose_mat < 0.8) {
                    // diffuse
                    auto albedo = color::random() * color::random();
                    sphere_material = make_shared<lambertian>(albedo);
                    world.add(make_shared<sphere>(center, 0.2, sphere_material));
                } else if (choose_mat < 0.95) {
                    // metal
                    auto albedo = color::random(0.5, 1);
                    auto fuzz = random_double(0, 0.5);
                    sphere_material = make_shared<metal>(albedo, fuzz);
                    world.add(make_shared<sphere>(center, 0.2, sphere_material));
                } else {
                    // glass
                    sphere_material = make_shared<dielectric>(1.5);
                    world.add(make_shared<sphere>(center, 0.2, sphere_material));
                }
            }
        }
    }

    auto material1 = make_shared<dielectric>(1.5);
    world.add(make_shared<sphere>(point3(0, 1, 0), 1.0, material1));

    auto material2 = make_shared<lambertian>(color(0.4, 0.2, 0.1));
    world.add(make_shared<sphere>(point3(-4, 1, 0), 1.0, material2));

    auto material3 = make_shared<metal>(color(0.7, 0.6, 0.5), 0.0);
    world.add(make_shared<sphere>(point3(4, 1, 0), 1.0, material3));


    cam.aspect_ratio = 16.0 / 9.0;
    cam.max_depth    = 50;

    cam.vfov     = 20;
    cam.lookfrom = point3(13,2,3);
    cam.lookat   = point3(0,0,0);
    cam.vup      = vec3(0,1,0);

    cam.defocus_angle = 0.6;
    cam.focus_dist    = 10.0;

    return world;
}


// Real elevation data in place of noise: a named place, or a square around a latitude and
// longitude. The command line fills in the request; load_dem (dem_source.h) resolves it and
// loads the grid before the terrain is built.
struct dem_options {
    std::string place;               // --place NAME
    bool        has_center = false;  // --center LAT,LON
    double      lat = 0, lon = 0;
    double      extent_km    = 20;   // --extent KM, with --center
    int         zoom         = -1;   // --zoom N; <0 picks one to match the grid spacing
    double      exaggeration = 1.0;  // --exaggerate F: vertical stretch
    bool        download     = false;         // --download: fetch tiles even for a bundled place
    std::string cache_dir    = "tile-cache";  // --tile-cache DIR
    std::string save_path;                    // --save-dem PATH: write the region as Terrarium PNG

    // Resolved by load_dem.
    std::string     label;
    geo_region      region;
    double          snow_line_m = 0;
    terrain_palette palette = terrain_palette::alpine;
    shared_ptr<const dem_grid> grid;

    bool requested() const { return !place.empty() || has_center; }
    bool loaded() const    { return grid != nullptr; }
};

// How the terrain scene is built and framed. The defaults are the README hero image.
struct terrain_options {
    int    resolution  = 512;   // Grid vertices per side; cost grows with the square of this
    double size        = 10.0;  // World extent of the terrain in x and z
    double peak_height = 3.2;   // World height of the tallest possible peak

    bool   plain = true;        // A flat plain under the terrain, so it isn't a floating island

    double sun_elevation = 19;  // Degrees above the horizon
    double sun_azimuth   = 250; // Compass degrees: 0 north, 90 east

    terrain_params terrain;
    dem_options    dem;
};

// The terrain on its own: the height grid, its mesh, and the material that shades it.
struct terrain_model {
    heightfield field;
    interval heights;
    shared_ptr<terrain_material> material;
    hittable_list triangles;  // The mesh, one entry per triangle
    hittable_list skirt;      // Real terrain only: walls down from its border, for rendering
};

// Samples the noise, or the loaded elevation data, into a heightfield and meshes it.
//
// Real terrain is built at one world unit per kilometre, the scale the analysis defaults (a
// 2 m eye height of 0.002) already assume for the fractal terrain.
inline terrain_model build_terrain(const terrain_options& opts) {
    const dem_options& dem = opts.dem;
    heightfield field = dem.loaded()
        ? heightfield_from_dem(*dem.grid, opts.resolution, opts.size, 0.001, dem.exaggeration)
        : generate_heightfield(opts.resolution, opts.size, opts.peak_height, opts.terrain);
    const interval heights = field.height_range();
    auto material = make_shared<terrain_material>(heights);

    if (dem.loaded()) {
        material->snow_altitude = dem.snow_line_m > 0 ? 0.001 * dem.exaggeration * dem.snow_line_m
                                                      : infinity;
        material->snow_altitude_blend = 0.001 * dem.exaggeration * 150;  // Patchy over 150 m
        if (dem.palette == terrain_palette::desert) {
            material->grass = color(0.42, 0.30, 0.19);  // Sandy scrub on the flats
            material->rock  = color(0.38, 0.17, 0.09);  // Red sandstone and shale walls
        }
    }
    hittable_list triangles = field.to_triangles(material);

    // Walls to a base a little below the lowest ground, in the brown of exposed soil. Only for
    // rendering: the analysis grid looks at the surface alone.
    hittable_list skirt;
    if (dem.loaded()) {
        const double base = heights.min - std::fmax(0.03 * opts.size, 0.1 * heights.size());
        skirt = field.skirt(base, make_shared<lambertian>(color(0.20, 0.15, 0.11)));
    }
    return terrain_model{std::move(field), heights, material, std::move(triangles), std::move(skirt)};
}

// Which way the camera looks at the terrain.
enum class terrain_view {
    hero,      // Low and close, peaks against the sky: the README image
    overview,  // High and further back, so analysis overlays can be read across the whole field
};

// Puts the terrain on its plain and sets up the camera, sun and sky around it. `world` holds the
// terrain already, as loose triangles or as a BVH built over them.
inline void frame_terrain(camera& cam, const terrain_options& opts, const interval& heights,
                          hittable_list& world, terrain_view view = terrain_view::hero) {
    if (opts.plain) {
        // Two big triangles just below the terrain's base. The terrain's edges already fade to
        // that base height, so the mesh blends into the plain instead of ending in a cliff.
        const double reach = 60 * opts.size;
        const double y = heights.min - 0.002;
        auto plain_material = make_shared<lambertian>(color(0.16, 0.15, 0.12));
        point3 a(-reach, y, -reach), b(reach, y, -reach), c(reach, y, reach), d(-reach, y, reach);
        world.add(make_shared<triangle>(a, c, b, plain_material));
        world.add(make_shared<triangle>(a, d, c, plain_material));
    }

    cam.aspect_ratio = 16.0 / 9.0;
    cam.max_depth    = 8;  // Shadow rays do the direct lighting, so paths can stay short

    // Camera heights are measured from the lowest ground: zero for the fractal terrain, but real
    // terrain can start kilometres above sea level.
    if (opts.dem.loaded()) {
        // Real terrain is far flatter for its width than the fractal range (the Matterhorn rises
        // 2.8 km in 16 km; the fractal peaks 3.2 in 10), so a camera down among the peaks sees
        // only foreground. Stand back from the south-east corner instead, above the highest
        // ground, and take in the whole block.
        const double mid = heights.min + 0.5 * heights.size();
        const bool hero = view == terrain_view::hero;
        cam.vfov     = hero ? 30 : 34;
        cam.lookfrom = point3(0.80*opts.size, mid + (hero ? 0.55 : 0.95)*opts.size, 1.20*opts.size);
        cam.lookat   = point3(0, mid - 0.15*heights.size(), 0.04*opts.size);
    } else if (view == terrain_view::hero) {
        cam.vfov     = 34;
        cam.lookfrom = point3(0.52*opts.size, heights.min + 0.20*opts.size, 0.72*opts.size);
        cam.lookat   = point3(0, heights.min + 0.50*heights.size(), 0);
    } else {
        cam.vfov     = 38;
        cam.lookfrom = point3(0.55*opts.size, heights.min + 0.80*opts.size, 1.05*opts.size);
        cam.lookat   = point3(0, heights.min + 0.15*heights.size(), 0.05*opts.size);
    }
    cam.vup = vec3(0,1,0);

    cam.defocus_angle = 0;

    cam.sun.enabled   = true;
    cam.sun.direction = sun_light::direction_from_elevation_azimuth(opts.sun_elevation,
                                                                    opts.sun_azimuth);
    cam.sun.intensity = color(1.0, 0.87, 0.70) * 7.5;  // Warm, because the sun is low

    cam.sky.horizon        = color(0.62, 0.68, 0.76);
    cam.sky.zenith         = color(0.16, 0.33, 0.72);
    cam.sky.glow_strength  = 0.9;
    cam.sky.glow_sharpness = 14;
}

// A fractal mountain range under a low sun: fBm and ridged noise sampled into a heightfield,
// meshed into the BVH, shaded by slope and height, and lit by one directional sun with
// shadow rays.
inline hittable_list terrain_scene(camera& cam, const terrain_options& opts) {
    terrain_model model = build_terrain(opts);
    hittable_list world = std::move(model.triangles);
    for (const auto& wall : model.skirt.objects)
        world.add(wall);
    frame_terrain(cam, opts, model.heights, world);
    return world;
}

#endif
