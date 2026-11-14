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
    "  --no-plain     leave out the flat plain under the terrain\n";

#endif
