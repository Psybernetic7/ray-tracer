#ifndef DEM_H
#define DEM_H

#include "geometry/heightfield.h"
#include "parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>


// Real elevation data: Terrarium tiles decoded from bytes, Web Mercator tile maths, and
// resampling a square around a latitude/longitude into a heightfield.
//
// No I/O happens here. The CLI reads PNGs from disk (or downloads them) and the browser will
// fetch them; both hand this code decoded RGB pixels, so the engine stays the same everywhere.
//
// Source: the AWS Open Data "Terrain Tiles" (Mapzen/Tilezen). A Terrarium tile is a 256x256
// PNG whose colour encodes height: h = R*256 + G + B/256 - 32768 metres, so plain image
// decoders read it and no GIS library is needed.

constexpr int    dem_tile_size   = 256;
constexpr int    dem_max_zoom    = 15;           // The deepest zoom the Terrarium set has
constexpr double earth_radius_m  = 6378137.0;    // WGS84 equatorial radius, as Web Mercator uses

inline double terrarium_decode(uint8_t r, uint8_t g, uint8_t b) {
    return r * 256.0 + g + b / 256.0 - 32768.0;
}

// The inverse, for writing DEM crops back out in the same format. Clamped to what the encoding
// can hold (-32768 to about +32767 m) and rounded to its 1/256 m resolution.
inline void terrarium_encode(double metres, uint8_t& r, uint8_t& g, uint8_t& b) {
    double v = std::round((metres + 32768.0) * 256.0);
    v = std::fmin(std::fmax(v, 0.0), 16777215.0);
    const uint32_t n = uint32_t(v);
    r = uint8_t(n >> 16);
    g = uint8_t((n >> 8) & 0xff);
    b = uint8_t(n & 0xff);
}


// --- Web Mercator ---------------------------------------------------------------------------
// "Global pixel" coordinates at zoom z: the whole world is a square 256 * 2^z pixels wide,
// x growing east from longitude -180 and y growing south from latitude +85.05.

inline double world_pixels(int zoom) { return dem_tile_size * std::ldexp(1.0, zoom); }

inline double mercator_x(double lon_degrees, int zoom) {
    return (lon_degrees + 180.0) / 360.0 * world_pixels(zoom);
}

inline double mercator_y(double lat_degrees, int zoom) {
    const double lat = lat_degrees * pi / 180.0;
    return (1.0 - std::log(std::tan(lat) + 1.0 / std::cos(lat)) / pi) / 2.0 * world_pixels(zoom);
}

inline double mercator_lon(double x, int zoom) { return x / world_pixels(zoom) * 360.0 - 180.0; }

inline double mercator_lat(double y, int zoom) {
    const double n = pi * (1.0 - 2.0 * y / world_pixels(zoom));
    return std::atan(std::sinh(n)) * 180.0 / pi;
}

// Ground distance covered by one pixel at this latitude and zoom.
inline double metres_per_pixel(double lat_degrees, int zoom) {
    return 2 * pi * earth_radius_m * std::cos(lat_degrees * pi / 180.0) / world_pixels(zoom);
}

// The coarsest zoom whose pixels are no larger than `spacing_m`, so the DEM is at least as
// detailed as the grid it is resampled onto, without downloading more than that needs.
inline int choose_zoom(double lat_degrees, double spacing_m, int max_zoom = dem_max_zoom) {
    for (int z = 0; z < max_zoom; z++)
        if (metres_per_pixel(lat_degrees, z) <= spacing_m) return z;
    return max_zoom;
}

struct tile_id {
    int z = 0, x = 0, y = 0;
    bool operator<(const tile_id& o) const {
        return z != o.z ? z < o.z : (x != o.x ? x < o.x : y < o.y);
    }
    bool operator==(const tile_id& o) const { return z == o.z && x == o.x && y == o.y; }
};

inline tile_id tile_at(double lat_degrees, double lon_degrees, int zoom) {
    const int n = 1 << zoom;
    auto clamp = [n](int k) { return k < 0 ? 0 : (k >= n ? n - 1 : k); };
    return tile_id{zoom, clamp(int(std::floor(mercator_x(lon_degrees, zoom) / dem_tile_size))),
                         clamp(int(std::floor(mercator_y(lat_degrees, zoom) / dem_tile_size)))};
}


// --- A region on the ground -----------------------------------------------------------------

// A square of ground centred on a latitude/longitude, `extent_m` metres on a side, aligned with
// north. Resampled onto a regular grid in metres, so it maps straight onto a heightfield.
struct geo_region {
    double lat = 0, lon = 0;
    double extent_m = 10000;

