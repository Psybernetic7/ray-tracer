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

#endif
