#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "rtheader.h"

#include <algorithm>
#include <vector>


// Accumulates radiance samples per pixel. Rendering can happen in several passes (for
// progressive display); the image is always the running average of everything added so far.
class framebuffer {
  public:
    framebuffer(int width, int height)
      : image_width(width), image_height(height), accum(size_t(width) * height) {}

    int width() const  { return image_width; }
    int height() const { return image_height; }
    int samples_per_pixel() const { return samples; }

    void add_sample_sum(int i, int j, const color& sample_sum) {
        accum[size_t(j) * image_width + i] += sample_sum;
    }

    void finish_pass(int pass_samples) {
        samples += pass_samples;
    }

    void clear() {
        std::fill(accum.begin(), accum.end(), color(0,0,0));
        samples = 0;
    }

    color pixel(int i, int j) const {
        if (samples == 0)
            return color(0,0,0);
        return accum[size_t(j) * image_width + i] / samples;
    }

    std::vector<rgb8> to_rgb8() const {
        // Converts the averaged image to gamma-corrected 8-bit pixels, row by row from the top.
        std::vector<rgb8> pixels;
        pixels.reserve(accum.size());
        for (int j = 0; j < image_height; j++)
            for (int i = 0; i < image_width; i++)
                pixels.push_back(::to_rgb8(pixel(i, j)));
        return pixels;
    }

  private:
    int image_width;
    int image_height;
    int samples = 0;            // Samples per pixel accumulated so far
    std::vector<color> accum;   // Sum of all samples for each pixel
};

#endif
