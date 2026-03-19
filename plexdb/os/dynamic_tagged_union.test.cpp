#include <catch2/catch_test_macros.hpp>

import plexdb.os.dynamic_tagged_union;
import plexdb.base;

using namespace plexdb;

TEST_CASE("DynamicTaggedUnion default construction", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> u;
    REQUIRE(!u);
    REQUIRE(u.ptr == nullptr);
    REQUIRE(u.index == DynamicTaggedUnion<int, double>::invalid_index);
}

TEST_CASE("DynamicTaggedUnion value construction", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> u(42);
    REQUIRE(u);
    REQUIRE(type_matches_tag<int>(u));
    REQUIRE(get<int>(u) == 42);
}

TEST_CASE("DynamicTaggedUnion assignment from value", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> u;
    u = 3.14;
    REQUIRE(u);
    REQUIRE(type_matches_tag<double>(u));
    REQUIRE(get<double>(u) == 3.14);

    SECTION("reassign same type") {
        u = 2.71;
        REQUIRE(type_matches_tag<double>(u));
        REQUIRE(get<double>(u) == 2.71);
    }

    SECTION("reassign different type") {
        u = 7;
        REQUIRE(type_matches_tag<int>(u));
        REQUIRE(get<int>(u) == 7);
    }
}

TEST_CASE("DynamicTaggedUnion copy construction", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> src(99);
    DynamicTaggedUnion<int, double> dst(src);

    REQUIRE(dst);
    REQUIRE(get<int>(dst) == 99);
    // Independent allocation
    REQUIRE(dst.ptr != src.ptr);
}

TEST_CASE("DynamicTaggedUnion copy construction from empty", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> src;
    DynamicTaggedUnion<int, double> dst(src);
    REQUIRE(!dst);
    REQUIRE(dst.ptr == nullptr);
}

TEST_CASE("DynamicTaggedUnion move construction", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> src(55);
    void* original_ptr = src.ptr;

    DynamicTaggedUnion<int, double> dst(move(src));

    REQUIRE(dst);
    REQUIRE(dst.ptr == original_ptr);
    REQUIRE(get<int>(dst) == 55);
    REQUIRE(!src);
    REQUIRE(src.ptr == nullptr);
}

TEST_CASE("DynamicTaggedUnion copy assignment", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> a(10);
    DynamicTaggedUnion<int, double> b;

    SECTION("empty <- valued") {
        b = a;
        REQUIRE(get<int>(b) == 10);
        REQUIRE(b.ptr != a.ptr);
    }

    SECTION("valued <- empty") {
        DynamicTaggedUnion<int, double> empty;
        a = empty;
        REQUIRE(!a);
    }

    SECTION("valued <- valued same type") {
        DynamicTaggedUnion<int, double> c(20);
        a = c;
        REQUIRE(get<int>(a) == 20);
    }

    SECTION("valued <- valued different type") {
        DynamicTaggedUnion<int, double> c(1.5);
        a = c;
        REQUIRE(type_matches_tag<double>(a));
        REQUIRE(get<double>(a) == 1.5);
    }
}

TEST_CASE("DynamicTaggedUnion move assignment", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> src(77);
    void* original_ptr = src.ptr;

    DynamicTaggedUnion<int, double> dst;
    dst = move(src);

    REQUIRE(dst.ptr == original_ptr);
    REQUIRE(get<int>(dst) == 77);
    REQUIRE(!src);
}

TEST_CASE("DynamicTaggedUnion self-assignment", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> u(5);
    u = u;
    REQUIRE(get<int>(u) == 5);
}

TEST_CASE("DynamicTaggedUnion visit", "[dynamic_tagged_union]") {
    DynamicTaggedUnion<int, double> u(3.14);

    double result = visit(u, [](auto& v) -> double {
        return static_cast<double>(v);
    });
    REQUIRE(result == 3.14);
}
