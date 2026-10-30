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
    int         image_width       = 1200;
    int         samples_per_pixel = 500;
    int         threads           = 0;
    uint64_t    seed              = 1;
    bool        use_bvh           = true;
    bool        quiet             = false;
    std::string output            = "image.png";
};

static void print_usage() {
    std::cerr <<
        "Usage: rt-cli [options]\n"
        "  --width N      image width in pixels (default 1200)\n"
        "  --spp N        samples per pixel (default 500)\n"
        "  --threads N    worker threads, 0 = all hardware threads (default 0)\n"
        "  --seed N       random seed for the scene and render (default 1)\n"
        "  --no-bvh       intersect objects linearly instead of through a BVH\n"
        "  --quiet        don't print progress\n"
        "  --out PATH     output image, .png or .ppm (default image.png)\n";
}

static bool parse_options(int argc, char* argv[], options& opts) {
    for (int k = 1; k < argc; k++) {
        std::string arg = argv[k];
        auto next_value = [&](long long& value) {
            if (k + 1 >= argc) return false;
            try { value = std::stoll(argv[++k]); } catch (...) { return false; }
            return true;
        };

        long long value;
        if (arg == "--width" && next_value(value) && value > 0)        opts.image_width = int(value);
        else if (arg == "--spp" && next_value(value) && value > 0)     opts.samples_per_pixel = int(value);
        else if (arg == "--threads" && next_value(value) && value >= 0) opts.threads = int(value);
        else if (arg == "--seed" && next_value(value))                 opts.seed = uint64_t(value);
        else if (arg == "--no-bvh")                                    opts.use_bvh = false;
        else if (arg == "--quiet")                                     opts.quiet = true;
        else if (arg == "--out" && k + 1 < argc)                       opts.output = argv[++k];
        else {
            std::cerr << "Invalid argument: " << arg << "\n";
            return false;
        }
    }
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
    hittable_list scene = showcase_scene(cam);
    cam.image_width = opts.image_width;
    cam.initialize();

    shared_ptr<hittable> world = opts.use_bvh ? shared_ptr<hittable>(make_shared<bvh_node>(scene))
                                              : make_shared<hittable_list>(scene);

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