    // The latitude/longitude of a point `east_m` and `north_m` from the centre. A local tangent
    // plane: exact enough over tens of kilometres (the error grows with the square of distance,
    // and is about 0.01% at 20 km), and it keeps every grid cell the same size in metres.
    void offset(double east_m, double north_m, double& lat_out, double& lon_out) const {
        lat_out = lat + north_m / earth_radius_m * 180.0 / pi;
        lon_out = lon + east_m / (earth_radius_m * std::cos(lat_out * pi / 180.0)) * 180.0 / pi;
    }

    // Grid vertex (i,j) of a res x res grid over the region: i west to east, j north to south,
    // the same orientation as the heightfield (north at -z).
    void vertex(int i, int j, int resolution, double& lat_out, double& lon_out) const {
        const double step = extent_m / (resolution - 1);
        offset(-0.5 * extent_m + i * step, 0.5 * extent_m - j * step, lat_out, lon_out);
    }
};

// Every tile at `zoom` that the region's grid samples from, one tile of margin included where
// bilinear filtering reaches over a tile edge.
inline std::vector<tile_id> tiles_covering(const geo_region& region, int zoom) {
    double lat_n, lon_w, lat_s, lon_e;
    region.offset(-0.5 * region.extent_m,  0.5 * region.extent_m, lat_n, lon_w);
    region.offset( 0.5 * region.extent_m, -0.5 * region.extent_m, lat_s, lon_e);
    // The tangent plane's west edge bends slightly with latitude; take the widest of both rows.
    double lat_tmp, lon_w2, lon_e2;
    region.offset(-0.5 * region.extent_m, -0.5 * region.extent_m, lat_tmp, lon_w2);
    region.offset( 0.5 * region.extent_m,  0.5 * region.extent_m, lat_tmp, lon_e2);
    lon_w = std::fmin(lon_w, lon_w2);
    lon_e = std::fmax(lon_e, lon_e2);

    const int n = 1 << zoom;
    auto tile_of = [](double pixel) { return int(std::floor(pixel / dem_tile_size)); };
    // Bilinear filtering reads the pixel centres either side, half a pixel out.
    const int x0 = tile_of(mercator_x(lon_w, zoom) - 0.5), x1 = tile_of(mercator_x(lon_e, zoom) + 0.5);
    const int y0 = tile_of(mercator_y(lat_n, zoom) - 0.5), y1 = tile_of(mercator_y(lat_s, zoom) + 0.5);

    std::vector<tile_id> tiles;
    for (int y = std::max(y0, 0); y <= std::min(y1, n - 1); y++)
        for (int x = x0; x <= x1; x++)
            tiles.push_back(tile_id{zoom, ((x % n) + n) % n, y});  // Wraps across the antimeridian
    return tiles;
}


// --- Elevation grids ------------------------------------------------------------------------

// A rectangular grid of elevations in metres, row by row from the north. Used both for decoded
// tiles and for a region resampled to a heightfield's resolution (one value per vertex).
struct dem_grid {
    int width = 0, height = 0;
    std::vector<float> metres;

    dem_grid() = default;
    dem_grid(int w, int h) : width(w), height(h), metres(size_t(w) * size_t(h), 0.0f) {}

    float& at(int x, int y)       { return metres[size_t(y) * size_t(width) + size_t(x)]; }
    float  at(int x, int y) const { return metres[size_t(y) * size_t(width) + size_t(x)]; }

    // Bilinear sample where u,v run 0..1 from the first vertex to the last.
    double sample_uv(double u, double v) const {
        const double x = std::fmin(std::fmax(u, 0.0), 1.0) * (width - 1);
        const double y = std::fmin(std::fmax(v, 0.0), 1.0) * (height - 1);
        const int x0 = std::min(int(x), width - 2 < 0 ? 0 : width - 2);
        const int y0 = std::min(int(y), height - 2 < 0 ? 0 : height - 2);
        const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        const double fx = x - x0, fy = y - y0;
        const double top    = at(x0, y0) * (1 - fx) + at(x1, y0) * fx;
        const double bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
        return top * (1 - fy) + bottom * fy;
    }

    // Lowest and highest elevation, in metres.
    std::pair<double, double> range() const {
        double lo = infinity, hi = -infinity;
        for (float m : metres) { lo = std::fmin(lo, m); hi = std::fmax(hi, m); }
        return {lo, hi};
    }
};

// Decodes Terrarium pixels (tightly packed 8-bit RGB, rows from the top) into metres.
inline dem_grid decode_terrarium(const uint8_t* rgb, int width, int height) {
    dem_grid grid(width, height);
    for (size_t k = 0; k < grid.metres.size(); k++)
        grid.metres[k] = float(terrarium_decode(rgb[3*k], rgb[3*k + 1], rgb[3*k + 2]));
    return grid;
}

