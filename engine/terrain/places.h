#ifndef PLACES_H
#define PLACES_H

#include <cstring>


// Well-known places with a region, and the look that suits them. Each is a square centred on a
// summit or viewpoint. A copy of each region's elevation is bundled with the project
// (data/places/<name>.png), so they render and analyze offline and in CI.

enum class terrain_palette {
    alpine,  // Grass, grey rock, snow: the fractal terrain's default
    desert,  // Sand and red sandstone, no snow
};

struct place {
    const char* name;        // What --place takes
    const char* label;       // For printing
    double lat, lon;         // Centre, degrees north and east
    double extent_km;        // Side of the square
    double snow_line_m;      // Snow above this altitude; <= 0 for none
    terrain_palette palette;
};

inline constexpr place known_places[] = {
    // The canyon north of Grand Canyon Village: the South Rim (~2,150 m), the Colorado River
    // (~750 m) and the higher North Rim (~2,500 m), 16 km across.
    {"grand-canyon", "Grand Canyon, Arizona",      36.1100, -112.1000, 24.0,    0, terrain_palette::desert},
    // The Matterhorn (4,478 m) above Zermatt, with the glaciers of the Swiss-Italian border.
    {"matterhorn",   "Matterhorn, Swiss Alps",     45.9763,    7.6586, 16.0, 2900, terrain_palette::alpine},
    // Mount Fuji (3,776 m), the whole cone and its foothills.
    {"fuji",         "Mount Fuji, Japan",          35.3606,  138.7274, 32.0, 2900, terrain_palette::alpine},
};

inline const place* find_place(const char* name) {
    for (const place& p : known_places)
        if (std::strcmp(p.name, name) == 0) return &p;
    return nullptr;
}

#endif
