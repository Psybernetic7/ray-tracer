#ifndef HORIZON_MAP_H
#define HORIZON_MAP_H

#include "rtheader.h"
#include "analysis/analysis_grid.h"
#include "analysis/scalar_map.h"
#include "analysis/sun_hours.h"
#include "geometry/hittable.h"
#include "lighting/sun.h"
#include "parallel.h"

#include <atomic>
#include <cstdint>
#include <vector>


// For every cell, how high the terrain rises against the sky in each compass direction.
//
// This is the optimization for sun-hours. Brute force asks "is the sun visible?" with a ray for
// every sun position, and a year has thousands. But the answer depends only on the sun's
// elevation and azimuth, and on how high the skyline is in that direction. So measure the
// skyline once, in a fixed set of directions, and every sun position afterwards is a table
// lookup: is the sun higher than the skyline it stands over? The ray cost no longer depends on
// how finely the year is sampled at all.
class horizon_map {
  public:
    static constexpr int default_directions = 64;

    horizon_map(const analysis_grid& grid, const hittable& world,
                int directions = default_directions, int thread_count = 0)
      : grid_width(grid.width()), grid_height(grid.height()),
        direction_count(directions < 4 ? 4 : directions),
        horizons(size_t(grid_width) * size_t(grid_height) * size_t(direction_count), 90.0f)
    {
        std::atomic<uint64_t> rays{0};

        parallel_for(grid_height, thread_count, [&](int y) {
            uint64_t row_rays = 0;
            for (int x = 0; x < grid_width; x++) {
                if (!grid.valid(x, y)) continue;  // Left at 90 degrees: never sees the sun

                // Neighbouring directions have nearly the same skyline, so each search starts
                // from the one before it.
                const point3 origin = grid.origin(x, y);
                double previous = 0;
                for (int d = 0; d < direction_count; d++) {
                    previous = skyline(world, origin, azimuth_of(d), previous, row_rays);
                    horizons[index(x, y, d)] = float(previous);
                }
            }
            rays += row_rays;
        });

        rays_cast = rays;
    }

    int directions() const { return direction_count; }

    // Rays it took to build the map.
    uint64_t rays() const { return rays_cast; }

    // Skyline elevation in degrees toward direction d, which points at compass azimuth
    // d * 360 / directions(). Never below 0: a sun under the true horizon is night anyway.
    double elevation(int x, int y, int d) const { return horizons[index(x, y, d)]; }

    // Skyline elevation toward any azimuth, interpolated between the two nearest directions.
    double elevation_toward(int x, int y, double azimuth) const {
        int d0; double t;
        locate(azimuth, d0, t);
        return lerp(elevation(x, y, d0), elevation(x, y, (d0 + 1) % direction_count), t);
    }

    double azimuth_of(int d) const { return 360.0 * d / direction_count; }

    // Which pair of directions an azimuth falls between, and how far along.
    void locate(double azimuth, int& d0, double& t) const {
        double f = azimuth / 360.0 * direction_count;
        f -= direction_count * std::floor(f / direction_count);  // Wrap into [0, directions)
        d0 = int(f) % direction_count;
        t = f - std::floor(f);
    }

  private:
    int grid_width, grid_height, direction_count;
    std::vector<float> horizons;  // Floats halve the memory; a degree needs no more precision
    uint64_t rays_cast = 0;

    // How closely each skyline is pinned down: 90 degrees / 2^10, under a tenth of a degree,
    // while the sun moves a quarter of a degree every minute.
    static constexpr double precision = 90.0 / 1024;

    size_t index(int x, int y, int d) const {
        return (size_t(y) * size_t(grid_width) + size_t(x)) * size_t(direction_count) + size_t(d);
    }

    // Finds the skyline toward one azimuth with shadow rays, starting from a guess.
    //
    // This works because the terrain is a heightfield. Along one azimuth it is a single profile
    // of height against distance, so if a ray at some elevation is blocked, every lower ray in
    // that direction is blocked too. The search first brackets the skyline by stepping out from
    // the guess in doubling steps, then bisects the bracket. With a good guess (the previous
    // direction's skyline) the bracket is a degree or two wide and takes a handful of rays,
    // where bisecting all of 0..90 would take eleven. Most directions from a ridge or a peak
    // have no skyline above the horizontal at all, and cost exactly one ray.
    static double skyline(const hittable& world, const point3& origin, double azimuth,
                          double guess, uint64_t& rays) {
        auto blocked = [&](double elevation) {
            rays++;
            const vec3 dir = sun_light::direction_from_elevation_azimuth(elevation, azimuth);
            return world.occluded(ray(origin, dir), interval(0, infinity));
        };

        // Bracket the skyline between lo (blocked) and hi (clear). Straight up is never blocked
        // for a heightfield, so 90 is always a valid hi.
        double lo, hi;
        double step = 1.0;
        if (blocked(guess)) {
            lo = guess;
            hi = 90;
            while (lo + step < 90) {
                if (!blocked(lo + step)) { hi = lo + step; break; }
                lo += step;
                step *= 2;
            }
        } else {
            if (guess <= 0)
                return 0;  // Clear at the horizontal: no skyline to find
            hi = guess;
            lo = 0;
            while (hi - step > 0) {
                if (blocked(hi - step)) { lo = hi - step; break; }
                hi -= step;
                step *= 2;
            }
            if (lo == 0 && !blocked(0))
                return 0;
        }

        while (hi - lo > precision) {
            const double mid = 0.5 * (lo + hi);
            if (blocked(mid)) lo = mid;
            else              hi = mid;
        }
        return 0.5 * (lo + hi);  // The true skyline is somewhere in (lo, hi]
    }
};

// Sun-hours from a horizon map: no rays at all, just a skyline lookup per sun position.
inline scalar_map sun_hours_from_horizons(const analysis_grid& grid, const horizon_map& horizons,
                                          const std::vector<sun_sample>& samples,
                                          sun_quantity quantity, int thread_count = 0) {
    // Every cell looks up the same azimuths, so find each sample's pair of directions once.
    std::vector<int> bins(samples.size());
    std::vector<double> fractions(samples.size());
    for (size_t s = 0; s < samples.size(); s++)
        horizons.locate(samples[s].azimuth, bins[s], fractions[s]);

    scalar_map result(grid.width(), grid.height(), std::nan(""));
    const int directions = horizons.directions();

    parallel_for(grid.height(), thread_count, [&](int y) {
        for (int x = 0; x < grid.width(); x++) {
            if (!grid.valid(x, y)) continue;

            const vec3& normal = grid.normal(x, y);
            double total = 0;

            for (size_t s = 0; s < samples.size(); s++) {
                const double skyline = lerp(horizons.elevation(x, y, bins[s]),
                                            horizons.elevation(x, y, (bins[s] + 1) % directions),
                                            fractions[s]);
                if (samples[s].elevation > skyline)
                    total += sun_contribution(samples[s], normal, quantity);
            }
            result.at(x, y) = total;
        }
    });

    return result;
}

#endif
