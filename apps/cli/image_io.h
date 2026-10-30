#ifndef IMAGE_IO_H
#define IMAGE_IO_H

#include "render/framebuffer.h"

#include <string>


// Writes the framebuffer to `path`. The format is chosen by extension: .png or .ppm.
// Returns false (with a message in `error`) if the extension is unknown or the write fails.
bool write_image(const framebuffer& fb, const std::string& path, std::string& error);

#endif
