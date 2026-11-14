#ifndef SUN_H
#define SUN_H

#include "rtheader.h"


// The sun, as a directional light.
//
// Because the sun is 150 million km away its rays arrive parallel, so it needs a direction and
// an intensity but no position. It is not part of the scene geometry: nothing can hit it, and
// the renderer lights surfaces by aiming shadow rays at it (next-event estimation). A sun that
// was modelled as geometry would be hopeless here, since a scattered ray finds a 0.5-degree
// disk about once in a hundred thousand tries.
//
// M2 replaces `direction` with a real solar position from latitude, day and hour; the rest of
// this stays as it is.
struct sun_light {
    bool   enabled   = false;
    vec3   direction = unit_vector(vec3(-0.45, 0.4, -0.6));  // Unit vector pointing *at* the sun
    color  intensity = color(1.0, 0.95, 0.85) * 9.0;         // Irradiance on a surface facing it

    // The sun's apparent radius: 0.265 degrees from Earth. Shadow rays sampled across that disk
    // give penumbrae that widen with distance from the caster, which is most of what makes a
    // sunlit landscape read as real.
    double angular_radius_degrees = 0.265;

    // Should the sky show the sun's disk? It makes the sun visible to the camera and in
    // reflections, at the cost of a trace of double counting (a scattered ray can both receive
    // the sun through a shadow ray and land on the disk). The disk is small enough that the
    // error is far below the sampling noise.
    bool disk_in_sky = true;

    // Direction to a random point on the sun's disk, for one soft shadow ray.
    vec3 sample_direction() const {
        const double cos_max = std::cos(degrees_to_radians(angular_radius_degrees));
        return sample_cone(direction, cos_max);
    }

    // Sun direction from elevation above the horizon and compass azimuth, with y up,
    // north at -z and east at +x. M2's solar position calculation feeds this.
    static vec3 direction_from_elevation_azimuth(double elevation_degrees, double azimuth_degrees) {
        const double el = degrees_to_radians(elevation_degrees);
        const double az = degrees_to_radians(azimuth_degrees);
        const double horizontal = std::cos(el);
        return vec3(horizontal * std::sin(az), std::sin(el), -horizontal * std::cos(az));
    }

  private:
    // A direction drawn uniformly from the cone of half-angle acos(cos_max) around `axis`.
    static vec3 sample_cone(const vec3& axis, double cos_max) {
        const double cos_theta = 1 - random_double() * (1 - cos_max);
        const double sin_theta = std::sqrt(std::fmax(0.0, 1 - cos_theta*cos_theta));
        const double phi = 2*pi*random_double();

        // Any two vectors perpendicular to the axis will do for the disk's basis.
        const vec3 w = unit_vector(axis);
        const vec3 helper = std::fabs(w.x()) > 0.9 ? vec3(0,1,0) : vec3(1,0,0);
        const vec3 u = unit_vector(cross(helper, w));
        const vec3 v = cross(w, u);

        return sin_theta*std::cos(phi)*u + sin_theta*std::sin(phi)*v + cos_theta*w;
    }
};


// The sky, as the only other light in the scene: a vertical gradient that also serves as fill
// light for everything the sun doesn't reach. The defaults reproduce the plain blue-to-white
// gradient the renderer has always used.
struct sky_light {
    color horizon = color(1.0, 1.0, 1.0);
    color zenith  = color(0.5, 0.7, 1.0);

    // A broad warm halo around the sun: the aerosol scattering that makes a low sun look hazy.
    // This is sky radiance, not the sun's, so it is not double counted by the shadow rays.
    double glow_strength  = 0.0;
    color  glow_tint      = color(1.0, 0.72, 0.42);
    double glow_sharpness = 12.0;   // Higher is a tighter halo

    color radiance(const vec3& unit_direction, const sun_light& sun) const {
        const double a = 0.5*(unit_direction.y() + 1.0);
        color sky = (1.0 - a)*horizon + a*zenith;

        if (!sun.enabled)
            return sky;

        const double cos_angle = dot(unit_direction, sun.direction);

        if (glow_strength > 0 && cos_angle > 0)
            sky += glow_strength * std::pow(cos_angle, glow_sharpness) * glow_tint;

        if (sun.disk_in_sky && cos_angle > std::cos(degrees_to_radians(sun.angular_radius_degrees)))
            sky += sun.intensity;

        return sky;
    }
};

#endif
