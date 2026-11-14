#ifndef COLORMAP_H
#define COLORMAP_H

#include "math/color.h"


// Perceptually uniform colormaps for analysis output. Equal steps in value look like equal steps
// in colour, they stay readable in greyscale and to colour-blind viewers, and they never invent
// the false boundaries a rainbow map does.
//
// Both are degree-6 polynomial fits to the matplotlib maps (Matt Zucker, public domain): far
// smaller than a 256-entry table, and within about 6 levels out of 255 of it, which is too
// little to see in a continuous gradient. They return sRGB
// values in [0,1], ready for an 8-bit image. The renderer works in linear light, so convert
// with srgb_to_linear before using one as an albedo.

inline color colormap_polynomial(double t, const color c[7]) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    color result = c[6];
    for (int k = 5; k >= 0; k--)
        result = c[k] + t*result;  // Horner's rule
    return color(std::fmin(std::fmax(result.x(), 0.0), 1.0),
                 std::fmin(std::fmax(result.y(), 0.0), 1.0),
                 std::fmin(std::fmax(result.z(), 0.0), 1.0));
}

// Dark purple through teal to yellow. The default for sun-hours.
inline color viridis(double t) {
    static const color c[7] = {
        color( 0.2777273272234177,  0.005407344544966578,  0.3340998053353061),
        color( 0.1050930431085774,  1.404613529898575,     1.384590162594685),
        color(-0.3308618287255563,  0.214847559468213,     0.09509516302823659),
        color(-4.634230498983486,  -5.799100973351585,   -19.33244095627987),
        color( 6.228269936347081,  14.17993336680509,     56.69055260068105),
        color( 4.776384997670288, -13.74514537774601,    -65.35303263337234),
        color(-5.435455855934631,   4.645852612178535,    26.3124352495832),
    };
    return colormap_polynomial(t, c);
}

// Black through red and orange to pale yellow. Reads as heat, which suits irradiance.
inline color inferno(double t) {
    static const color c[7] = {
        color(  0.0002189403691192265,  0.001651004631001012, -0.01948089843709184),
        color(  0.1065134194856116,     0.5639564367884091,    3.932712388889277),
        color( 11.60249308247187,      -3.972853965665698,   -15.9423941062914),
        color(-41.70399613139459,      17.43639888205313,     44.35414519872813),
        color( 77.162935699427,       -33.40235894210092,    -81.80730925738993),
        color(-71.31942824499214,      32.62606426397723,     73.20951985803202),
        color( 25.13112622477341,     -12.24266895238567,    -23.07032500287172),
    };
    return colormap_polynomial(t, c);
}

// The renderer's output stage applies gamma 2 (see to_rgb8), so squaring is its exact inverse:
// a colormap colour used as an albedo comes back out as that same colour on a lit surface.
inline color srgb_to_linear(const color& c) {
    return c * c;
}

#endif
