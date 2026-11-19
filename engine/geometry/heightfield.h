#ifndef HEIGHTFIELD_H
#define HEIGHTFIELD_H

#include "geometry/hittable_list.h"
#include "geometry/triangle.h"

#include <vector>


// A regular grid of heights laid over a rectangle in the xz plane, with y up.
//
// The grid stores one height per *vertex*, so a cols x rows field has (cols-1) x (rows-1) cells
// and twice that many triangles. Those triangles go straight into the M0 BVH: the mesh needs no
// traversal code of its own, and a heightfield-specific traverser can be benchmarked against
// the BVH later without changing anything else.
class heightfield {
  public:
    // size_x and size_z are the world extent of the whole field, centred on the origin.
    heightfield(int cols, int rows, double size_x, double size_z)
      : grid_cols(cols < 2 ? 2 : cols), grid_rows(rows < 2 ? 2 : rows),
        extent_x(size_x), extent_z(size_z),
        heights(size_t(grid_cols) * size_t(grid_rows), 0.0) {}

    int cols() const { return grid_cols; }
    int rows() const { return grid_rows; }

    double size_x() const { return extent_x; }
    double size_z() const { return extent_z; }

    // World-space distance between neighbouring vertices.
    double cell_x() const { return extent_x / (grid_cols - 1); }
    double cell_z() const { return extent_z / (grid_rows - 1); }

    double& at(int i, int j)       { return heights[index(i, j)]; }
    double  at(int i, int j) const { return heights[index(i, j)]; }

    // Vertex (i,j) in world space. i runs along x, j along z.
    point3 vertex_position(int i, int j) const {
        return point3(-0.5*extent_x + i*cell_x(), at(i, j), -0.5*extent_z + j*cell_z());
    }

    // Normalized surface coordinates, 0..1 across the field. M2 indexes its sun-hours and
    // viewshed maps with these.
    double u_at(int i) const { return double(i) / (grid_cols - 1); }
    double v_at(int j) const { return double(j) / (grid_rows - 1); }

    // Vertex normal from central differences on the grid. Sharing these between the triangles
    // of neighbouring cells is what turns a faceted mesh into a smooth surface.
    vec3 vertex_normal(int i, int j) const {
        const int i0 = clamp_i(i - 1), i1 = clamp_i(i + 1);
        const int j0 = clamp_j(j - 1), j1 = clamp_j(j + 1);

        const double dhdx = (at(i1, j) - at(i0, j)) / ((i1 - i0) * cell_x());
        const double dhdz = (at(i, j1) - at(i, j0)) / ((j1 - j0) * cell_z());

        // The surface is y = h(x,z), whose gradient gives the normal (-dh/dx, 1, -dh/dz).
        return unit_vector(vec3(-dhdx, 1.0, -dhdz));
    }

    interval height_range() const {
        interval range;
        for (double h : heights) {
            if (h < range.min) range.min = h;
            if (h > range.max) range.max = h;
        }
        return range;
    }

    aabb bounding_box() const {
        const interval y = height_range();
        return aabb(point3(-0.5*extent_x, y.min, -0.5*extent_z),
                    point3( 0.5*extent_x, y.max,  0.5*extent_z));
    }

    // Two triangles per cell, with shared smooth normals and grid surface coordinates. Vertices
    // are wound counter-clockwise seen from above, so each triangle's geometric normal points at
    // the sky like its interpolated one does. The diagonal alternates in a checkerboard so a
    // regular grid doesn't produce a visible directional weave across the whole surface.
    hittable_list to_triangles(shared_ptr<material> mat) const {
        hittable_list mesh;
        mesh.objects.reserve(size_t(grid_cols - 1) * size_t(grid_rows - 1) * 2);

        std::vector<vec3> normals(heights.size());
        for (int j = 0; j < grid_rows; j++)
            for (int i = 0; i < grid_cols; i++)
                normals[index(i, j)] = vertex_normal(i, j);

        auto corner = [&](int i, int j) {
            return vertex{vertex_position(i, j), normals[index(i, j)], u_at(i), v_at(j)};
        };

        for (int j = 0; j < grid_rows - 1; j++) {
            for (int i = 0; i < grid_cols - 1; i++) {
                const vertex a = corner(i,     j);
                const vertex b = corner(i + 1, j);
                const vertex c = corner(i + 1, j + 1);
                const vertex d = corner(i,     j + 1);

                if ((i + j) % 2 == 0) {
                    mesh.add(make_shared<triangle>(a, c, b, mat));
                    mesh.add(make_shared<triangle>(a, d, c, mat));
                } else {
                    mesh.add(make_shared<triangle>(a, d, b, mat));
                    mesh.add(make_shared<triangle>(b, d, c, mat));
                }
            }
        }

        return mesh;
    }

    // Vertical walls from every border edge down to `base_y`, facing outward. Real terrain is
    // cut out of a larger landscape, so its border is a cliff of arbitrary height; walls turn
    // the floating sheet into a solid block, the way a printed relief model looks.
    hittable_list skirt(double base_y, shared_ptr<material> mat) const {
        hittable_list walls;
        auto wall = [&](int i0, int j0, int i1, int j1, const vec3& outward) {
            const point3 a = vertex_position(i0, j0), b = vertex_position(i1, j1);
            const point3 a0(a.x(), base_y, a.z()), b0(b.x(), base_y, b.z());
            // Wind so the geometric normal points outward: (b - a) x (a0 - a) for this order.
            if (dot(cross(b - a, a0 - a), outward) > 0) {
                walls.add(make_shared<triangle>(a, b, a0, mat));
                walls.add(make_shared<triangle>(b, b0, a0, mat));
            } else {
                walls.add(make_shared<triangle>(a, a0, b, mat));
                walls.add(make_shared<triangle>(b, a0, b0, mat));
            }
        };
        for (int i = 0; i < grid_cols - 1; i++) {
            wall(i, 0, i + 1, 0, vec3(0, 0, -1));                         // North
            wall(i, grid_rows - 1, i + 1, grid_rows - 1, vec3(0, 0, 1));  // South
        }
        for (int j = 0; j < grid_rows - 1; j++) {
            wall(0, j, 0, j + 1, vec3(-1, 0, 0));                         // West
            wall(grid_cols - 1, j, grid_cols - 1, j + 1, vec3(1, 0, 0));  // East
        }
        return walls;
    }

  private:
    int    grid_cols, grid_rows;
    double extent_x, extent_z;
    std::vector<double> heights;

    size_t index(int i, int j) const { return size_t(j) * size_t(grid_cols) + size_t(i); }

    int clamp_i(int i) const { return i < 0 ? 0 : (i >= grid_cols ? grid_cols - 1 : i); }
    int clamp_j(int j) const { return j < 0 ? 0 : (j >= grid_rows ? grid_rows - 1 : j); }
};

#endif
