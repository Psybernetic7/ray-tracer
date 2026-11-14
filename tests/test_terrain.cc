#include "doctest.h"

#include "rtheader.h"
#include "geometry/bvh.h"
#include "geometry/heightfield.h"
#include "geometry/hittable_list.h"
#include "geometry/sphere.h"
#include "geometry/triangle.h"
#include "lighting/sun.h"
#include "render/material.h"
#include "terrain/noise.h"
#include "terrain/terrain.h"
#include "terrain/terrain_material.h"


TEST_CASE("gradient noise is seeded, smooth and bounded") {
    // Zero at every lattice point is a property of gradient noise: the gradients are anchored
    // there, so the dot products all vanish.
    CHECK(gradient_noise(3, -7, 42) == doctest::Approx(0.0));

    double min_value = infinity, max_value = -infinity;
    for (int k = 0; k < 20000; k++) {
        double x = -40 + 80.0 * (k % 137) / 137.0;
        double y = -40 + 80.0 * (k % 211) / 211.0;
        double n = gradient_noise(x * 0.37, y * 0.41, 7);
        REQUIRE(n >= -1.001);
        REQUIRE(n <= 1.001);
        min_value = std::fmin(min_value, n);
        max_value = std::fmax(max_value, n);
    }
    CHECK(min_value < -0.4);  // The field really does swing both ways
    CHECK(max_value > 0.4);

    // Same input, same output; a different seed is a different field.
    CHECK(gradient_noise(1.25, 2.5, 1) == gradient_noise(1.25, 2.5, 1));
    CHECK(gradient_noise(1.25, 2.5, 1) != gradient_noise(1.25, 2.5, 2));

    // Smoothness: a small step in the input makes a small step in the output.
    double a = gradient_noise(1.25, 2.5, 3);
    double b = gradient_noise(1.2501, 2.5, 3);
    CHECK(std::fabs(a - b) < 0.01);
}

TEST_CASE("fbm and ridged stay in range and add detail per octave") {
    fbm_params p;
    p.seed = 11;

    for (int k = 0; k < 2000; k++) {
        double x = 0.013 * k, y = 0.007 * k;
        REQUIRE(std::fabs(fbm(x, y, p)) <= 1.001);
        double r = ridged(x, y, p);
        REQUIRE(r >= 0.0);
        REQUIRE(r <= 1.001);
    }

    // One octave of fBm is just the noise itself.
    fbm_params one = p;
    one.octaves = 1;
    CHECK(fbm(0.3, 0.8, one) == doctest::Approx(gradient_noise(0.3, 0.8, mix_seed(p.seed, 0))));

    // Adding octaves changes the field (there is detail that one octave did not have).
    fbm_params many = p;
    many.octaves = 6;
    CHECK(fbm(0.3, 0.8, many) != doctest::Approx(fbm(0.3, 0.8, one)));
}

TEST_CASE("domain warp moves the sample point, and not at all when off") {
    fbm_params p;
    double x = 0.4, y = 0.6;

    warp_point(x, y, 0.0, p);
    CHECK(x == 0.4);
    CHECK(y == 0.6);

    warp_point(x, y, 0.5, p);
    CHECK(x != 0.4);
    CHECK(y != 0.6);
    CHECK(std::fabs(x - 0.4) <= 0.5);  // The displacement is bounded by the strength
    CHECK(std::fabs(y - 0.6) <= 0.5);
}

TEST_CASE("terrain height is normalized and falls to zero at the edges") {
    terrain_params p;
    p.seed = 99;

    double highest = 0;
    for (int j = 0; j <= 40; j++) {
        for (int i = 0; i <= 40; i++) {
            double h = terrain_height(i / 40.0, j / 40.0, p);
            REQUIRE(h >= 0.0);
            REQUIRE(h <= 1.0);
            highest = std::fmax(highest, h);
        }
    }
    CHECK(highest > 0.2);  // There is a mountain in there somewhere

    // The island falloff pins the border to the base height, so the mesh can meet a plain.
    CHECK(terrain_height(0.0, 0.5, p) == doctest::Approx(0.0));
    CHECK(terrain_height(1.0, 0.5, p) == doctest::Approx(0.0));
    CHECK(terrain_height(0.5, 0.0, p) == doctest::Approx(0.0));
    CHECK(terrain_height(0.5, 1.0, p) == doctest::Approx(0.0));

    // Same seed, same terrain.
    CHECK(terrain_height(0.31, 0.62, p) == terrain_height(0.31, 0.62, p));
    terrain_params other = p;
    other.seed = 100;
    CHECK(terrain_height(0.31, 0.62, p) != terrain_height(0.31, 0.62, other));
}

