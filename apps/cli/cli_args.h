#ifndef CLI_ARGS_H
#define CLI_ARGS_H

#include "scenes.h"

#include <string>


// Walks the command line one option at a time. Shared by the render and analyze commands so
// they parse values, and the terrain options they have in common, the same way.
class arg_cursor {
  public:
    arg_cursor(int argc, char* argv[], int first) : argc(argc), argv(argv), k(first) {}

    bool done() const { return k >= argc; }
    std::string next() { return argv[k++]; }

    bool value(long long& out) {
        if (k >= argc) return false;
        try { out = std::stoll(argv[k++]); } catch (...) { return false; }
        return true;
    }

    bool value(double& out) {
        if (k >= argc) return false;
        try { out = std::stod(argv[k++]); } catch (...) { return false; }
        return true;
    }

    bool value(std::string& out) {
        if (k >= argc) return false;
        out = argv[k++];
        return true;
    }

    // Reads the next value into `target` if it is valid; leaves `target` alone otherwise.
    bool read_int(int& target, long long min) {
        long long n;
        if (!value(n) || n < min) return false;
        target = int(n);
        return true;
    }

    bool read_real(double& target, bool must_be_positive = false) {
        double x;
        if (!value(x) || (must_be_positive && x <= 0)) return false;
        target = x;
        return true;
    }

  private:
    int argc;
    char** argv;
    int k;
};

enum class parse_result { no_match, ok, bad_value };

// Reads "LAT,LON" in degrees.
inline bool read_center(arg_cursor& args, dem_options& dem) {
    std::string text;
    if (!args.value(text)) return false;
    const auto comma = text.find(',');
    if (comma == std::string::npos) return false;
    try {
        const double lat = std::stod(text.substr(0, comma));
        const double lon = std::stod(text.substr(comma + 1));
        if (lat < -85 || lat > 85 || lon < -180 || lon > 180) return false;
        dem.lat = lat;
        dem.lon = lon;
        dem.has_center = true;
        return true;
    } catch (...) {
        return false;
    }
}

inline bool ends_with_png(const std::string& path) {
    return path.size() >= 4 && path.compare(path.size() - 4, 4, ".png") == 0;
}

// The options that shape the terrain itself, accepted by both commands.
inline parse_result parse_terrain_option(const std::string& arg, arg_cursor& args,
                                         terrain_options& opts) {
    auto result = [](bool ok) { return ok ? parse_result::ok : parse_result::bad_value; };

    if (arg == "--res")       return result(args.read_int(opts.resolution, 2));
    if (arg == "--octaves")   return result(args.read_int(opts.terrain.octaves, 1));
    if (arg == "--frequency") return result(args.read_real(opts.terrain.frequency, true));
    if (arg == "--ridge")     return result(args.read_real(opts.terrain.ridge_mix));
    if (arg == "--warp")      return result(args.read_real(opts.terrain.warp));
    if (arg == "--height")    return result(args.read_real(opts.peak_height, true));
    if (arg == "--sun-elev")  return result(args.read_real(opts.sun_elevation));
    if (arg == "--sun-azim")  return result(args.read_real(opts.sun_azimuth));
    if (arg == "--no-plain")  { opts.plain = false; return parse_result::ok; }

    dem_options& dem = opts.dem;
    if (arg == "--place")      return result(args.value(dem.place));
    if (arg == "--center")     return result(read_center(args, dem));
    if (arg == "--extent")     return result(args.read_real(dem.extent_km, true));
    if (arg == "--zoom")       return result(args.read_int(dem.zoom, 0) && dem.zoom <= dem_max_zoom);
    if (arg == "--exaggerate") return result(args.read_real(dem.exaggeration, true));
    if (arg == "--tile-cache") return result(args.value(dem.cache_dir));
    if (arg == "--save-dem")   return result(args.value(dem.save_path) && ends_with_png(dem.save_path));
    if (arg == "--download")   { dem.download = true; return parse_result::ok; }
    return parse_result::no_match;
}

inline const char* terrain_usage =
    "  --res N        heightfield vertices per side (default 512)\n"
    "  --octaves N    noise octaves (default 9)\n"
    "  --frequency F  mountains across the field at the coarsest octave (default 1.6)\n"
    "  --ridge F      0 = rounded hills, 1 = sharp ridges (default 0.78)\n"
    "  --warp F       domain warp strength, 0 = off (default 0.35)\n"
    "  --height F     world height of the tallest peak (default 3.2)\n"
    "  --sun-elev D   render sun elevation above the horizon in degrees\n"
    "  --sun-azim D   render sun azimuth in degrees, 0 north / 90 east\n"
    "  --no-plain     leave out the flat plain under the terrain\n"
    "\n"
    "Real terrain (AWS Terrain Tiles, in place of the noise):\n"
    "  --place NAME      grand-canyon | matterhorn | fuji (bundled, works offline)\n"
    "  --center LAT,LON  any square of the Earth, downloaded with curl and cached\n"
    "  --extent KM       side of the square around --center (default 20)\n"
    "  --zoom N          tile zoom 0-15 (default: matched to the --res grid spacing)\n"
    "  --exaggerate F    vertical exaggeration (default 1)\n"
    "  --download        fetch tiles for a --place instead of using its bundled copy\n"
    "  --tile-cache DIR  where downloaded tiles are kept (default tile-cache)\n"
    "  --save-dem PATH   also write the region's elevations as a Terrarium .png\n";

#endif
