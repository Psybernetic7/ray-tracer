// The browser's view of the engine: the studio class exposed to JavaScript through embind.
//
// Everything interesting lives in studio.h. This file only converts between C++ and JavaScript
// values: numbers and strings go straight through, results come back as plain JS objects, and
// the image is handed over as a view into WebAssembly memory instead of a copy.

#include "studio.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstring>
#include <string>
#include <vector>

using emscripten::val;


namespace {

// Decodes a Terrarium PNG from its file bytes. The browser's own image decoders may apply colour
// management to a PNG, which would quietly change the heights its colours encode, so the bytes
// are decoded here with the same stb_image the CLI uses.
bool decode_terrarium_png(const val& bytes, dem_grid& out, std::string& error) {
    const std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
    int w = 0, h = 0, channels = 0;
    unsigned char* rgb = stbi_load_from_memory(data.data(), int(data.size()), &w, &h, &channels, 3);
    if (!rgb) {
        error = std::string("could not decode elevation PNG: ") + stbi_failure_reason();
        return false;
    }
    out = decode_terrarium(rgb, w, h);
    stbi_image_free(rgb);
    return true;
}

val solar_to_js(const solar_position& sun) {
    val o = val::object();
    o.set("elevation", sun.elevation);
    o.set("azimuth", sun.azimuth);
    return o;
}

}  // namespace


class web_studio {
  public:
    explicit web_studio(int threads) : s(threads) {}

    void generate(int seed, double ridge, double warp, double peak_height, int resolution) {
        terrain_params p;
        p.seed = uint64_t(seed);
        p.ridge_mix = ridge;
        p.warp = warp;
        s.generate(p, resolution, peak_height);
    }

    bool load_place(const std::string& name, const val& png, int resolution) {
        const place* p = find_place(name.c_str());
        if (!p) {
            last_error = "unknown place: " + name;
            return false;
        }
        auto grid = make_shared<dem_grid>();
        if (!decode_terrarium_png(png, *grid, last_error)) return false;
        s.load_place(*p, grid, resolution);
        return true;
    }

    // Returns the tiles to fetch, as [{z, x, y}, ...].
    val begin_region(double lat, double lon, double extent_km, int resolution) {
        val list = val::array();
        int k = 0;
        for (const tile_id& t : s.begin_region(lat, lon, extent_km, resolution)) {
            val tile = val::object();
            tile.set("z", t.z);
            tile.set("x", t.x);
            tile.set("y", t.y);
            list.set(k++, tile);
        }
        return list;
    }

    bool add_tile(int x, int y, const val& png) {
        dem_grid tile;
        if (!decode_terrarium_png(png, tile, last_error)) return false;
        if (tile.width != dem_tile_size || tile.height != dem_tile_size) {
            last_error = "an elevation tile was not 256x256";
            return false;
        }
        s.add_tile(x, y, std::move(tile));
        return true;
    }

    bool finish_region(const std::string& label) { return s.finish_region(label, last_error); }

    std::string error() const { return last_error; }

    void set_size(int w, int h) { s.set_size(w, h); }
    void set_orbit(double azimuth, double elevation, double distance) { s.set_orbit(azimuth, elevation, distance); }
    val set_sun_time(double day, double hour) { return solar_to_js(s.set_sun_time(day, hour)); }
    val sun() const { return solar_to_js(s.sun_now()); }

    void set_mode(int mode) { s.set_mode(studio_mode(std::clamp(mode, 0, 3))); }
    void set_latitude(double degrees) { s.set_latitude(degrees); }
    void set_grid_size(int cells) { s.set_grid_size(cells); }
    bool set_observer(double u, double v) { return s.set_observer(u, v); }
    void set_observer_height(double metres) { s.set_observer_height_m(metres); }

    bool analysis_pending() const { return s.analysis_pending(); }
    void run_analysis() { s.run_analysis(); }
    void render_pass(int samples) { s.render_pass(samples); }

    int width() const { return s.width(); }
    int height() const { return s.height(); }
    int samples() const { return s.samples(); }

    // A view straight into WebAssembly memory, valid until the next call into the studio. The
    // caller copies it out before posting it to the page.
    val pixels() {
        const std::vector<uint8_t>& rgba = s.rgba();
        return val(emscripten::typed_memory_view(rgba.size(), rgba.data()));
    }

