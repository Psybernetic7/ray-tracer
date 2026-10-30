#include "doctest.h"

#include "rtheader.h"
#include "geometry/bvh.h"
#include "geometry/hittable_list.h"
#include "geometry/sphere.h"
#include "render/camera.h"
#include "render/framebuffer.h"
#include "render/material.h"
#include "render/renderer.h"


static hittable_list small_scene() {
    hittable_list world;
    world.add(make_shared<sphere>(point3(0,-100.5,-1), 100, make_shared<lambertian>(color(0.8,0.8,0.0))));
    world.add(make_shared<sphere>(point3(0,0,-1.2), 0.5, make_shared<lambertian>(color(0.1,0.2,0.5))));
    world.add(make_shared<sphere>(point3(-1,0,-1), 0.5, make_shared<dielectric>(1.5)));
    world.add(make_shared<sphere>(point3(1,0,-1), 0.5, make_shared<metal>(color(0.8,0.6,0.2), 0.3)));
    return world;
}

static camera small_camera() {
    camera cam;
    cam.aspect_ratio = 16.0 / 9.0;
    cam.image_width  = 80;
    cam.max_depth    = 10;
    cam.initialize();
    return cam;
}

static bool same_pixels(const framebuffer& a, const framebuffer& b) {
    for (int j = 0; j < a.height(); j++)
        for (int i = 0; i < a.width(); i++)
            for (int c = 0; c < 3; c++)
                if (a.pixel(i, j)[c] != b.pixel(i, j)[c])
                    return false;
    return true;
}

TEST_CASE("render is identical regardless of thread count") {
    auto world = bvh_node(small_scene());
    auto cam = small_camera();

    framebuffer single(cam.image_width, cam.height());
    framebuffer multi(cam.image_width, cam.height());

    renderer r;
    r.tile_size = 8;  // Many small tiles so threads interleave
    r.seed = 99;

    r.thread_count = 1;
    r.render_pass(cam, world, single, 4);
    r.thread_count = 8;
    r.render_pass(cam, world, multi, 4);

    CHECK(same_pixels(single, multi));
}

TEST_CASE("progressive passes accumulate samples") {
    auto world = bvh_node(small_scene());
    auto cam = small_camera();
    framebuffer fb(cam.image_width, cam.height());

    renderer r;
    r.render_pass(cam, world, fb, 1);
    r.render_pass(cam, world, fb, 2);
    CHECK(fb.samples_per_pixel() == 3);

    // The sky at the top of the image must be bright and bluish, not black.
    color top = fb.pixel(cam.image_width / 2, 0);
    CHECK(top.z() > 0.5);
    CHECK(top.z() >= top.x());

    fb.clear();
    CHECK(fb.samples_per_pixel() == 0);
}
