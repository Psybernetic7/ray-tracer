#ifndef AABB_H
#define AABB_H

#include "rtheader.h"

// Axis-aligned bounding box, stored as one interval per axis.
class aabb {
  public:
    interval x, y, z;

    aabb() {} // The default AABB is empty, since intervals are empty by default.

    aabb(const interval& x, const interval& y, const interval& z) : x(x), y(y), z(z) {}

    aabb(const point3& a, const point3& b) {
        // Treat the two points a and b as extrema for the bounding box, so we don't require a
        // particular minimum/maximum coordinate order.
        x = (a[0] <= b[0]) ? interval(a[0], b[0]) : interval(b[0], a[0]);
        y = (a[1] <= b[1]) ? interval(a[1], b[1]) : interval(b[1], a[1]);
        z = (a[2] <= b[2]) ? interval(a[2], b[2]) : interval(b[2], a[2]);
    }

    aabb(const aabb& box0, const aabb& box1) {
        x = interval(std::fmin(box0.x.min, box1.x.min), std::fmax(box0.x.max, box1.x.max));
        y = interval(std::fmin(box0.y.min, box1.y.min), std::fmax(box0.y.max, box1.y.max));
        z = interval(std::fmin(box0.z.min, box1.z.min), std::fmax(box0.z.max, box1.z.max));
    }

    const interval& axis_interval(int n) const {
        if (n == 1) return y;
        if (n == 2) return z;
        return x;
    }

    bool hit(const ray& r, interval ray_t) const {
        // Slab test: clip the ray's t-range against each pair of axis planes in turn.
        const point3& ray_orig = r.origin();
        const vec3&   ray_dir  = r.direction();

        for (int axis = 0; axis < 3; axis++) {
            const interval& ax = axis_interval(axis);
            const double adinv = 1.0 / ray_dir[axis];

            auto t0 = (ax.min - ray_orig[axis]) * adinv;
            auto t1 = (ax.max - ray_orig[axis]) * adinv;

            if (t0 < t1) {
                if (t0 > ray_t.min) ray_t.min = t0;
                if (t1 < ray_t.max) ray_t.max = t1;
            } else {
                if (t1 > ray_t.min) ray_t.min = t1;
                if (t0 < ray_t.max) ray_t.max = t0;
            }

            if (ray_t.max <= ray_t.min)
                return false;
        }
        return true;
    }

    int longest_axis() const {
        // Returns the index of the longest axis of the bounding box.
        if (x.size() > y.size())
            return x.size() > z.size() ? 0 : 2;
        else
            return y.size() > z.size() ? 1 : 2;
    }

    point3 centroid() const {
        return point3(0.5*(x.min + x.max), 0.5*(y.min + y.max), 0.5*(z.min + z.max));
    }
};

#endif
