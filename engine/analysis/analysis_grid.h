#ifndef ANALYSIS_GRID_H
#define ANALYSIS_GRID_H

#include "rtheader.h"
#include "geometry/hittable.h"
#include "parallel.h"

#include <vector>


// The points an analysis is evaluated at: a regular grid over the terrain's footprint (centred
// on the origin, size_x by size_z), each point placed on the surface by dropping a ray onto it.
//
// Finding the ground with a ray rather than reading the height grid matters. Interpolating
// the grid bilinearly would put points slightly above or below the triangles that rays are
// actually tested against, and a point below the mesh is in permanent shadow. Dropping a ray
// gets the exact surface whatever the geometry is, which also keeps this working unchanged
// when M3 swaps fractal terrain for real elevation data.
class analysis_grid {
  public:
    analysis_grid(const hittable& world, double size_x, double size_z, int width, int height,
                  int thread_count = 0)
      : grid_width(width < 1 ? 1 : width), grid_height(height < 1 ? 1 : height),
        extent_x(size_x), extent_z(size_z),
        surface(size_t(grid_width) * size_t(grid_height)),
        normals(size_t(grid_width) * size_t(grid_height), vec3(0,1,0)),
        found(size_t(grid_width) * size_t(grid_height), 0)
    {
        // Rays start from these points, so lift them off the surface along the normal: far
        // enough that a ray doesn't hit the triangle it starts on, small against any feature.
        surface_lift = 1e-4 * std::fmax(extent_x, extent_z);

        const double top = world.bounding_box().y.max + 1.0;

        parallel_for(grid_height, thread_count, [&](int y) {
            for (int x = 0; x < grid_width; x++) {
                const point3 above(world_x(x), top, world_z(y));
                hit_record rec;
                if (!world.hit(ray(above, vec3(0,-1,0)), interval(0, infinity), rec))
                    continue;

                const size_t k = index(x, y);
                surface[k] = rec.p;
                normals[k] = rec.normal;  // The ray came from above, so this faces up
                found[k] = 1;
            }
        });
    }

    int width() const  { return grid_width; }
    int height() const { return grid_height; }

    // False where the dropped ray missed (outside the terrain, or a hole in the data).
    bool valid(int x, int y) const { return found[index(x, y)] != 0; }

    // The ground point itself, and the point rays leave from, just above it.
    const point3& ground(int x, int y) const { return surface[index(x, y)]; }
    point3 origin(int x, int y) const { return ground(x, y) + surface_lift * normal(x, y); }

    const vec3& normal(int x, int y) const { return normals[index(x, y)]; }

    double lift() const { return surface_lift; }

    // The valid cell with the highest ground: the natural default viewpoint for a viewshed.
    bool highest_cell(int& best_x, int& best_y) const {
        bool any = false;
        for (int y = 0; y < grid_height; y++) {
            for (int x = 0; x < grid_width; x++) {
                if (!valid(x, y)) continue;
                if (!any || ground(x, y).y() > ground(best_x, best_y).y()) {
                    best_x = x;
                    best_y = y;
                    any = true;
                }
            }
        }
        return any;
    }

    // World position of cell centres, matching scalar_map's cell-centred u,v.
    double world_x(int x) const { return extent_x * ((x + 0.5) / grid_width - 0.5); }
    double world_z(int y) const { return extent_z * ((y + 0.5) / grid_height - 0.5); }

  private:
    int    grid_width, grid_height;
    double extent_x, extent_z;
    double surface_lift;
    std::vector<point3> surface;
    std::vector<vec3> normals;
    std::vector<unsigned char> found;  // Not vector<bool>: threads write neighbouring cells

    size_t index(int x, int y) const { return size_t(y) * size_t(grid_width) + size_t(x); }
};

#endif
