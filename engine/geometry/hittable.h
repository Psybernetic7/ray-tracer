#ifndef HITTABLE_H
#define HITTABLE_H

#include "rtheader.h"
#include "geometry/aabb.h"

class material;

class hit_record {
  public:
    point3 p;
    vec3 normal;
    // A raw pointer, not shared_ptr: hit records are copied on every intersection, and copying
    // a shared_ptr is an atomic refcount update that threads would contend on. The scene owns
    // the materials and outlives every render.
    const material* mat;
    double t;
    bool front_face;

    void set_face_normal(const ray& r, const vec3& outward_normal) {
        // Sets the hit record normal vector.
        // NOTE: the parameter `outward_normal` is assumed to have unit length.

        front_face = dot(r.direction(), outward_normal) < 0;
        normal = front_face ? outward_normal : -outward_normal;
    }
};

class hittable {
  public:
    virtual ~hittable() = default;

    virtual bool hit(const ray& r, interval ray_t, hit_record& rec) const = 0;

    virtual aabb bounding_box() const = 0;
};

#endif
