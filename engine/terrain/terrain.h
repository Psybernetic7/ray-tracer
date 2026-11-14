#ifndef TERRAIN_H
#define TERRAIN_H

#include "geometry/heightfield.h"
#include "terrain/noise.h"


// Turns a seed into a mountain range: how the noise layers are combined into a height.
struct terrain_params {
    uint64_t seed       = 1;
    int      octaves    = 9;     // More octaves add finer detail, at a linear cost
    double   frequency  = 1.6;   // Mountains across the field at the coarsest octave
    double   lacunarity = 2.0;
    double   gain       = 0.5;

    // 0 is pure fBm (rounded hills), 1 is pure ridged multifractal (sharp crests). In between
    // is a range with both broad massifs and defined ridgelines.
    double ridge_mix = 0.78;

    double warp = 0.35;  // Domain warp strength; bends ridges so they look eroded

    // Raising the normalized height to a power above 1 pushes the low ground down and leaves
    // the peaks where they are, which carves out valleys and floodplains.
    double exponent = 1.4;

    // Fades the height to zero across this fraction of the field near its border, so the mesh
    // meets the surrounding plain instead of ending in a cliff.
    double edge_falloff = 0.45;
};

// Terrain height at normalized coordinates u,v in [0,1], returned in [0,1].
inline double terrain_height(double u, double v, const terrain_params& p) {
    fbm_params np;
    np.seed       = p.seed;
    np.octaves    = p.octaves;
    np.frequency  = p.frequency;
    np.lacunarity = p.lacunarity;
    np.gain       = p.gain;

    double x = u, y = v;
    warp_point(x, y, p.warp, np);

    const double hills = 0.5 * (fbm(x, y, np) + 1.0);  // fBm is signed; shift it into [0,1]
    const double peaks = ridged(x, y, np);

    double h = lerp(hills, peaks, std::fmin(std::fmax(p.ridge_mix, 0.0), 1.0));

    if (p.exponent != 1.0)
        h = std::pow(std::fmax(h, 0.0), p.exponent);

    if (p.edge_falloff > 0) {
        // Distance to the border as a square ring: 0 at the centre, 1 at the edge.
        const double d = std::fmax(std::fabs(2*u - 1), std::fabs(2*v - 1));
        h *= 1.0 - smoothstep(1.0 - p.edge_falloff, 1.0, d);
    }

    return std::fmin(std::fmax(h, 0.0), 1.0);
}

// Samples the generator into a grid. `size` is the field's world extent in x and z, and
// `peak_height` the world height of the tallest possible peak.
inline heightfield generate_heightfield(int resolution, double size, double peak_height,
                                        const terrain_params& p) {
    heightfield field(resolution, resolution, size, size);

    for (int j = 0; j < field.rows(); j++)
        for (int i = 0; i < field.cols(); i++)
            field.at(i, j) = peak_height * terrain_height(field.u_at(i), field.v_at(j), p);

    return field;
}

#endif