TEST_CASE("triangle intersection, barycentric coordinates and misses") {
    auto mat = make_shared<lambertian>(color(0.5, 0.5, 0.5));
    // A triangle in the z = 0 plane, with the right angle at the origin.
    triangle tri(point3(0, 0, 0), point3(1, 0, 0), point3(0, 1, 0), mat);
    hit_record rec;

    // Straight at the middle of the triangle.
    REQUIRE(tri.hit(ray(point3(0.25, 0.25, -2), vec3(0, 0, 1)), interval(0.001, infinity), rec));
    CHECK(rec.t == doctest::Approx(2));
    CHECK(rec.p.x() == doctest::Approx(0.25));
    CHECK(rec.normal.z() == doctest::Approx(-1));  // Faces back toward the ray
    // The winding (right angle at the origin, then +x, then +y) puts the outward normal along
    // +z, so a ray travelling +z arrives on the back face.
    CHECK_FALSE(rec.front_face);
    CHECK(rec.mat == mat.get());

    // Outside the hypotenuse, inside the triangle's bounding box.
    CHECK_FALSE(tri.hit(ray(point3(0.8, 0.8, -2), vec3(0, 0, 1)), interval(0.001, infinity), rec));
    // Behind the triangle.
    CHECK_FALSE(tri.hit(ray(point3(0.25, 0.25, 2), vec3(0, 0, 1)), interval(0.001, infinity), rec));
    // Parallel to the triangle's plane.
    CHECK_FALSE(tri.hit(ray(point3(0.25, 0.25, 0), vec3(1, 0, 0)), interval(0.001, infinity), rec));
    // Beyond the end of the ray's interval.
    CHECK_FALSE(tri.hit(ray(point3(0.25, 0.25, -2), vec3(0, 0, 1)), interval(0.001, 1), rec));
}

TEST_CASE("triangle interpolates vertex normals and surface coordinates") {
    auto mat = make_shared<lambertian>(color(1, 1, 1));
    vertex a{point3(0, 0, 0), vec3(0, 0, -1), 0, 0};
    vertex b{point3(1, 0, 0), vec3(1, 0, 0), 1, 0};
    vertex c{point3(0, 1, 0), vec3(0, 0, -1), 0, 1};
    triangle tri(a, b, c, mat);
    hit_record rec;

    // Right on top of vertex b, whose normal points along +x.
    REQUIRE(tri.hit(ray(point3(0.999, 0.0005, -1), vec3(0, 0, 1)), interval(0.001, infinity), rec));
    CHECK(rec.normal.x() == doctest::Approx(1).epsilon(0.01));
    CHECK(rec.u == doctest::Approx(1).epsilon(0.01));
    CHECK(rec.v == doctest::Approx(0).epsilon(0.01));

    // The centroid: u and v average the three vertices.
    REQUIRE(tri.hit(ray(point3(1.0/3, 1.0/3, -1), vec3(0, 0, 1)), interval(0.001, infinity), rec));
    CHECK(rec.u == doctest::Approx(1.0/3));
    CHECK(rec.v == doctest::Approx(1.0/3));
    CHECK(unit_vector(rec.normal).length() == doctest::Approx(1));
}

TEST_CASE("an axis-aligned triangle is still hit by a ray in its own plane's box") {
    // A flat triangle has a zero-thickness bounding box, which the slab test pads so it
    // doesn't compute inf * 0 for a ray travelling inside that plane.
    auto mat = make_shared<lambertian>(color(1, 1, 1));
    triangle flat(point3(-1, 0, -1), point3(1, 0, -1), point3(0, 0, 1), mat);

    CHECK(flat.bounding_box().y.size() > 0);
    CHECK(flat.bounding_box().hit(ray(point3(0, 0, -5), vec3(0, 0, 1)), interval(0, infinity)));

    hit_record rec;
    REQUIRE(flat.hit(ray(point3(0, 1, 0), vec3(0, -1, 0)), interval(0.001, infinity), rec));
    CHECK(rec.t == doctest::Approx(1));
}

