#ifndef NOISE_H
#define NOISE_H

#include "math/constants.h"
#include "math/rng.h"

#include <cmath>
#include <cstdint>


// Seeded 2D gradient noise and the fractal sums built on top of it.
//
// Everything here is a pure function of its arguments and a seed: no permutation table, no
// global state, no setup step. That keeps the terrain reproducible from a seed alone and lets
// any number of threads sample it at once, which matters because M2 samples the terrain from
// every render thread and the web build generates it on a worker.

// 16 unit gradients, every 22.5 degrees around the circle. A small fixed set is what classic
// Perlin noise uses; it keeps the lattice dot products cheap and the artifacts mild.
inline constexpr double noise_gradients[32] = {
     1.0000000000000000,  0.0000000000000000,
     0.9238795325112867,  0.3826834323650898,
     0.7071067811865476,  0.7071067811865476,
     0.3826834323650898,  0.9238795325112867,
     0.0000000000000000,  1.0000000000000000,
    -0.3826834323650898,  0.9238795325112867,
    -0.7071067811865476,  0.7071067811865476,
    -0.9238795325112867,  0.3826834323650898,
    -1.0000000000000000,  0.0000000000000000,
    -0.9238795325112867, -0.3826834323650898,
    -0.7071067811865476, -0.7071067811865476,
    -0.3826834323650898, -0.9238795325112867,
     0.0000000000000000, -1.0000000000000000,
     0.3826834323650898, -0.9238795325112867,
     0.7071067811865476, -0.7071067811865476,
     0.9238795325112867, -0.3826834323650898,
};

inline uint64_t noise_lattice_hash(int xi, int yi, uint64_t seed) {
    // Two splitmix64 finalizer rounds over the lattice coordinates. Cheap, and well enough
    // distributed that neighbouring cells get unrelated gradients.
    uint64_t h = mix_seed(seed ^ 0x51633e2d9a7fb5c1ULL, uint64_t(int64_t(xi)));
    return mix_seed(h, uint64_t(int64_t(yi)));
}

inline double noise_grad_dot(int xi, int yi, double dx, double dy, uint64_t seed) {
    const int g = int(noise_lattice_hash(xi, yi, seed) & 15) * 2;
    return noise_gradients[g]*dx + noise_gradients[g + 1]*dy;
}

inline double noise_fade(double t) {
    // Quintic interpolant: zero first and second derivatives at the lattice points, so fBm
    // sums don't show the grid as faint creases.
    return t*t*t*(t*(t*6 - 15) + 10);
}

// Gradient ("Perlin") noise. Returns roughly [-1,1]; it is zero at every lattice point.
inline double gradient_noise(double x, double y, uint64_t seed) {
    const double fx = std::floor(x), fy = std::floor(y);
    const int xi = int(fx), yi = int(fy);
    const double tx = x - fx, ty = y - fy;

    const double n00 = noise_grad_dot(xi,     yi,     tx,     ty,     seed);
    const double n10 = noise_grad_dot(xi + 1, yi,     tx - 1, ty,     seed);
    const double n01 = noise_grad_dot(xi,     yi + 1, tx,     ty - 1, seed);
    const double n11 = noise_grad_dot(xi + 1, yi + 1, tx - 1, ty - 1, seed);

    const double u = noise_fade(tx), v = noise_fade(ty);
    const double nx0 = lerp(n00, n10, u);
    const double nx1 = lerp(n01, n11, u);

    // 2D gradient noise peaks near 1/sqrt(2), so scale up to use the full [-1,1] range.
    return 1.4142135623730951 * lerp(nx0, nx1, v);
}

// Settings shared by the fractal sums below.
struct fbm_params {
    uint64_t seed       = 1;
    int      octaves    = 8;    // Number of noise layers summed
    double   frequency  = 1.0;  // Features per unit of input space, at the first octave
    double   lacunarity = 2.0;  // Frequency multiplier per octave
    double   gain       = 0.5;  // Amplitude multiplier per octave
};

// Fractional Brownian motion: octaves of gradient noise at rising frequency and falling
// amplitude. Rolling hills. Normalized to roughly [-1,1].
inline double fbm(double x, double y, const fbm_params& p) {
    double sum = 0, amplitude = 1, frequency = p.frequency, norm = 0;

    for (int octave = 0; octave < p.octaves; octave++) {
        // Each octave gets its own seed, so octaves are independent rather than the same
        // field sampled at a different zoom.
        sum  += amplitude * gradient_noise(x*frequency, y*frequency,
                                           mix_seed(p.seed, uint64_t(octave)));
        norm += amplitude;
        frequency *= p.lacunarity;
        amplitude *= p.gain;
    }

    return norm > 0 ? sum / norm : 0.0;
}

// Ridged multifractal (Musgrave): fold the noise at zero with 1-|n| so the creases become
// crests, square it to sharpen them, and weight each octave by the previous one so detail
// only appears along the ridges. Returns [0,1]. This is what makes mountains look eroded
// instead of lumpy.
inline double ridged(double x, double y, const fbm_params& p) {
    double sum = 0, amplitude = 1, frequency = p.frequency, norm = 0, weight = 1;

    for (int octave = 0; octave < p.octaves; octave++) {
        double signal = 1.0 - std::fabs(gradient_noise(x*frequency, y*frequency,
                                                       mix_seed(p.seed, uint64_t(octave))));
        signal *= signal;
        signal *= weight;

        sum  += amplitude * signal;
        norm += amplitude;
        weight = signal < 0 ? 0 : (signal > 1 ? 1 : signal);

        frequency *= p.lacunarity;
        amplitude *= p.gain;
    }

    return norm > 0 ? sum / norm : 0.0;
}

// Domain warping: displace the sample point by another noise field before sampling. Straight
// ridges start to bend and branch, which reads as erosion rather than noise.
inline void warp_point(double& x, double& y, double strength, const fbm_params& p) {
    if (strength == 0) return;

    fbm_params wx = p, wy = p;
    wx.octaves = wy.octaves = p.octaves < 4 ? p.octaves : 4;  // Warp wants the broad shape only
    wx.seed = mix_seed(p.seed, 0x5741u);
    wy.seed = mix_seed(p.seed, 0x9c3bu);

    const double dx = fbm(x, y, wx);
    const double dy = fbm(x, y, wy);
    x += strength * dx;
    y += strength * dy;
}

#endif
