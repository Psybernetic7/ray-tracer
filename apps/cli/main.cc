#include "rtheader.h"

#include "geometry/bvh.h"
#include "geometry/hittable_list.h"
#include "render/camera.h"
#include "render/framebuffer.h"
#include "render/renderer.h"

#include "image_io.h"
#include "scenes.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>


struct options {
    std::string scene             = "terrain";
    int         image_width       = 1200;
    int         samples_per_pixel = 500;
    int         threads           = 0;
    uint64_t    seed              = 1;
    bool        use_bvh           = true;
    bool        quiet             = false;
    bool        no_sun            = false;
    std::string output            = "image.png";

    terrain_options terrain;
};

static void print_usage() {
    std::cerr <<
        "Usage: rt-cli [options]\n"
        "  --scene NAME   terrain | showcase (default terrain)\n"
        "  --width N      image width in pixels (default 1200)\n"
        "  --spp N        samples per pixel (default 500)\n"
        "  --threads N    worker threads, 0 = all hardware threads (default 0)\n"
        "  --seed N       random seed for the scene and render (default 1)\n"
        "  --no-bvh       intersect objects linearly instead of through a BVH\n"
        "  --quiet        don't print progress\n"
        "  --out PATH     output image, .png or .ppm (default image.png)\n"
        "\n"
        "Terrain scene options:\n"
        "  --res N        heightfield vertices per side (default 512)\n"
        "  --octaves N    noise octaves (default 9)\n"
        "  --frequency F  mountains across the field at the coarsest octave (default 1.6)\n"
        "  --ridge F      0 = rounded hills, 1 = sharp ridges (default 0.78)\n"
        "  --warp F       domain warp strength, 0 = off (default 0.35)\n"
        "  --height F     world height of the tallest peak (default 3.2)\n"
        "  --sun-elev D   sun elevation above the horizon in degrees (default 19)\n"
        "  --sun-azim D   sun azimuth in degrees, 0 north / 90 east (default 250)\n"
        "  --no-sun       light with the sky alone, no sun or shadow rays\n"
        "  --no-plain     leave out the flat plain under the terrain\n";
}

static bool parse_options(int argc, char* argv[], options& opts) {
    for (int k = 1; k < argc; k++) {
        std::string arg = argv[k];
        auto next_value = [&](long long& value) {
            if (k + 1 >= argc) return false;
            try { value = std::stoll(argv[++k]); } catch (...) { return false; }
            return true;
        };
        auto next_real = [&](double& value) {
            if (k + 1 >= argc) return false;
            try { value = std::stod(argv[++k]); } catch (...) { return false; }
            return true;
        };

        long long value;
        double real;
        if (arg == "--width" && next_value(value) && value > 0)        opts.image_width = int(value);
        else if (arg == "--spp" && next_value(value) && value > 0)     opts.samples_per_pixel = int(value);
        else if (arg == "--threads" && next_value(value) && value >= 0) opts.threads = int(value);
        else if (arg == "--seed" && next_value(value))                 opts.seed = uint64_t(value);
        else if (arg == "--no-bvh")                                    opts.use_bvh = false;
        else if (arg == "--quiet")                                     opts.quiet = true;
        else if (arg == "--out" && k + 1 < argc)                       opts.output = argv[++k];
        else if (arg == "--scene" && k + 1 < argc)                     opts.scene = argv[++k];
        else if (arg == "--res" && next_value(value) && value >= 2)    opts.terrain.resolution = int(value);
        else if (arg == "--octaves" && next_value(value) && value > 0) opts.terrain.terrain.octaves = int(value);
        else if (arg == "--frequency" && next_real(real) && real > 0)  opts.terrain.terrain.frequency = real;
        else if (arg == "--ridge" && next_real(real))                  opts.terrain.terrain.ridge_mix = real;
        else if (arg == "--warp" && next_real(real))                   opts.terrain.terrain.warp = real;
        else if (arg == "--height" && next_real(real) && real > 0)     opts.terrain.peak_height = real;
        else if (arg == "--sun-elev" && next_real(real))               opts.terrain.sun_elevation = real;
        else if (arg == "--sun-azim" && next_real(real))               opts.terrain.sun_azimuth = real;
        else if (arg == "--no-sun")                                    opts.no_sun = true;
        else if (arg == "--no-plain")                                  opts.terrain.plain = false;
        else {
            std::cerr << "Invalid argument: " << arg << "\n";
            return false;
        }
    }

    if (opts.scene != "terrain" && opts.scene != "showcase") {
        std::cerr << "Unknown scene: " << opts.scene << "\n";
        return false;
    }

    opts.terrain.terrain.seed = opts.seed;
    return true;
}

int main(int argc, char* argv[]) {
    options opts;
    if (!parse_options(argc, argv, opts)) {
        print_usage();
        return 1;
    }

    camera cam;
    seed_random(opts.seed);

    auto build_start = std::chrono::steady_clock::now();
    hittable_list scene = opts.scene == "terrain" ? terrain_scene(cam, opts.terrain)
                                                  : showcase_scene(cam);
    if (opts.no_sun)
        cam.sun.enabled = false;

    cam.image_width = opts.image_width;
    cam.initialize();

    shared_ptr<hittable> world = opts.use_bvh ? shared_ptr<hittable>(make_shared<bvh_node>(scene))
                                              : make_shared<hittable_list>(scene);
    std::chrono::duration<double> build_elapsed = std::chrono::steady_clock::now() - build_start;

    if (!opts.quiet) {
        std::fprintf(stderr, "Scene: %s, %zu primitives, built in %.2f s\n",
                     opts.scene.c_str(), scene.objects.size(), build_elapsed.count());
    }

    framebuffer fb(cam.image_width, cam.height());

    renderer r;
    r.thread_count = opts.threads;
    r.seed = opts.seed;
    if (!opts.quiet) {
        r.on_progress = [](int done, int total) {
            std::fprintf(stderr, "\rTiles: %d/%d ", done, total);
        };
    }

    auto start = std::chrono::steady_clock::now();
    r.render_pass(cam, *world, fb, opts.samples_per_pixel);
    std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    std::string error;
    if (!write_image(fb, opts.output, error)) {
        std::cerr << "\n" << error << "\n";
        return 1;
    }

    std::fprintf(stderr, "\rRendered %dx%d at %d spp in %.2f s -> %s\n",
                 fb.width(), fb.height(), fb.samples_per_pixel(), elapsed.count(), opts.output.c_str());
    return 0;
}
