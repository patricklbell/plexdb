export module objstore.engine.dtype;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;

using namespace plexdb;

export namespace objstore {
    enum class DType : U8 {
        text,
        uuid,
        timestamp,
        smallint,
        int_,
        bigint,
        counter,
        boolean,
        float_,
        double_,
    };

    namespace dtype {
        using WriteValue = TaggedUnion<AutoString8, S64, bool, F64>;
        using ReadValue = TaggedUnion<AutoString8, S64, S32, S16, U8, F64, F32>;
    
        inline constexpr U64 hash(const WriteValue& value) {
            return visit(value, [](auto& v) -> U64 {
                using T = Decay<decltype(v)>;
    
                if constexpr (SameAs<T, AutoString8>) {
                    return hash(v);
                }
                if constexpr (Either<T, S64, bool, F64>) {
                    return os::memory_cast<U64>(&v);
                }
            });
        }
        
        constexpr inline String8 to_str(DType dtype) {
            switch (dtype) {
                case DType::text:       return "text";
                case DType::int_:       return "int";
                case DType::bigint:     return "bigint";
                case DType::smallint:   return "smallint";
                case DType::counter:    return "counter";
                case DType::timestamp:  return "timestamp";
                case DType::boolean:    return "boolean";
                case DType::float_:     return "float";
                case DType::double_:    return "double";
                case DType::uuid:       return "uuid";
            }
            return "unknown";
        }
        
        template<typename T>
        bool can_write(DType dtype) {
            if constexpr (Either<RemoveCV<T>, String8>) {
                return dtype == DType::text || dtype == DType::timestamp || dtype == DType::uuid;
            }
            if constexpr (Either<RemoveCV<T>, S64>) {
                return dtype == DType::int_ || dtype == DType::bigint || dtype == DType::smallint || dtype == DType::counter;
            }
            if constexpr (Either<RemoveCV<T>, bool>) {
                return dtype == DType::boolean;
            }
            if constexpr (Either<RemoveCV<T>, F64, F32>) {
                return dtype == DType::float_ || dtype == DType::double_;
            }
            
            return false;
        }
        
        template<typename F>
        concept Write = requires(F f, const U8* src, U64 size) {
            f(src, size);
        };
    
        template<typename F>
        concept Read = requires(F f, U8* src, U64 size) {
            f(src, size);
        };
    
        // @todo avoid copy for zero init
        void write_default(const Write auto& w, DType dtype) {
            switch (dtype) {
                case DType::text:
                case DType::timestamp:
                case DType::uuid:{
                    U64 length = 0_u64;
                    w(reinterpret_cast<const U8*>(&length), sizeof(length));
                }break;
                case DType::smallint:{
                    S16 smallint = 0_s16;
                    w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
                }break;
                case DType::int_:{
                    S32 int_ = 0_s32;
                    w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
                }break;
                case DType::counter:
                case DType::bigint:{
                    S64 bigint = 0_s64;
                    w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
                }break;
                case DType::boolean:{
                    U8 boolean = false;
                    w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
                }break;
                case DType::float_:{
                    F32 float_ = 0_f32;
                    w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
                }break;
                case DType::double_:{
                    F64 double_ = 0_f64;
                    w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
                }break;
            }
        }
        void write(const Write auto& w, const String8& src, DType dtype) {
            switch (dtype) {
                case DType::text:
                case DType::timestamp:
                case DType::uuid:{
                    w(reinterpret_cast<const U8*>(&src.length), sizeof(src.length));
                    w(reinterpret_cast<const U8*>(src.data), src.length);
                }break;
                default:{
                    assert_true(false, "missing implementation of String8 copy for dtype " + to_str(dtype) + ", this should never happen");
                }break;
            }
        }
        void write(const Write auto& w, S64 src, DType dtype) {
            switch (dtype) {
                case DType::smallint:{
                    S16 smallint = static_cast<S16>(src);
                    w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
                }break;
                case DType::int_:{
                    S32 int_ = static_cast<S32>(src);
                    w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
                }break;
                case DType::counter:
                case DType::bigint:{
                    S64 bigint = static_cast<S64>(src);
                    w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
                }break;
                default:{
                    assert_true(false, "missing implementation of S64 copy for dtype " + to_str(dtype) + ", this should never happen");
                }break;
            }
        }
        void write(const Write auto& w, bool src, DType dtype) {
            switch (dtype) {
                case DType::boolean:{
                    U8 boolean = static_cast<U8>(src);
                    w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
                }break;
                default:{
                    assert_true(false, "missing implementation of bool copy for dtype " + to_str(dtype) + ", this should never happen");
                }break;
            }
        }
        void write(const Write auto& w, F64 src, DType dtype) {
            switch (dtype) {
                case DType::float_:{
                    F32 float_ = static_cast<F32>(src);
                    w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
                }break;
                case DType::double_:{
                    F64 double_ = static_cast<F64>(src);
                    w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
                }break;
                default:{
                    assert_true(false, "missing implementation of F64 copy for dtype " + to_str(dtype) + ", this should never happen");
                }break;
            }
        }
    
        void write(const Write auto& w, const WriteValue& src, DType dtype) {
            visit(src, [&dtype, &w](auto& v) {
                using T = Decay<decltype(v)>;
                using V = Conditional<SameAs<T, AutoString8>, String8, T>;
    
                // default initialise if invalid value
                if (!can_write<V>(dtype)) {
                    write_default(w, dtype);
                    return;
                }
    
                if constexpr (SameAs<T, AutoString8>) {
                    write(w, String8(v), dtype);
                } else {
                    write(w, v, dtype);
                }
            });
        }
    
        ReadValue read(const Read auto& r, DType dtype) {
            switch (dtype) {
                case DType::text:
                case DType::uuid:
                case DType::timestamp:{
                    U64 length;
                    r(reinterpret_cast<U8*>(&length), sizeof(length));

                    AutoString8 value{length};
                    r(reinterpret_cast<U8*>(value.c_str), length);
                
                    return {move(value)};
                }break;
                case DType::smallint:{
                    S16 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::int_:{
                    S32 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::counter:
                case DType::bigint:{
                    S64 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::boolean:{
                    U8 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::float_:{
                    F32 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::double_:{
                    F64 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
            }
        }

        AutoString8 to_str(const ReadValue& value, DType dtype) {
            switch (dtype) {
                case DType::text:
                case DType::uuid:
                case DType::timestamp:{
                    return get<AutoString8>(value);
                }break;
                case DType::smallint:{
                    return plexdb::to_str(get<S16>(value));
                }break;
                case DType::int_:{
                    return plexdb::to_str(get<S32>(value));
                }break;
                case DType::counter:
                case DType::bigint:{
                    return plexdb::to_str(get<S64>(value));
                }break;
                case DType::boolean:{
                    return static_cast<bool>(get<U8>(value)) ? "true"_as : "false"_as;
                }break;
                case DType::float_:{
                    return plexdb::to_str(get<F32>(value));
                }break;
                case DType::double_:{
                    return plexdb::to_str(get<F64>(value));
                }break;
            }
        }
    }
}
