#ifndef TRIANGLE_H
#define TRIANGLE_H

#include "geometry/hittable.h"


// One corner of a triangle. Normals and surface coordinates are optional: leave the normal at
// zero and the triangle shades flat with its geometric normal.
struct vertex {
    point3 p;
    vec3   n = vec3(0,0,0);
    double u = 0, v = 0;
};

// A single triangle, intersected with Moller-Trumbore. The test solves for the barycentric
// coordinates and the ray parameter in one go, needs no precomputed plane equation, and can
// reject a miss after the first of the two barycentric comparisons.
class triangle : public hittable {
  public:
    // Flat-shaded triangle.
    triangle(const point3& a, const point3& b, const point3& c, shared_ptr<material> mat)
      : triangle(vertex{a}, vertex{b}, vertex{c}, mat) {}

    // Per-vertex normals give smooth shading across a mesh; per-vertex u,v are interpolated
    // into the hit record for texture and overlay lookups.
    triangle(const vertex& a, const vertex& b, const vertex& c, shared_ptr<material> mat)
      : v0(a.p), e1(b.p - a.p), e2(c.p - a.p), mat(mat),
        uv0{a.u, a.v}, uv1{b.u, b.v}, uv2{c.u, c.v}
    {
        const vec3 geometric = cross(e1, e2);
        const vec3 unit_geometric = geometric.near_zero() ? vec3(0,1,0) : unit_vector(geometric);
        n0 = a.n.near_zero() ? unit_geometric : unit_vector(a.n);
        n1 = b.n.near_zero() ? unit_geometric : unit_vector(b.n);
        n2 = c.n.near_zero() ? unit_geometric : unit_vector(c.n);

        bbox = aabb(aabb(a.p, b.p), aabb(a.p, c.p)).padded(1e-8);
    }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        const vec3 pvec = cross(r.direction(), e2);
        const double det = dot(e1, pvec);

        // The ray runs parallel to the triangle's plane (or the triangle is degenerate).
        if (std::fabs(det) < 1e-12)
            return false;

        const double inv_det = 1.0 / det;
        const vec3 tvec = r.origin() - v0;

        const double b1 = dot(tvec, pvec) * inv_det;
        if (b1 < 0 || b1 > 1)
            return false;

        const vec3 qvec = cross(tvec, e1);
        const double b2 = dot(r.direction(), qvec) * inv_det;
        if (b2 < 0 || b1 + b2 > 1)
            return false;

        const double t = dot(e2, qvec) * inv_det;
        if (!ray_t.surrounds(t))
            return false;

        // b1 and b2 weight the second and third vertices; b0 weights the first.
        const double b0 = 1 - b1 - b2;

        rec.t = t;
        rec.p = r.at(t);
        rec.u = b0*uv0[0] + b1*uv1[0] + b2*uv2[0];
        rec.v = b0*uv0[1] + b1*uv1[1] + b2*uv2[1];
        rec.set_face_normal(r, unit_vector(b0*n0 + b1*n1 + b2*n2));
        rec.mat = mat.get();

        return true;
    }

    aabb bounding_box() const override { return bbox; }

  private:
    point3 v0;      // First vertex
    vec3   e1, e2;  // Edges to the second and third vertex
    vec3   n0, n1, n2;
    shared_ptr<material> mat;
    double uv0[2], uv1[2], uv2[2];
    aabb   bbox;
};

#endif
