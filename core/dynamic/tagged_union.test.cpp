#include <catch2/catch_test_macros.hpp>

import plexdb.dynamic.tagged_union;
import plexdb.base;

using namespace plexdb;

TEST_CASE("AutoTaggedUnion default construction", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> u;
    REQUIRE(!u);
    REQUIRE(u.ptr == nullptr);
    REQUIRE(u.index == AutoTaggedUnion<int, double>::invalid_index);
}

TEST_CASE("AutoTaggedUnion value construction", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> u(42);
    REQUIRE(u);
    REQUIRE(type_matches_tag<int>(u));
    REQUIRE(get<int>(u) == 42);
}

TEST_CASE("AutoTaggedUnion assignment from value", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> u;
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

TEST_CASE("AutoTaggedUnion copy construction", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> src(99);
    AutoTaggedUnion<int, double> dst(src);

    REQUIRE(dst);
    REQUIRE(get<int>(dst) == 99);
    // Independent allocation
    REQUIRE(dst.ptr != src.ptr);
}

TEST_CASE("AutoTaggedUnion copy construction from empty", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> src;
    AutoTaggedUnion<int, double> dst(src);
    REQUIRE(!dst);
    REQUIRE(dst.ptr == nullptr);
}

TEST_CASE("AutoTaggedUnion move construction", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> src(55);
    void* original_ptr = src.ptr;

    AutoTaggedUnion<int, double> dst(move(src));

    REQUIRE(dst);
    REQUIRE(dst.ptr == original_ptr);
    REQUIRE(get<int>(dst) == 55);
    REQUIRE(!src);
    REQUIRE(src.ptr == nullptr);
}

TEST_CASE("AutoTaggedUnion copy assignment", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> a(10);
    AutoTaggedUnion<int, double> b;

    SECTION("empty <- valued") {
        b = a;
        REQUIRE(get<int>(b) == 10);
        REQUIRE(b.ptr != a.ptr);
    }

    SECTION("valued <- empty") {
        AutoTaggedUnion<int, double> empty;
        a = empty;
        REQUIRE(!a);
    }

    SECTION("valued <- valued same type") {
        AutoTaggedUnion<int, double> c(20);
        a = c;
        REQUIRE(get<int>(a) == 20);
    }

    SECTION("valued <- valued different type") {
        AutoTaggedUnion<int, double> c(1.5);
        a = c;
        REQUIRE(type_matches_tag<double>(a));
        REQUIRE(get<double>(a) == 1.5);
    }
}

TEST_CASE("AutoTaggedUnion move assignment", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> src(77);
    void* original_ptr = src.ptr;

    AutoTaggedUnion<int, double> dst;
    dst = move(src);

    REQUIRE(dst.ptr == original_ptr);
    REQUIRE(get<int>(dst) == 77);
    REQUIRE(!src);
}

TEST_CASE("AutoTaggedUnion self-assignment", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> u(5);
    auto& u_ref = u;
    u = u_ref;
    REQUIRE(get<int>(u) == 5);
}

TEST_CASE("AutoTaggedUnion visit", "[dynamic.tagged_union]") {
    AutoTaggedUnion<int, double> u(3.14);

    double result = visit(u, [](auto& v) -> double {
        return static_cast<double>(v);
    });
    REQUIRE(result == 3.14);
}
