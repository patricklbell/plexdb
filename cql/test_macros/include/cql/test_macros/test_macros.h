#pragma once

#include <coroutine>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.coroutine;
import plexdb.pager.test_helpers;
import cql.test_helpers;

#define PLEXDB_CQL_CAT2(a, b) a##b
#define PLEXDB_CQL_CAT(a, b) PLEXDB_CQL_CAT2(a, b)

// Wraps a Catch2 test as a coroutine that runs against a freshly-opened
// ServerFixture (named `fixture`). open_server/close_server bracket the body;
// drive_test_pager pumps both the setup and the user coroutine on the calling
// thread, the same way PAGER_TEST_CASE drives pager I/O.
#define CQL_NATIVE_TEST_CASE(name, tags) \
    static plexdb::coroutine::Task<void> PLEXDB_CQL_CAT(cql_native_test_, __LINE__)(cql::test::ServerFixture& fixture); \
    TEST_CASE(name, tags) { \
        cql::test::ServerFixture fixture{}; \
        drive_test_pager(cql::test::open_server(fixture)); \
        drive_test_pager(PLEXDB_CQL_CAT(cql_native_test_, __LINE__)(fixture)); \
        cql::test::close_server(fixture); \
    } \
    static plexdb::coroutine::Task<void> PLEXDB_CQL_CAT(cql_native_test_, __LINE__)(cql::test::ServerFixture& fixture)

namespace Catch {
    template<>
    struct StringMaker<plexdb::String8> {
        static std::string convert(plexdb::String8 const& value) {
            return value.data ? std::string(value.data, value.length) : std::string{};
        }
    };

    template<>
    struct StringMaker<plexdb::AutoString8> {
        static std::string convert(plexdb::AutoString8 const& value) {
            return value.c_str ? std::string(value.c_str, value.length) : std::string{};
        }
    };

    template<typename T, typename Size>
    struct StringMaker<plexdb::DynamicArray<T, Size>> {
        static std::string convert(plexdb::DynamicArray<T, Size> const& value) {
            std::string out = "[";
            for (Size i = 0; i < value.length; i++) {
                if (i > 0) out += ", ";
                out += StringMaker<T>::convert(value.ptr[i]);
            }
            out += "]";
            return out;
        }
    };

    template<>
    struct StringMaker<cql::test::Frame> {
        static std::string convert(cql::test::Frame const& f) {
            std::string out = "Frame{v=0x";
            const char* hex = "0123456789abcdef";
            out += hex[(f.version >> 4) & 0xF];
            out += hex[ f.version       & 0xF];
            out += ", op=0x";
            out += hex[(f.opcode  >> 4) & 0xF];
            out += hex[ f.opcode        & 0xF];
            out += ", stream=";
            out += std::to_string(int(f.stream));
            out += ", body_len=";
            out += std::to_string(f.body_len);
            out += "}";
            return out;
        }
    };
}