TEST_CASE("heightfield geometry: positions, normals and mesh size") {
    heightfield field(5, 5, 4.0, 4.0);
    CHECK(field.cell_x() == doctest::Approx(1.0));

    // A flat field has straight-up normals everywhere, edges included.
    CHECK(field.vertex_normal(0, 0).y() == doctest::Approx(1));
    CHECK(field.vertex_normal(2, 2).y() == doctest::Approx(1));

    CHECK(field.vertex_position(0, 0).x() == doctest::Approx(-2.0));
    CHECK(field.vertex_position(4, 4).z() == doctest::Approx(2.0));
    CHECK(field.u_at(0) == doctest::Approx(0));
    CHECK(field.u_at(4) == doctest::Approx(1));

    // A constant slope along x: height = x, so the normal tilts into -x.
    for (int j = 0; j < field.rows(); j++)
        for (int i = 0; i < field.cols(); i++)
            field.at(i, j) = field.vertex_position(i, j).x();

    vec3 n = field.vertex_normal(2, 2);
    CHECK(n.x() == doctest::Approx(-1.0 / std::sqrt(2.0)));
    CHECK(n.y() == doctest::Approx(1.0 / std::sqrt(2.0)));
    CHECK(n.z() == doctest::Approx(0));

    CHECK(field.height_range().min == doctest::Approx(-2.0));
    CHECK(field.height_range().max == doctest::Approx(2.0));

    // Two triangles per cell, and the mesh's bounds match the field's.
    hittable_list mesh = field.to_triangles(make_shared<lambertian>(color(1, 1, 1)));
    CHECK(mesh.objects.size() == 4 * 4 * 2);
    CHECK(mesh.bounding_box().y.max == doctest::Approx(field.height_range().max));
}

TEST_CASE("a ray dropped onto a terrain mesh lands on the surface") {
    terrain_params p;
    p.seed = 5;
    heightfield field = generate_heightfield(33, 8.0, 2.0, p);
    bvh_node world(field.to_triangles(make_shared<lambertian>(color(1, 1, 1))));

    // Straight down from above, at a few interior grid vertices: the hit must be at that
    // vertex's own height, since the vertex is a corner shared by the surrounding triangles.
    for (int j = 1; j < field.rows() - 1; j += 7) {
        for (int i = 1; i < field.cols() - 1; i += 7) {
            point3 vertex_point = field.vertex_position(i, j);
            ray down(point3(vertex_point.x(), 10.0, vertex_point.z()), vec3(0, -1, 0));

            hit_record rec;
            REQUIRE(world.hit(down, interval(0.001, infinity), rec));
            CHECK(rec.p.y() == doctest::Approx(vertex_point.y()));
            CHECK(rec.normal.y() > 0);          // The surface faces the sky
            CHECK(rec.u == doctest::Approx(field.u_at(i)));
            CHECK(rec.v == doctest::Approx(field.v_at(j)));
        }
    }
}

TEST_CASE("occluded agrees with hit, and the bvh agrees with a linear list") {
    seed_random(2024);
    hittable_list list;
    for (int k = 0; k < 200; k++)
        list.add(make_shared<sphere>(vec3::random(-8, 8), random_double(0.2, 1.2),
                                     make_shared<lambertian>(color::random())));
    bvh_node bvh(list);

    int blocked = 0;
    for (int k = 0; k < 4000; k++) {
        ray r(vec3::random(-12, 12), random_unit_vector());
        interval range(0.001, infinity);

        hit_record rec;
        bool hit = bvh.hit(r, range, rec);
        REQUIRE(bvh.occluded(r, range) == hit);
        REQUIRE(list.occluded(r, range) == hit);
        if (hit) blocked++;

        // A range that stops short of the nearest hit must report nothing in the way.
        if (hit)
            REQUIRE_FALSE(bvh.occluded(r, interval(0.001, rec.t - 1e-9)));
    }
    CHECK(blocked > 500);  // The test actually exercises blocked rays
}

