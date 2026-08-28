module cql.column_types.twos_complement;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;

import cql.engine.io.types;

using namespace plexdb;

namespace cql::twos_complement {
    static void magnitude_bounds(const U8* magnitude, U64 magnitude_len, U64& start) {
        start = 0;
        while (start < magnitude_len && magnitude[start] == 0) {
            start++;
        }
    }

    // true iff the unsigned value of mag[0..len) (big-endian) is <= 2^(8*len-1) — the largest
    // magnitude whose negation still fits in a `len`-byte two's complement value.
    static bool fits_in_negative_length(const U8* mag, U64 len) {
        if (mag[0] < 0x80) {
            return true;
        }
        if (mag[0] != 0x80) {
            return false;
        }
        for (U64 i = 1; i < len; i++) {
            if (mag[i] != 0) {
                return false;
            }
        }
        return true;
    }

    U64 length(bool negative, const U8* magnitude, U64 magnitude_len) {
        U64 start;
        magnitude_bounds(magnitude, magnitude_len, start);
        U64 len = magnitude_len - start;
        if (len == 0) {
            return 1;
        }
        if (!negative) {
            return len + ((magnitude[start] & 0x80) ? 1 : 0);
        }
        return fits_in_negative_length(magnitude + start, len) ? len : len + 1;
    }

    void write(io::Writer w, bool negative, const U8* magnitude, U64 magnitude_len) {
        U64 start;
        magnitude_bounds(magnitude, magnitude_len, start);
        U64 len = magnitude_len - start;
        if (len == 0) {
            U8 zero = 0;
            w(&zero, 1);
            return;
        }
        U64              total = length(negative, magnitude, magnitude_len);
        U64              pad   = total - len;
        DynamicArray<U8> out;
        resize(out, total);
        for (U64 i = 0; i < pad; i++) {
            out.ptr[i] = 0;
        }
        os::memory_copy(out.ptr + pad, magnitude + start, len);
        if (negative) {
            for (U64 i = 0; i < total; i++) {
                out.ptr[i] = U8(~out.ptr[i]);
            }
            for (S64 i = S64(total) - 1; i >= 0; i--) {
                U16 s      = U16(out.ptr[i]) + 1;
                out.ptr[i] = U8(s & 0xFF);
                if (s <= 0xFF) {
                    break;
                }
            }
        }
        w(out.ptr, total);
    }

    Decoded read(const U8* data, U64 len) {
        Decoded result{};
        if (len == 0) {
            return result;
        }
        bool negative = (data[0] & 0x80) != 0;
        if (!negative) {
            U64 start = 0;
            while (start < len && data[start] == 0) {
                start++;
            }
            resize(result.magnitude, len - start);
            if (len - start > 0) {
                os::memory_copy(result.magnitude.ptr, data + start, len - start);
            }
            return result;
        }
        DynamicArray<U8> mag;
        resize(mag, len);
        for (U64 i = 0; i < len; i++) {
            mag.ptr[i] = U8(~data[i]);
        }
        for (S64 i = S64(len) - 1; i >= 0; i--) {
            U16 s      = U16(mag.ptr[i]) + 1;
            mag.ptr[i] = U8(s & 0xFF);
            if (s <= 0xFF) {
                break;
            }
        }
        U64 start = 0;
        while (start < len && mag.ptr[start] == 0) {
            start++;
        }
        result.negative = true;
        resize(result.magnitude, len - start);
        if (len - start > 0) {
            os::memory_copy(result.magnitude.ptr, mag.ptr + start, len - start);
        }
        return result;
    }
}
