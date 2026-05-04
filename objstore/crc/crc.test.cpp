#include <catch2/catch_test_macros.hpp>

import plexdb.base.types;
import objstore.crc;

using namespace plexdb;

TEST_CASE("crc32 matches known test vectors", "[objstore.crc]") {
    using namespace objstore::crc;

    U8 test[] = "123456789";
    CHECK(crc32(test, 9) == 0xCBF43926);

    U8 empty[] = "";
    CHECK(crc32(empty, 0) == 0x00000000);
}

TEST_CASE("crc32 CQL v5 initial value matches seed bytes", "[objstore.crc]") {
    using namespace objstore::crc;

    U8 seed[] = {0xfa, 0x2d, 0x55, 0xca};
    U32 seed_crc = crc32(seed, 4);
    CHECK(seed_crc == 0x44777ED3u);
    CHECK(CRC32_CQL_V5_INIT == (seed_crc ^ 0xFFFFFFFFu));

    // crc32(seed + data) == crc32(data, CQL_V5_INIT): chaining property
    U8 data[] = "123456789";
    U8 seed_and_data[] = {0xfa, 0x2d, 0x55, 0xca, '1','2','3','4','5','6','7','8','9'};
    CHECK(crc32(data, 9, CRC32_CQL_V5_INIT) == crc32(seed_and_data, 13));

    // Wire regression: confirmed payload from cql_py_example => stored CRC 0x1F5FF572
    U8 wire_payload[] = {
        0x05, 0x00, 0x00, 0x02, 0x0b, 0x00, 0x00, 0x00,
        0x31, 0x00, 0x03, 0x00, 0x0f, 0x54, 0x4f, 0x50,
        0x4f, 0x4c, 0x4f, 0x47, 0x59, 0x5f, 0x43, 0x48,
        0x41, 0x4e, 0x47, 0x45, 0x00, 0x0d, 0x53, 0x54,
        0x41, 0x54, 0x55, 0x53, 0x5f, 0x43, 0x48, 0x41,
        0x4e, 0x47, 0x45, 0x00, 0x0d, 0x53, 0x43, 0x48,
        0x45, 0x4d, 0x41, 0x5f, 0x43, 0x48, 0x41, 0x4e,
        0x47, 0x45
    };
    CHECK(crc32(wire_payload, 58, CRC32_CQL_V5_INIT) == 0x1F5FF572u);
}
