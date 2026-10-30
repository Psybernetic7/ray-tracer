#ifndef RENDERER_H
#define RENDERER_H

#include "rtheader.h"
#include "geometry/hittable.h"
#include "render/camera.h"
#include "render/framebuffer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>


// Splits the image into square tiles and renders them on a pool of threads. Threads pull the
// next tile from a shared counter, so fast threads naturally pick up more work.
class renderer {
  public:
    int tile_size    = 32;  // Tile edge length in pixels
    int thread_count = 0;   // Worker threads; 0 means one per hardware thread
    uint64_t seed    = 0;   // The same seed always produces the same image

    // Called after each finished tile with (tiles_done, tiles_total). Calls never overlap.
    std::function<void(int, int)> on_progress;

    // Adds `samples` samples per pixel to `fb`. The camera must be initialized and match the
    // framebuffer's dimensions.
    void render_pass(const camera& cam, const hittable& world, framebuffer& fb, int samples) const {
        const int tiles_x = (fb.width()  + tile_size - 1) / tile_size;
        const int tiles_y = (fb.height() + tile_size - 1) / tile_size;
        const int tile_count = tiles_x * tiles_y;

        // Seeding per pass and per tile (not per thread) makes the image independent of which
        // thread happens to render which tile.
        const uint64_t pass_seed = mix_seed(seed, uint64_t(fb.samples_per_pixel()));

        std::atomic<int> next_tile{0};
        std::atomic<int> tiles_done{0};
        std::mutex progress_mutex;

        auto worker = [&]() {
            for (int tile = next_tile++; tile < tile_count; tile = next_tile++) {
                seed_random(mix_seed(pass_seed, uint64_t(tile)));

                const int x0 = (tile % tiles_x) * tile_size;
                const int y0 = (tile / tiles_x) * tile_size;
                const int x1 = std::min(x0 + tile_size, fb.width());
                const int y1 = std::min(y0 + tile_size, fb.height());

                for (int j = y0; j < y1; j++) {
                    for (int i = x0; i < x1; i++) {
                        color sample_sum(0,0,0);
                        for (int sample = 0; sample < samples; sample++)
                            sample_sum += cam.ray_color(cam.get_ray(i, j), cam.max_depth, world);
                        fb.add_sample_sum(i, j, sample_sum);
                    }
                }

                const int done = ++tiles_done;
                if (on_progress) {
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    on_progress(done, tile_count);
                }
            }
        };

        int threads_to_use = thread_count > 0 ? thread_count
                                              : int(std::thread::hardware_concurrency());
        threads_to_use = std::clamp(threads_to_use, 1, tile_count);

        // The calling thread works too, so spawn one fewer.
        std::vector<std::thread> helpers;
        for (int t = 1; t < threads_to_use; t++)
            helpers.emplace_back(worker);
        worker();
        for (auto& helper : helpers)
            helper.join();

        fb.finish_pass(samples);
    }
};

#endif
