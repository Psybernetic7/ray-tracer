#ifndef OVERLAY_H
#define OVERLAY_H

#include "rtheader.h"
#include "math/colormap.h"


// How an analysis result is coloured, shared by everything that shows one: the CLI's flat maps
// and renders, and the browser app. Colours here are sRGB, the space images are stored in.

enum class overlay_kind {
    sun_hours,   // Hours of direct sun, on viridis
    irradiance,  // Solar energy, on inferno, which reads as heat
    viewshed,    // 1 visible, 0 hidden
};

inline const color viewshed_seen_color   = color(1.00, 0.62, 0.20);
inline const color viewshed_hidden_color = color(0.16, 0.17, 0.20);
inline const color no_data_color         = color(0.35, 0.35, 0.35);

// Turns a value into an sRGB colour. `range` is the span the colormap is stretched over; the
// viewshed ignores it.
struct overlay_colors {
    overlay_kind kind = overlay_kind::sun_hours;
    interval range;

    color operator()(double value) const {
        if (kind == overlay_kind::viewshed)
            return (1 - value)*viewshed_hidden_color + value*viewshed_seen_color;
        const double t = range.size() > 0 ? (value - range.min) / range.size() : 0.5;
        return kind == overlay_kind::irradiance ? inferno(t) : viridis(t);
    }
};

// The albedo a terrain point is repainted with in a 3D render, given the value there and the
// albedo the terrain material picked (`base`, linear). Missing data keeps the terrain's colour.
inline color overlay_albedo(const overlay_colors& colors, double value, const color& base) {
    if (std::isnan(value))
        return base;
    if (colors.kind == overlay_kind::viewshed) {
        // Keep the terrain's own colours, warmed where visible and darkened where hidden.
        const color seen = 0.4*base + 0.6*srgb_to_linear(viewshed_seen_color);
        return (1 - value)*(0.35*base) + value*seen;
    }
    return srgb_to_linear(colors(value));
}

#endif
