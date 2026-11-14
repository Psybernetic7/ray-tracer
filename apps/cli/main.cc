#include "rtheader.h"

#include "geometry/bvh.h"
#include "geometry/hittable_list.h"
#include "render/camera.h"
#include "render/framebuffer.h"
#include "render/renderer.h"

#include "analyze.h"
#include "cli_args.h"
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
        "Usage: rt-cli [options]            render a scene\n"
        "       rt-cli analyze [options]    sun-hours and viewshed analysis (see rt-cli analyze --help)\n"
        "\n"
        "  --scene NAME   terrain | showcase (default terrain)\n"
        "  --width N      image width in pixels (default 1200)\n"
        "  --spp N        samples per pixel (default 500)\n"
        "  --threads N    worker threads, 0 = all hardware threads (default 0)\n"
        "  --seed N       random seed for the scene and render (default 1)\n"
        "  --no-bvh       intersect objects linearly instead of through a BVH\n"
        "  --quiet        don't print progress\n"
        "  --out PATH     output image, .png or .ppm (default image.png)\n"
        "\n"
        "Terrain scene options:\n" << terrain_usage <<
        "  --no-sun       light with the sky alone, no sun or shadow rays\n";
}

static bool parse_options(int argc, char* argv[], options& opts) {
    arg_cursor args(argc, argv, 1);
    while (!args.done()) {
        const std::string arg = args.next();
        bool ok = true;

        const parse_result terrain = parse_terrain_option(arg, args, opts.terrain);
        if (terrain == parse_result::ok) continue;
        if (terrain == parse_result::bad_value) ok = false;
        else if (arg == "--scene")   ok = args.value(opts.scene);
        else if (arg == "--width")   ok = args.read_int(opts.image_width, 1);
        else if (arg == "--spp")     ok = args.read_int(opts.samples_per_pixel, 1);
        else if (arg == "--threads") ok = args.read_int(opts.threads, 0);
        else if (arg == "--out")     ok = args.value(opts.output);
        else if (arg == "--no-bvh")  opts.use_bvh = false;
        else if (arg == "--quiet")   opts.quiet = true;
        else if (arg == "--no-sun")  opts.no_sun = true;
        else if (arg == "--seed") {
            long long n = 0;
            ok = args.value(n);
            opts.seed = uint64_t(n);
        }
        else if (arg == "--help") {
            print_usage();
            std::exit(0);
        }
        else {
            std::cerr << "Invalid argument: " << arg << "\n";
            return false;
        }

        if (!ok) {
            std::cerr << "Invalid value for " << arg << "\n";
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
    if (argc > 1 && std::string(argv[1]) == "analyze")
        return run_analyze(argc, argv);

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
