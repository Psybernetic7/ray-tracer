#ifndef SUN_HOURS_H
#define SUN_HOURS_H

#include "rtheader.h"
#include "analysis/analysis_grid.h"
#include "analysis/scalar_map.h"
#include "geometry/hittable.h"
#include "lighting/solar.h"
#include "parallel.h"

#include <atomic>
#include <cstdint>
#include <vector>


// Which sun positions a year is sampled at.
struct sun_schedule {
    double latitude = 46.5;  // Degrees north. 46.5 runs through the Swiss Alps
    int    day_step = 7;     // Sample every Nth day of the year
    double minutes  = 15;    // Time step through each sampled day
};

// One sampled sun position, and the slice of the year it stands for.
struct sun_sample {
    vec3   direction;  // Unit vector toward the sun
    double elevation;  // Degrees above the horizon (always positive: night is left out)
    double azimuth;    // Compass degrees
    double hours;      // Hours of the year this sample represents
    double dni;        // Clear-sky direct normal irradiance at this elevation, kW/m^2
};

// Every daytime sun position in the schedule. Each sample sits in the middle of its time step,
// and its weight scales the sampled days up to a full year, so results read as annual totals
// whatever the step sizes.
inline std::vector<sun_sample> sun_samples(const sun_schedule& schedule) {
    const int day_step = schedule.day_step < 1 ? 1 : schedule.day_step;
    const double step_hours = (schedule.minutes > 0 ? schedule.minutes : 15) / 60;
    const int steps_per_day = int(std::ceil(24 / step_hours));

    int sampled_days = 0;
    for (int day = 1; day <= 365; day += day_step)
        sampled_days++;
    const double year_weight = 365.0 / sampled_days;

    std::vector<sun_sample> samples;
    for (int day = 1; day <= 365; day += day_step) {
        for (int step = 0; step < steps_per_day; step++) {
            const double hour = (step + 0.5) * step_hours;
            if (hour >= 24) break;

            const solar_position pos = sun_position(schedule.latitude, day, hour);
            if (pos.elevation <= 0)
                continue;

            const double slice = std::fmin(step_hours, 24 - step * step_hours);
            samples.push_back({pos.direction, pos.elevation, pos.azimuth,
                               slice * year_weight, clear_sky_dni(pos.elevation)});
        }
    }
    return samples;
}

// What to add up for each sun position the ground can see.
enum class sun_quantity {
    hours,       // Hours of direct sun per year
    irradiance,  // Direct-beam energy per year on the sloped ground, kWh/m^2. This is the number
                 // solar-panel siting uses: it weights each hour by how squarely the sun hits
                 // the slope and by how much atmosphere the light came through.
};

inline double sun_contribution(const sun_sample& sun, const vec3& normal, sun_quantity quantity) {
    if (quantity == sun_quantity::hours)
        return sun.hours;
    return sun.hours * sun.dni * std::fmax(0.0, dot(normal, sun.direction));
}

// Sun-hours by brute force: one shadow ray per cell per sun position. Simple and exact for the
// samples taken, and the reference the horizon-map version is checked against. Its cost is
// cells x sun positions rays, so it grows with every extra day or minute sampled.
inline scalar_map sun_hours_brute_force(const analysis_grid& grid, const hittable& world,
                                        const std::vector<sun_sample>& samples,
                                        sun_quantity quantity, int thread_count = 0,
                                        uint64_t* rays_cast = nullptr) {
    scalar_map result(grid.width(), grid.height(), std::nan(""));
    std::atomic<uint64_t> rays{0};

    parallel_for(grid.height(), thread_count, [&](int y) {
        uint64_t row_rays = 0;
        for (int x = 0; x < grid.width(); x++) {
            if (!grid.valid(x, y)) continue;

            const point3 origin = grid.origin(x, y);
            const vec3& normal = grid.normal(x, y);
            double total = 0;

            for (const sun_sample& sun : samples) {
                const double contribution = sun_contribution(sun, normal, quantity);
                if (contribution <= 0) continue;  // Irradiance on a slope facing away: no ray needed

                row_rays++;
                if (!world.occluded(ray(origin, sun.direction), interval(0, infinity)))
                    total += contribution;
            }
            result.at(x, y) = total;
        }
        rays += row_rays;
    });

    if (rays_cast) *rays_cast = rays;
    return result;
}

#endif
