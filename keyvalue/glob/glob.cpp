module keyvalue.glob;

import plexdb.base;

using namespace plexdb;

namespace keyvalue::glob {
    bool match(String8 key, String8 pattern) {
        if (pattern.length == 1 && pattern[0] == '*') {
            return true;
        }
        U64 pi = 0, ki = 0;
        U64 star_pi = U64(-1), star_ki = 0;
        while (ki < key.length) {
            if (pi < pattern.length && (pattern[pi] == '?' || pattern[pi] == key[ki])) {
                pi++;
                ki++;
            } else if (pi < pattern.length && pattern[pi] == '*') {
                star_pi = pi++;
                star_ki = ki;
            } else if (star_pi != U64(-1)) {
                pi = star_pi + 1;
                ki = ++star_ki;
            } else {
                return false;
            }
        }
        while (pi < pattern.length && pattern[pi] == '*') {
            pi++;
        }
        return pi == pattern.length;
    }
}
