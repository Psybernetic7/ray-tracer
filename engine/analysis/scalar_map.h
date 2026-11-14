#ifndef SCALAR_MAP_H
#define SCALAR_MAP_H

#include "rtheader.h"

#include <vector>


// A grid of numbers laid over the terrain's footprint: sun-hours, irradiance, visibility.
//
// Cells are centred: cell (x,y) covers u in [x/width, (x+1)/width], and the same for v, which
// matches the terrain mesh's surface coordinates. That is what lets the renderer look a value up
// from a hit record's u,v. A cell with no data (its ground point couldn't be found) holds NaN,
// and every summary below skips it.
class scalar_map {
  public:
    scalar_map(int width, int height, double fill = 0.0)
      : map_width(width), map_height(height), cells(size_t(width) * size_t(height), fill) {}

    int width() const  { return map_width; }
    int height() const { return map_height; }

    double& at(int x, int y)       { return cells[index(x, y)]; }
    double  at(int x, int y) const { return cells[index(x, y)]; }

    const std::vector<double>& values() const { return cells; }

    // Bilinear interpolation between cell centres, clamped at the border. NaN cells spread to
    // their neighbours, so missing data shows as missing instead of being smoothed over.
    double sample(double u, double v) const {
        const double fx = u * map_width - 0.5;
        const double fy = v * map_height - 0.5;
        const int x0 = clamp_x(int(std::floor(fx))), x1 = clamp_x(int(std::floor(fx)) + 1);
        const int y0 = clamp_y(int(std::floor(fy))), y1 = clamp_y(int(std::floor(fy)) + 1);
        const double tx = std::fmin(std::fmax(fx - std::floor(fx), 0.0), 1.0);
        const double ty = std::fmin(std::fmax(fy - std::floor(fy), 0.0), 1.0);

        const double top    = lerp(at(x0, y0), at(x1, y0), tx);
        const double bottom = lerp(at(x0, y1), at(x1, y1), tx);
        return lerp(top, bottom, ty);
    }

    // Smallest and largest values, ignoring NaN. Empty if there is no data at all.
    interval range() const {
        interval r;
        for (double value : cells) {
            if (std::isnan(value)) continue;
            if (value < r.min) r.min = value;
            if (value > r.max) r.max = value;
        }
        return r;
    }

    double mean() const {
        double sum = 0;
        size_t count = 0;
        for (double value : cells) {
            if (std::isnan(value)) continue;
            sum += value;
            count++;
        }
        return count > 0 ? sum / double(count) : 0.0;
    }

  private:
    int map_width, map_height;
    std::vector<double> cells;

    size_t index(int x, int y) const { return size_t(y) * size_t(map_width) + size_t(x); }

    int clamp_x(int x) const { return x < 0 ? 0 : (x >= map_width ? map_width - 1 : x); }
    int clamp_y(int y) const { return y < 0 ? 0 : (y >= map_height ? map_height - 1 : y); }
};

#endif