// The reverse: a grid as Terrarium RGB bytes, for saving a region as a single PNG.
inline std::vector<uint8_t> encode_terrarium(const dem_grid& grid) {
    std::vector<uint8_t> rgb(grid.metres.size() * 3);
    for (size_t k = 0; k < grid.metres.size(); k++)
        terrarium_encode(grid.metres[k], rgb[3*k], rgb[3*k + 1], rgb[3*k + 2]);
    return rgb;
}


// Decoded tiles at one zoom level, stitched into one seamless surface addressed in global
// pixel coordinates. Tiles that were never added read as missing, which sampling reports.
class tile_mosaic {
  public:
    explicit tile_mosaic(int zoom) : z(zoom) {}

    int zoom() const { return z; }

    void add(int x, int y, dem_grid tile) { tiles[{x, y}] = std::move(tile); }
    bool has(int x, int y) const { return tiles.count({x, y}) != 0; }
    size_t size() const { return tiles.size(); }

    // Elevation at global pixel (px, py) at this mosaic's zoom, bilinear between the four
    // nearest pixel centres, which may sit in up to four different tiles. Each pixel's value
    // belongs to its centre, half a pixel in from its corner.
    bool sample(double px, double py, double& out) const {
        const double x = px - 0.5, y = py - 0.5;
        const long long x0 = (long long)std::floor(x), y0 = (long long)std::floor(y);
        const double fx = x - double(x0), fy = y - double(y0);

        double v00, v10, v01, v11;
        if (!pixel(x0, y0, v00) || !pixel(x0 + 1, y0, v10) ||
            !pixel(x0, y0 + 1, v01) || !pixel(x0 + 1, y0 + 1, v11))
            return false;

        out = (v00 * (1 - fx) + v10 * fx) * (1 - fy) + (v01 * (1 - fx) + v11 * fx) * fy;
        return true;
    }

  private:
    int z;
    std::map<std::pair<int, int>, dem_grid> tiles;

    bool pixel(long long gx, long long gy, double& out) const {
        const long long world = (long long)dem_tile_size << z;
        gx = ((gx % world) + world) % world;                       // Wrap east-west
        gy = gy < 0 ? 0 : (gy >= world ? world - 1 : gy);          // Clamp at the poles
        const int tx = int(gx / dem_tile_size), ty = int(gy / dem_tile_size);
        const auto it = tiles.find({tx, ty});
        if (it == tiles.end() || it->second.width != dem_tile_size || it->second.height != dem_tile_size)
            return false;
        out = it->second.at(int(gx % dem_tile_size), int(gy % dem_tile_size));
        return true;
    }
};

// Resamples the region onto a resolution x resolution vertex grid. Returns false if any vertex
// falls on a tile the mosaic doesn't have.
inline bool sample_region(const tile_mosaic& mosaic, const geo_region& region, int resolution,
                          dem_grid& out, int thread_count = 0) {
    resolution = resolution < 2 ? 2 : resolution;
    out = dem_grid(resolution, resolution);
    std::vector<char> row_ok(size_t(resolution), 1);

    parallel_for(resolution, thread_count, [&](int j) {
        for (int i = 0; i < resolution; i++) {
            double lat, lon, h;
            region.vertex(i, j, resolution, lat, lon);
            if (!mosaic.sample(mercator_x(lon, mosaic.zoom()), mercator_y(lat, mosaic.zoom()), h)) {
                row_ok[size_t(j)] = 0;
                return;
            }
            out.at(i, j) = float(h);
        }
    });

    for (char ok : row_ok)
        if (!ok) return false;
    return true;
}

// Turns elevations into a heightfield `size` world units across. `units_per_metre` scales
// both axes (1/1000 makes one world unit a kilometre, the scale the analysis defaults assume);
// `exaggeration` stretches only the vertical. The grid is resampled if the resolution differs.
inline heightfield heightfield_from_dem(const dem_grid& dem, int resolution, double size,
                                        double units_per_metre, double exaggeration = 1.0) {
    heightfield field(resolution, resolution, size, size);
    const bool same = dem.width == field.cols() && dem.height == field.rows();
    const double scale = units_per_metre * exaggeration;
    for (int j = 0; j < field.rows(); j++)
        for (int i = 0; i < field.cols(); i++)
            field.at(i, j) = scale * (same ? dem.at(i, j) : dem.sample_uv(field.u_at(i), field.v_at(j)));
    return field;
}

#endif
