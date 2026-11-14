#ifndef VIEWSHED_H
#define VIEWSHED_H

#include "rtheader.h"
#include "analysis/analysis_grid.h"
#include "analysis/scalar_map.h"
#include "geometry/hittable.h"
#include "parallel.h"

#include <atomic>
#include <cstdint>


// What can be seen from a point: 1 for every cell in line of sight of the observer, 0 for every
// cell hidden behind terrain, NaN where there is no ground. One ray per cell, from the observer
// to a point target_height above the ground there. A target height above zero asks a different
// question: not "can I see the ground?" but "can I see a person, a mast, a building there?".
inline scalar_map viewshed(const analysis_grid& grid, const hittable& world,
                           const point3& observer, double target_height = 0,
                           int thread_count = 0, uint64_t* rays_cast = nullptr) {
    scalar_map result(grid.width(), grid.height(), std::nan(""));
    std::atomic<uint64_t> rays{0};

    parallel_for(grid.height(), thread_count, [&](int y) {
        uint64_t row_rays = 0;
        for (int x = 0; x < grid.width(); x++) {
            if (!grid.valid(x, y)) continue;

            const point3 target = grid.origin(x, y) + vec3(0, target_height, 0);
            const vec3 sight = target - observer;
            const double distance = sight.length();

            if (distance < 2 * grid.lift()) {  // The observer's own cell
                result.at(x, y) = 1;
                continue;
            }

            // The ray's direction is the whole sightline, so t runs from 0 at the observer to 1
            // at the target. Stop just short of 1: a sightline that arrives at a grazing angle
            // would otherwise clip the ground right beside the target and count it as hidden.
            const double t_end = 1 - 2 * grid.lift() / distance;

            row_rays++;
            const bool hidden = world.occluded(ray(observer, sight), interval(0, t_end));
            result.at(x, y) = hidden ? 0.0 : 1.0;
        }
        rays += row_rays;
    });

    if (rays_cast) *rays_cast = rays;
    return result;
}

#endif
