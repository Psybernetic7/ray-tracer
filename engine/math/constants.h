#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <limits>


// Constants

inline constexpr double infinity = std::numeric_limits<double>::infinity();
inline constexpr double pi = 3.1415926535897932385;

// Utility Functions

inline double degrees_to_radians(double degrees) {
    return degrees * pi / 180.0;
}

inline double lerp(double a, double b, double t) {
    return a + t*(b - a);
}

inline double smoothstep(double edge0, double edge1, double x) {
    // 0 below edge0, 1 above edge1, with a smooth S-curve in between.
    if (edge1 <= edge0) return x < edge0 ? 0.0 : 1.0;
    double t = (x - edge0) / (edge1 - edge0);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t*t*(3 - 2*t);
}

#endif