TEST_CASE("sun direction from elevation and azimuth") {
    // Straight up.
    vec3 noon = sun_light::direction_from_elevation_azimuth(90, 0);
    CHECK(noon.y() == doctest::Approx(1));

    // On the horizon, due east is +x and due north is -z.
    vec3 east = sun_light::direction_from_elevation_azimuth(0, 90);
    CHECK(east.x() == doctest::Approx(1));
    CHECK(east.y() == doctest::Approx(0));
    vec3 north = sun_light::direction_from_elevation_azimuth(0, 0);
    CHECK(north.z() == doctest::Approx(-1));

    // 45 degrees up, due south.
    vec3 south = sun_light::direction_from_elevation_azimuth(45, 180);
    CHECK(south.y() == doctest::Approx(1 / std::sqrt(2.0)));
    CHECK(south.z() == doctest::Approx(1 / std::sqrt(2.0)));

    CHECK(south.length() == doctest::Approx(1));
}

TEST_CASE("sun disk sampling stays inside the sun's angular radius") {
    seed_random(3);
    sun_light sun;
    sun.direction = unit_vector(vec3(1, 2, 3));
    sun.angular_radius_degrees = 2.0;
    const double cos_limit = std::cos(degrees_to_radians(sun.angular_radius_degrees));

    double closest = 1.0;
    for (int k = 0; k < 2000; k++) {
        vec3 sample = sun.sample_direction();
        REQUIRE(sample.length() == doctest::Approx(1));
        double cos_angle = dot(sample, sun.direction);
        REQUIRE(cos_angle >= cos_limit - 1e-9);
        closest = std::fmin(closest, cos_angle);
    }
    CHECK(closest < std::cos(degrees_to_radians(1.0)));  // It spreads over the whole disk
}

TEST_CASE("sky keeps its old gradient by default and brightens toward the sun") {
    sky_light sky;
    sun_light off;  // Disabled

    // The default sky is exactly the gradient the renderer used before the sun existed.
    color up = sky.radiance(vec3(0, 1, 0), off);
    CHECK(up.x() == doctest::Approx(0.5));
    CHECK(up.z() == doctest::Approx(1.0));
    color level = sky.radiance(vec3(1, 0, 0), off);
    CHECK(level.x() == doctest::Approx(0.75));

    sun_light sun;
    sun.enabled = true;
    sun.direction = vec3(0, 1, 0);
    sun.disk_in_sky = true;
    sky.glow_strength = 1.0;

    // Looking into the sun is brighter than looking away from it.
    CHECK(sky.radiance(vec3(0, 1, 0), sun).y() > sky.radiance(vec3(0, -1, 0), sun).y());
    // With the disk switched off, the sun's own intensity is gone from the sky.
    sun.disk_in_sky = false;
    CHECK(sky.radiance(vec3(0, 1, 0), sun).y() < sun.intensity.y());
}

TEST_CASE("terrain material picks albedo by slope and height") {
    terrain_material mat(interval(0, 10));
    hit_record rec;
    rec.front_face = true;

    auto albedo_at = [&](double height, const vec3& normal) {
        rec.p = point3(0, height, 0);
        rec.normal = unit_vector(normal);
        color albedo;
        REQUIRE(mat.diffuse_albedo(rec, albedo));
        return albedo;
    };

    // Low and flat: grass, so green is the strongest channel.
    color low = albedo_at(1.0, vec3(0, 1, 0));
    CHECK(low.y() > low.x());
    CHECK(low.y() > low.z());

    // Low and near-vertical: bare rock, which is greyer and brighter than grass.
    color cliff = albedo_at(1.0, vec3(1, 0.05, 0));
    CHECK(cliff.x() > low.x());
    CHECK(std::fabs(cliff.x() - cliff.z()) < 0.1);

    // High and flat: snow, bright in every channel.
    color peak = albedo_at(9.5, vec3(0, 1, 0));
    CHECK(peak.x() > 0.7);
    CHECK(peak.z() > 0.7);

    // High but too steep to hold snow: back to rock.
    color steep_peak = albedo_at(9.5, vec3(1, 0.05, 0));
    CHECK(steep_peak.x() < 0.5);
}
