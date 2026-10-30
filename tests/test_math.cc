#include "doctest.h"

#include "rtheader.h"


TEST_CASE("interval contains, surrounds and clamps") {
    interval i(1, 2);
    CHECK(i.contains(1));
    CHECK_FALSE(i.surrounds(1));
    CHECK(i.surrounds(1.5));
    CHECK(i.clamp(5) == 2);
    CHECK(i.clamp(-5) == 1);
    CHECK_FALSE(interval::empty.contains(0));
    CHECK(interval::universe.contains(1e300));
}

TEST_CASE("vec3 dot, cross and unit_vector") {
    vec3 x(1,0,0), y(0,1,0);
    CHECK(dot(x, y) == 0);
    CHECK(cross(x, y)[2] == 1);
    CHECK(unit_vector(vec3(3,4,0)).length() == doctest::Approx(1.0));
}

TEST_CASE("rng is reproducible for a given seed") {
    seed_random(42);
    double a = random_double(), b = random_double();
    seed_random(42);
    CHECK(random_double() == a);
    CHECK(random_double() == b);
    CHECK(a != b);
}

TEST_CASE("random_double stays in [0,1) and averages to one half") {
    seed_random(7);
    double sum = 0;
    const int n = 100000;
    for (int k = 0; k < n; k++) {
        double r = random_double();
        REQUIRE(r >= 0.0);
        REQUIRE(r < 1.0);
        sum += r;
    }
    CHECK(sum / n == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("to_rgb8 applies gamma 2 and clamps") {
    CHECK(to_rgb8(color(0,0,0)).r == 0);
    CHECK(to_rgb8(color(1,1,1)).g == 255);
    CHECK(to_rgb8(color(4,4,4)).b == 255);
    CHECK(to_rgb8(color(0.25,0.25,0.25)).r == 128);  // sqrt(0.25) = 0.5
}
