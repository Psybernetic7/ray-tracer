#ifndef DEM_SOURCE_H
#define DEM_SOURCE_H

#include "scenes.h"

#include <string>


// Resolves a --place or --center request into elevation data, and sets the terrain up to use it:
// the grid, the world size (one unit per kilometre) and the place's look.
//
// A named place loads its bundled copy from data/places/ unless --download is given. Anything
// else comes from AWS Terrain Tiles: the tiles covering the region are downloaded with curl
// into a local cache (each tile once, ever), decoded, stitched and resampled to the grid.
//
// Does nothing and returns true when no real terrain was asked for.
bool load_dem(terrain_options& opts, int thread_count, std::string& error);

// One line of credit for the elevation data, to print after using it.
extern const char* dem_attribution;

#endif
