#ifndef BVH_H
#define BVH_H

#include "rtheader.h"
#include "geometry/aabb.h"
#include "geometry/hittable.h"
#include "geometry/hittable_list.h"

#include <algorithm>
#include <vector>


// Bounding volume hierarchy: a binary tree of bounding boxes. A ray that misses a node's box
// skips everything inside it, turning a linear scan over all objects into a logarithmic one.
class bvh_node : public hittable {
  public:
    bvh_node(hittable_list list) : bvh_node(list.objects, 0, list.objects.size()) {
        // There's a C++ subtlety here. This constructor (without span indices) creates an
        // implicit copy of the hittable list, which we will modify. The lifetime of the copied
        // list only extends until this constructor exits. That's OK, because we only need to
        // persist the resulting bounding volume hierarchy.
    }

    bvh_node(std::vector<shared_ptr<hittable>>& objects, size_t start, size_t end) {
        // Build the bounding box of the span of source objects.
        for (size_t object_index = start; object_index < end; object_index++)
            bbox = aabb(bbox, objects[object_index]->bounding_box());

        size_t object_span = end - start;

        if (object_span == 1) {
            left = right = objects[start];
        } else if (object_span == 2) {
            left = objects[start];
            right = objects[start+1];
        } else {
            // Split at the median along the longest axis of the objects' centroids, so both
            // halves are spatially compact.
            aabb centroid_bounds;
            for (size_t object_index = start; object_index < end; object_index++) {
                auto c = objects[object_index]->bounding_box().centroid();
                centroid_bounds = aabb(centroid_bounds, aabb(c, c));
            }
            int axis = centroid_bounds.longest_axis();

            auto mid = start + object_span/2;
            std::nth_element(objects.begin() + start, objects.begin() + mid, objects.begin() + end,
                [axis](const shared_ptr<hittable>& a, const shared_ptr<hittable>& b) {
                    return a->bounding_box().centroid()[axis] < b->bounding_box().centroid()[axis];
                });

            left = make_shared<bvh_node>(objects, start, mid);
            right = make_shared<bvh_node>(objects, mid, end);
        }
    }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        if (!bbox.hit(r, ray_t))
            return false;

        bool hit_left = left->hit(r, ray_t, rec);
        bool hit_right = right->hit(r, interval(ray_t.min, hit_left ? rec.t : ray_t.max), rec);

        return hit_left || hit_right;
    }

    aabb bounding_box() const override { return bbox; }

  private:
    shared_ptr<hittable> left;
    shared_ptr<hittable> right;
    aabb bbox;
};

#endif
