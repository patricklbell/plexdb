module;
#include <charconv>

module cql.engine.literal_widen;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;
import cql.column_types;

using namespace plexdb;
using namespace cql::column_types;

namespace cql {
    VarInt varint_from_s64(S64 v) {
        VarInt result{};
        if (v == 0) {
            return result;
        }
        result.negative = v < 0;
        U64 mag         = result.negative ? (0ull - U64(v)) : U64(v);
        U8  bytes[8];
        for (int i = 7; i >= 0; i--) {
            bytes[i] = U8(mag & 0xFF);
            mag >>= 8;
        }
        U64 start = 0;
        while (start < 7 && bytes[start] == 0) {
            start++;
        }
        resize(result.magnitude, 8 - start);
        os::memory_copy(result.magnitude.ptr, bytes + start, 8 - start);
        return result;
    }

    Decimal decimal_from_s64(S64 v) {
        return Decimal{0, varint_from_s64(v)};
    }

    S64 s64_from_varint_truncating(const VarInt& v) {
        U64 n    = v.magnitude.length;
        U64 take = n < 8 ? n : 8;
        U64 mag  = 0;
        for (U64 i = 0; i < take; i++) {
            mag = (mag << 8) | U64(v.magnitude.ptr[n - take + i]);
        }
        return v.negative ? S64(0ull - mag) : S64(mag);
    }

    Optional<S64> try_s64_from_varint(const VarInt& v) {
        U64 start = 0;
        while (start < v.magnitude.length && v.magnitude.ptr[start] == 0) {
            start++;
        }
        U64 len = v.magnitude.length - start;
        if (len > 8) {
            return {};
        }
        if (len == 8) {
            if (v.magnitude.ptr[start] >= 0x80 && !(v.negative && v.magnitude.ptr[start] == 0x80)) {
                return {};
            }
            if (v.magnitude.ptr[start] == 0x80) {
                for (U64 i = 1; i < 8; i++) {
                    if (v.magnitude.ptr[start + i] != 0) {
                        return {};
                    }
                }
            }
        }
        U64 mag = 0;
        for (U64 i = 0; i < len; i++) {
            mag = (mag << 8) | U64(v.magnitude.ptr[start + i]);
        }
        return v.negative ? S64(0ull - mag) : S64(mag);
    }

    // Accumulates decimal digit characters into a big-endian magnitude via repeated
    // multiply-by-10-add-digit, tracking scale from digits seen after '.' and any exponent.
    static Decimal decimal_from_chars(const char* s, const char* end) {
        const char* p   = s;
        bool        neg = false;
        if (p < end && (*p == '-' || *p == '+')) {
            neg = (*p == '-');
            p++;
        }
        DynamicArray<U8> digits;
        S32              scale    = 0;
        bool             seen_dot = false;
        for (; p < end; p++) {
            char c = *p;
            if (c == '.') {
                seen_dot = true;
                continue;
            }
            if (c == 'e' || c == 'E') {
                p++;
                break;
            }
            if (c < '0' || c > '9') {
                continue;
            }
            U32 carry = U32(c - '0');
            for (U64 i = digits.length; i-- > 0;) {
                U32 val       = U32(digits.ptr[i]) * 10 + carry;
                digits.ptr[i] = U8(val & 0xFF);
                carry         = val >> 8;
            }
            while (carry > 0) {
                DynamicArray<U8> grown;
                resize(grown, digits.length + 1);
                grown.ptr[0] = U8(carry & 0xFF);
                if (digits.length > 0) {
                    os::memory_copy(grown.ptr + 1, digits.ptr, digits.length);
                }
                carry  = carry >> 8;
                digits = move(grown);
            }
            if (seen_dot) {
                scale++;
            }
        }
        S32 exp = 0;
        if (p < end) {
            bool exp_neg = false;
            if (p < end && (*p == '-' || *p == '+')) {
                exp_neg = (*p == '-');
                p++;
            }
            S32 e = 0;
            for (; p < end; p++) {
                e = e * 10 + (*p - '0');
            }
            exp = exp_neg ? -e : e;
        }
        scale -= exp;
        U64 start = 0;
        while (start < digits.length && digits.ptr[start] == 0) {
            start++;
        }
        VarInt unscaled{};
        unscaled.negative = neg && (start < digits.length);
        resize(unscaled.magnitude, digits.length - start);
        if (digits.length - start > 0) {
            os::memory_copy(unscaled.magnitude.ptr, digits.ptr + start, digits.length - start);
        }
        return Decimal{scale, move(unscaled)};
    }

