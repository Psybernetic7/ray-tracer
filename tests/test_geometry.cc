#include "doctest.h"

#include "rtheader.h"
#include "geometry/aabb.h"
#include "geometry/bvh.h"
#include "geometry/hittable_list.h"
#include "geometry/sphere.h"
#include "render/material.h"


TEST_CASE("sphere hit returns the nearest root and outward normal") {
    auto mat = make_shared<lambertian>(color(0.5,0.5,0.5));
    sphere s(point3(0,0,-5), 1, mat);
    hit_record rec;

    REQUIRE(s.hit(ray(point3(0,0,0), vec3(0,0,-1)), interval(0.001, infinity), rec));
    CHECK(rec.t == doctest::Approx(4));
    CHECK(rec.normal[2] == doctest::Approx(1));
    CHECK(rec.front_face);
    CHECK(rec.mat == mat.get());

    CHECK_FALSE(s.hit(ray(point3(0,0,0), vec3(0,1,0)), interval(0.001, infinity), rec));
}

TEST_CASE("sphere hit from inside returns the far root") {
    sphere s(point3(0,0,0), 2, make_shared<lambertian>(color(1,1,1)));
    hit_record rec;
    REQUIRE(s.hit(ray(point3(0,0,0), vec3(1,0,0)), interval(0.001, infinity), rec));
    CHECK(rec.t == doctest::Approx(2));
    CHECK_FALSE(rec.front_face);
}

TEST_CASE("aabb slab test, including rays parallel to an axis") {
    aabb box(point3(-1,-1,-1), point3(1,1,1));
    CHECK(box.hit(ray(point3(0,0,-5), vec3(0,0,1)), interval(0, infinity)));
    CHECK_FALSE(box.hit(ray(point3(0,2,-5), vec3(0,0,1)), interval(0, infinity)));
    CHECK_FALSE(box.hit(ray(point3(0,0,-5), vec3(0,0,1)), interval(0, 3)));  // stops short
    CHECK(box.longest_axis() == 2);  // ties resolve to z
}

TEST_CASE("bvh finds exactly the same hits as a linear list") {
    seed_random(123);
    hittable_list list;
    for (int k = 0; k < 300; k++) {
        auto mat = make_shared<lambertian>(color::random());
        list.add(make_shared<sphere>(vec3::random(-10, 10), random_double(0.1, 1.5), mat));
    }
    bvh_node bvh(list);

    int hits = 0;
    for (int k = 0; k < 5000; k++) {
        ray r(vec3::random(-15, 15), random_unit_vector());
        hit_record list_rec, bvh_rec;
        bool list_hit = list.hit(r, interval(0.001, infinity), list_rec);
        bool bvh_hit = bvh.hit(r, interval(0.001, infinity), bvh_rec);

        REQUIRE(list_hit == bvh_hit);
        if (list_hit) {
            hits++;
            CHECK(bvh_rec.t == list_rec.t);
            CHECK(bvh_rec.mat == list_rec.mat);
        }
    }
    CHECK(hits > 500);  // Make sure the test actually exercises hits
}
