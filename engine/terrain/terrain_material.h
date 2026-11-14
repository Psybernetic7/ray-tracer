#ifndef TERRAIN_MATERIAL_H
#define TERRAIN_MATERIAL_H

#include "render/material.h"

#include <functional>


// Terrain shading with no textures: the albedo is a function of where the surface is and which
// way it faces. Grass in the valleys, bare rock wherever the ground is too steep to hold soil,
// snow on high ground that is flat enough to settle on. The same rules a terrain artist would
// paint by hand, and they come out of the heightfield for free.
//
// Scattering is Lambertian, so the renderer can light this directly from the sun.
class terrain_material : public material {
  public:
    color grass = color(0.13, 0.19, 0.09);
    color rock  = color(0.22, 0.21, 0.19);
    color snow  = color(0.85, 0.88, 0.92);

    // Heights are normalized to 0 at the lowest point of the terrain and 1 at the highest.
    double snow_height = 0.62;  // Snow line
    double snow_blend  = 0.10;  // How gradually the snow line fades in

    // Slope is the sine of the surface's tilt: 0 flat, 0.71 at 45 degrees, 1 vertical.
    double rock_slope      = 0.36;  // Where rock starts to show through the grass
    double rock_blend      = 0.28;
    double snow_max_slope  = 0.66;  // Snow slides off anything steeper than this
    double snow_slope_blend = 0.2;

    // Optional: repaints the surface from its u,v coordinates (0..1 across the terrain), given
    // the albedo the slope and height rules picked. Analysis overlays hang off this, which keeps
    // the material ignorant of what is being overlaid: a heatmap, a viewshed, anything else.
    // Called from every render thread at once, so it must not modify shared state.
    std::function<color(double u, double v, const color& base)> overlay;

    explicit terrain_material(const interval& height_range) : heights(height_range) {}

    bool scatter(const ray& r_in, const hit_record& rec, color& attenuation, ray& scattered)
    const override {
        auto scatter_direction = rec.normal + random_unit_vector();
        if (scatter_direction.near_zero())
            scatter_direction = rec.normal;

        scattered = ray(rec.p, scatter_direction);
        attenuation = albedo(rec);
        return true;
    }

    bool diffuse_albedo(const hit_record& rec, color& out) const override {
        out = albedo(rec);
        return true;
    }

  private:
    interval heights;

    color albedo(const hit_record& rec) const {
        // Shade from the *outward* normal: a ray that hits the terrain from below would
        // otherwise see the slope inverted.
        const vec3 n = rec.front_face ? rec.normal : -rec.normal;

        const double slope = std::sqrt(std::fmax(0.0, 1.0 - n.y()*n.y()));
        const double height = heights.size() > 0 ? (rec.p.y() - heights.min) / heights.size() : 0.0;

        const double rockiness = smoothstep(rock_slope, rock_slope + rock_blend, slope);
        color ground = lerp_color(grass, rock, rockiness);

        const double snowiness = smoothstep(snow_height, snow_height + snow_blend, height)
                               * (1.0 - smoothstep(snow_max_slope - snow_slope_blend,
                                                   snow_max_slope, slope));

        const color base = lerp_color(ground, snow, snowiness);
        return overlay ? overlay(rec.u, rec.v, base) : base;
    }

    static color lerp_color(const color& a, const color& b, double t) {
        return (1 - t)*a + t*b;
    }
};

#endif
