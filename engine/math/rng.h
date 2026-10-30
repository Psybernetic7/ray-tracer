#ifndef RNG_H
#define RNG_H

#include <cstdint>

// Each thread owns its own generator state, so threads never share or contend on it.
// The renderer reseeds the state at the start of every tile, which makes renders
// reproducible regardless of how tiles are scheduled across threads.
inline thread_local uint64_t rng_state = 0x853c49e6748fea9bULL;

inline void seed_random(uint64_t seed) {
    rng_state = seed;
}

inline uint64_t mix_seed(uint64_t a, uint64_t b) {
    // Combines two values into a well-distributed seed (splitmix64 finalizer).
    uint64_t z = a * 0x9e3779b97f4a7c15ULL + b;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

inline uint64_t random_u64() {
    // splitmix64: tiny state, fast, and good statistical quality for rendering.
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

inline double random_double() {
    // Returns a random real in [0,1), using the top 53 bits for full double precision.
    return (random_u64() >> 11) * 0x1.0p-53;
}

inline double random_double(double min, double max) {
    // Returns a random real in [min,max).
    return min + (max-min)*random_double();
}

#endif
