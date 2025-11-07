module;
#include <stdint.h>
#include <float.h>

export module plexdb.common;

export import :std;

export namespace plexdb {
    using U8  = uint8_t;
    using U16 = uint16_t;
    using U32 = uint32_t;
    using U64 = uint64_t;
    using S8  = int8_t;
    using S16 = int16_t;
    using S32 = int32_t;
    using S64 = int64_t;
    using B8  = S8;
    using B16 = S16;
    using B32 = S32;
    using B64 = S64;
    using F32 = float;
    using F64 = double;

    constexpr U8  MAX_U8   = UINT8_MAX;
    constexpr U16 MAX_U16  = UINT16_MAX;
    constexpr U32 MAX_U32  = UINT32_MAX;
    constexpr U64 MAX_U64  = UINT64_MAX;

    constexpr S8  MAX_S8   = INT8_MAX;
    constexpr S16 MAX_S16  = INT16_MAX;
    constexpr S32 MAX_S32  = INT32_MAX;
    constexpr S64 MAX_S64  = INT64_MAX;

    constexpr S8  MIN_S8   = INT8_MIN;
    constexpr S16 MIN_S16  = INT16_MIN;
    constexpr S32 MIN_S32  = INT32_MIN;
    constexpr S64 MIN_S64  = INT64_MIN;

    constexpr F32 MAX_F32  = FLT_MAX;
    constexpr F32 MIN_F32  = FLT_MIN;
    constexpr F64 MAX_F64  = DBL_MAX;
    constexpr F64 MIN_F64  = DBL_MIN;

    constexpr B8  MAX_B8   = INT8_MAX;
    constexpr B16 MAX_B16  = INT16_MAX;
    constexpr B32 MAX_B32  = INT32_MAX;
    constexpr B64 MAX_B64  = INT64_MAX;
}