    val pick(double fx, double fy) {
        const pick_result p = s.pick(fx, fy);
        val o = val::object();
        o.set("hit", p.hit);
        o.set("onTerrain", p.on_terrain);
        o.set("u", p.u);
        o.set("v", p.v);
        o.set("elevation", p.elevation_m);
        o.set("geo", p.geo);
        o.set("lat", p.lat);
        o.set("lon", p.lon);
        o.set("value", p.value);  // NaN arrives as NaN
        return o;
    }

    // Everything the page shows about the terrain, the analysis and the timings.
    val info() const {
        const studio_stats& st = s.statistics();
        val o = val::object();
        o.set("threads", s.threads());
        o.set("hasTerrain", s.has_terrain());
        o.set("real", s.is_real());
        o.set("label", s.label());
        o.set("sizeKm", s.size_km());
        o.set("minElevation", s.min_elevation_m());
        o.set("maxElevation", s.max_elevation_m());
        o.set("latitude", s.latitude());
        o.set("triangles", double(st.triangles));
        o.set("terrainSeconds", st.terrain_seconds);
        o.set("bvhSeconds", st.bvh_seconds);
        o.set("gridSeconds", st.grid_seconds);
        o.set("horizonSeconds", st.horizon_seconds);
        o.set("horizonRays", double(st.horizon_rays));
        o.set("analysisSeconds", st.analysis_seconds);
        o.set("analysisRays", double(st.analysis_rays));
        o.set("sunPositions", double(st.sun_positions));
        o.set("passSeconds", st.pass_seconds);

        o.set("hasResult", s.has_result());
        if (s.has_result()) {
            const interval r = s.result_range();
            o.set("min", r.min);
            o.set("max", r.max);
            o.set("mean", s.result_mean());
        }
        double u, v;
        if (s.observer(u, v)) {
            o.set("observerU", u);
            o.set("observerV", v);
        }
        return o;
    }

    // The current colormap as `stops` sRGB colours from low to high, for the legend: the same
    // colours the render uses, computed by the same code.
    val legend(int stops) const {
        stops = std::clamp(stops, 2, 256);
        const overlay_colors& colors = s.result_colors();
        const interval r = colors.kind == overlay_kind::viewshed ? interval(0, 1) : s.result_range();
        val list = val::array();
        for (int k = 0; k < stops; k++) {
            const double t = double(k) / (stops - 1);
            const color c = colors(r.size() > 0 ? r.min + t * r.size() : 0.5);
            val rgb = val::array();
            rgb.set(0, int(255 * c.x() + 0.5));
            rgb.set(1, int(255 * c.y() + 0.5));
            rgb.set(2, int(255 * c.z() + 0.5));
            list.set(k, rgb);
        }
        return list;
    }

    // The bundled places, as [{name, label, lat, lon, extentKm}, ...].
    static val places() {
        val list = val::array();
        int k = 0;
        for (const place& p : known_places) {
            val o = val::object();
            o.set("name", std::string(p.name));
            o.set("label", std::string(p.label));
            o.set("lat", p.lat);
            o.set("lon", p.lon);
            o.set("extentKm", p.extent_km);
            list.set(k++, o);
        }
        return list;
    }

  private:
    studio s;
    std::string last_error;
};


EMSCRIPTEN_BINDINGS(studio_module) {
    emscripten::class_<web_studio>("Studio")
        .constructor<int>()
        .class_function("places", &web_studio::places)
        .function("generate", &web_studio::generate)
        .function("loadPlace", &web_studio::load_place)
        .function("beginRegion", &web_studio::begin_region)
        .function("addTile", &web_studio::add_tile)
        .function("finishRegion", &web_studio::finish_region)
        .function("error", &web_studio::error)
        .function("setSize", &web_studio::set_size)
        .function("setOrbit", &web_studio::set_orbit)
        .function("setSunTime", &web_studio::set_sun_time)
        .function("sun", &web_studio::sun)
        .function("setMode", &web_studio::set_mode)
        .function("setLatitude", &web_studio::set_latitude)
        .function("setGridSize", &web_studio::set_grid_size)
        .function("setObserver", &web_studio::set_observer)
        .function("setObserverHeight", &web_studio::set_observer_height)
        .function("analysisPending", &web_studio::analysis_pending)
        .function("runAnalysis", &web_studio::run_analysis)
        .function("renderPass", &web_studio::render_pass)
        .function("width", &web_studio::width)
        .function("height", &web_studio::height)
        .function("samples", &web_studio::samples)
        .function("pixels", &web_studio::pixels)
        .function("pick", &web_studio::pick)
        .function("info", &web_studio::info)
        .function("legend", &web_studio::legend);
}
