#include "image_io.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <fstream>


static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool write_ppm(const std::vector<rgb8>& pixels, int width, int height, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;

    // Binary PPM (P6): a short text header followed by raw RGB bytes.
    out << "P6\n" << width << ' ' << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size() * sizeof(rgb8)));
    return bool(out);
}

bool write_image(const framebuffer& fb, const std::string& path, std::string& error) {
    static_assert(sizeof(rgb8) == 3, "rgb8 must be tightly packed for image writers");
    const auto pixels = fb.to_rgb8();

    bool ok;
    if (ends_with(path, ".png")) {
        ok = stbi_write_png(path.c_str(), fb.width(), fb.height(), 3, pixels.data(), fb.width() * 3) != 0;
    } else if (ends_with(path, ".ppm")) {
        ok = write_ppm(pixels, fb.width(), fb.height(), path);
    } else {
        error = "unsupported output format (use .png or .ppm): " + path;
        return false;
    }

    if (!ok)
        error = "failed to write " + path;
    return ok;
}
