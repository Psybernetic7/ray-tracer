#ifndef IMAGE_IO_H
#define IMAGE_IO_H

#include "render/framebuffer.h"

#include <cstdint>
#include <string>
#include <vector>


// Writes the framebuffer to `path`. The format is chosen by extension: .png or .ppm.
// Returns false (with a message in `error`) if the extension is unknown or the write fails.
bool write_image(const framebuffer& fb, const std::string& path, std::string& error);

// Writes already-converted 8-bit pixels, row by row from the top, in the same formats.
bool write_image(const std::vector<rgb8>& pixels, int width, int height, const std::string& path,
                 std::string& error);

// Reads a PNG as tightly packed 8-bit RGB, rows from the top. Alpha or grey is converted.
bool read_rgb_image(const std::string& path, int& width, int& height, std::vector<uint8_t>& rgb,
                    std::string& error);

#endif