    Decimal decimal_from_f64(F64 v) {
        char buf[64];
        auto res = std::to_chars(buf, buf + sizeof(buf), v);
        return decimal_from_chars(buf, res.ptr);
    }

    static bool parse_ipv4(String8 s, U8 out[4]) {
        U64 i = 0;
        for (int part = 0; part < 4; part++) {
            if (part > 0) {
                if (i >= s.length || s.data[i] != '.') {
                    return false;
                }
                i++;
            }
            if (i >= s.length || s.data[i] < '0' || s.data[i] > '9') {
                return false;
            }
            U32 val     = 0;
            U32 ndigits = 0;
            while (i < s.length && s.data[i] >= '0' && s.data[i] <= '9') {
                val = val * 10 + U32(s.data[i] - '0');
                i++;
                ndigits++;
                if (ndigits > 3 || val > 255) {
                    return false;
                }
            }
            out[part] = U8(val);
        }
        return i == s.length;
    }

    // Standard colon-hex form with at most one "::" compression run. Does not support the
    // embedded-IPv4 suffix form (e.g. "::ffff:192.168.1.1").
    static bool parse_ipv6(String8 s, U8 out[16]) {
        U16 groups[8]   = {};
        U32 n_groups    = 0;
        S32 compress_at = -1;
        U64 i           = 0;

        if (s.length >= 2 && s.data[0] == ':' && s.data[1] == ':') {
            compress_at = 0;
            i           = 2;
        }

        while (i < s.length) {
            if (n_groups >= 8) {
                return false;
            }
            U32 val     = 0;
            U32 ndigits = 0;
            while (i < s.length && ndigits < 4) {
                char c = s.data[i];
                U32  d;
                if (c >= '0' && c <= '9') {
                    d = U32(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    d = U32(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    d = U32(c - 'A' + 10);
                } else {
                    break;
                }
                val = (val << 4) | d;
                i++;
                ndigits++;
            }
            if (ndigits == 0) {
                return false;
            }
            groups[n_groups++] = U16(val);

            if (i == s.length) {
                break;
            }
            if (s.data[i] != ':') {
                return false;
            }
            i++;
            if (i < s.length && s.data[i] == ':') {
                if (compress_at >= 0) {
                    return false; // only one "::" allowed
                }
                compress_at = S32(n_groups);
                i++;
                if (i == s.length) {
                    break;
                }
            }
        }

        U32 total_written = n_groups;
        if (compress_at < 0) {
            if (total_written != 8) {
                return false;
            }
        } else if (total_written > 8) {
            return false;
        }

        U32 zeros_to_insert = (compress_at >= 0) ? (8 - total_written) : 0;
        U32 out_idx         = 0;
        for (U32 g = 0; g < n_groups; g++) {
            if (S32(g) == compress_at) {
                for (U32 z = 0; z < zeros_to_insert; z++) {
                    out[out_idx * 2]     = 0;
                    out[out_idx * 2 + 1] = 0;
                    out_idx++;
                }
            }
            out[out_idx * 2]     = U8(groups[g] >> 8);
            out[out_idx * 2 + 1] = U8(groups[g] & 0xFF);
            out_idx++;
        }
        if (compress_at == S32(n_groups)) {
            for (U32 z = 0; z < zeros_to_insert; z++) {
                out[out_idx * 2]     = 0;
                out[out_idx * 2 + 1] = 0;
                out_idx++;
            }
        }
        return out_idx == 8;
    }

    Optional<Inet> inet_from_string(String8 s) {
        Inet v4{};
        if (parse_ipv4(s, &v4.v4[0])) {
            v4.is_v6 = false;
            return v4;
        }
        Inet v6{};
        v6.is_v6 = true;
        if (parse_ipv6(s, &v6.v6[0])) {
            return v6;
        }
        return {};
    }
}
