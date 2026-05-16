module;
#include <cstddef>
#include <limits>
#include <cstdint>
#include <new>

// it is not possible to get the source location at the call site without
// exposing macros.h or using STL in c++20
#include <source_location>

#include <plexdb/macros/macros.h>

export module plexdb.base.types;

namespace plexdb {
    // ========================================================================
    // private helper types
    // ========================================================================
    template<typename T> struct CVHelper                   { using removed = T; static constexpr bool is = false; };
    template<typename T> struct CVHelper<const T>          { using removed = T; static constexpr bool is = false; };
    template<typename T> struct CVHelper<volatile T>       { using removed = T; static constexpr bool is = false; };
    template<typename T> struct CVHelper<const volatile T> { using removed = T; static constexpr bool is = true;  };

    template<typename T> struct ConstHelper                   { using removed = T; static constexpr bool is = false; };
    template<typename T> struct ConstHelper<const T>          { using removed = T; static constexpr bool is = true;  };

    template<typename T> struct VolatileHelper                { using removed = T; static constexpr bool is = false; };
    template<typename T> struct VolatileHelper<volatile T>    { using removed = T; static constexpr bool is = true;  };

    template<typename T> struct LValueRefHelper                        { using removed = T; using added = T&; static constexpr bool is = false; };
    template<typename T> struct LValueRefHelper<T&>                    { using removed = T; using added = T&; static constexpr bool is = true; };
    template<>           struct LValueRefHelper<void>                  { using removed = void; using added = void; static constexpr bool is = false; };
    template<>           struct LValueRefHelper<const void>            { using removed = const void; using added = const void; static constexpr bool is = false; };
    template<>           struct LValueRefHelper<volatile void>         { using removed = volatile void; using added = volatile void; static constexpr bool is = false; };
    template<>           struct LValueRefHelper<const volatile void>   { using removed = const volatile void; static constexpr bool is = false; };

    template<typename T> struct RefHelper      { using removed = T; static constexpr bool is = false; };
    template<typename T> struct RefHelper<T&>  { using removed = T; static constexpr bool is = true;  };
    template<typename T> struct RefHelper<T&&> { using removed = T; static constexpr bool is = true;  };

    template<typename T> struct ArrayHelper                 { using removed = T; static constexpr bool is = false; };
    template<typename T, size_t N> struct ArrayHelper<T[N]> { using removed = T; static constexpr bool is = true; };

    template<typename T>                       struct FunctionHelper                    { static constexpr bool is = false; };
    template<typename Ret, typename... Args>   struct FunctionHelper<Ret(Args...)>      { using ret = Ret; static constexpr bool is = true; };
    template<typename Ret, typename... Args>   struct FunctionHelper<Ret(Args..., ...)> { using ret = Ret; static constexpr bool is = true; };

    template<bool b, typename T, typename F> struct ConditionalHelper               { using type = T; };
    template<typename T, typename F>         struct ConditionalHelper<false, T, F>  { using type = F; };
}

export namespace plexdb {
    // ========================================================================
    // data region
    // ========================================================================
    const char* not_implemented_msg = "not implemented";

    // ========================================================================
    // numerical types
    // ========================================================================
    using size_t = std::size_t;

    using U8  = std::uint8_t;
    using U16 = std::uint16_t;
    using U32 = std::uint32_t;
    using U64 = std::uint64_t;
    static_assert(sizeof(U8)  == 1);
    static_assert(sizeof(U16) == 2);
    static_assert(sizeof(U32) == 4);
    static_assert(sizeof(U64) == 8);

    using S8  = std::int8_t;
    using S16 = std::int16_t;
    using S32 = std::int32_t;
    using S64 = std::int64_t;
    static_assert(sizeof(S8)  == 1);
    static_assert(sizeof(S16) == 2);
    static_assert(sizeof(S32) == 4);
    static_assert(sizeof(S64) == 8);

    using B8  = S8;
    using B16 = S16;
    using B32 = S32;
    using B64 = S64;

    using F32 = float;
    using F64 = double;
    static_assert(sizeof(F32) == 4);
    static_assert(sizeof(F64) == 8);

    constexpr U8  MAX_U8  = std::numeric_limits<U8 >::max();
    constexpr U16 MAX_U16 = std::numeric_limits<U16>::max();
    constexpr U32 MAX_U32 = std::numeric_limits<U32>::max();
    constexpr U64 MAX_U64 = std::numeric_limits<U64>::max();

    constexpr S8  MAX_S8  = std::numeric_limits<S8 >::max();
    constexpr S16 MAX_S16 = std::numeric_limits<S16>::max();
    constexpr S32 MAX_S32 = std::numeric_limits<S32>::max();
    constexpr S64 MAX_S64 = std::numeric_limits<S64>::max();

    constexpr S8  MIN_S8  = std::numeric_limits<S8 >::min();
    constexpr S16 MIN_S16 = std::numeric_limits<S16>::min();
    constexpr S32 MIN_S32 = std::numeric_limits<S32>::min();
    constexpr S64 MIN_S64 = std::numeric_limits<S64>::min();

    constexpr F32 MAX_F32 = std::numeric_limits<F32>::max();
    constexpr F64 MAX_F64 = std::numeric_limits<F64>::max();

    constexpr F32 MIN_F32 = std::numeric_limits<F32>::min();
    constexpr F64 MIN_F64 = std::numeric_limits<F64>::min();

    constexpr B8  MAX_B8  = std::numeric_limits<B8 >::max();
    constexpr B16 MAX_B16 = std::numeric_limits<B16>::max();
    constexpr B32 MAX_B32 = std::numeric_limits<B32>::max();
    constexpr B64 MAX_B64 = std::numeric_limits<B64>::max();

    constexpr B8  MIN_B8  = std::numeric_limits<B8 >::min();
    constexpr B16 MIN_B16 = std::numeric_limits<B16>::min();
    constexpr B32 MIN_B32 = std::numeric_limits<B32>::min();
    constexpr B64 MIN_B64 = std::numeric_limits<B64>::min();

    template<typename T>
    using NumericLimits = std::numeric_limits<T>;

    constexpr U8 operator""_u8(unsigned long long value) {
        return static_cast<U8>(value);
    }
    constexpr U16 operator""_u16(unsigned long long value) {
        return static_cast<U16>(value);
    }
    constexpr U32 operator""_u32(unsigned long long value) {
        return static_cast<U32>(value);
    }
    constexpr U64 operator""_u64(unsigned long long value) {
        return static_cast<U64>(value);
    }

    constexpr S16 operator""_s16(unsigned long long value) {
        return static_cast<S16>(value);
    }
    constexpr S32 operator""_s32(unsigned long long value) {
        return static_cast<S32>(value);
    }
    constexpr S64 operator""_s64(unsigned long long value) {
        return static_cast<S64>(value);
    }

    constexpr F32 operator""_f32(long double value) {
        return static_cast<F32>(value);
    }
    constexpr F64 operator""_f64(long double value) {
        return static_cast<F64>(value);
    }

    constexpr F32 operator""_f32(unsigned long long value) {
        return static_cast<F32>(value);
    }
    constexpr F64 operator""_f64(unsigned long long value) {
        return static_cast<F64>(value);
    }

    constexpr U64 operator""_kb(unsigned long long value) {
        return static_cast<U64>(1024_u64*value);
    }
    constexpr U64 operator""_mb(unsigned long long value) {
        return static_cast<U64>(1024_u64*1024_u64*value);
    }
    constexpr U64 operator""_gb(unsigned long long value) {
        return static_cast<U64>(1024_u64*1024_u64*1024_u64*value);
    }

    // ========================================================================
    // branch prediction
    // ========================================================================
    template<typename T>
    [[nodiscard, gnu::always_inline]]
    constexpr bool likely(T expr) noexcept {
        return PLEXDB_LIKELY(static_cast<bool>(expr));
    }

    template<typename T>
    [[nodiscard, gnu::always_inline]]
    constexpr bool unlikely(T expr) noexcept {
        return PLEXDB_UNLIKELY(static_cast<bool>(expr));
    }

    // ========================================================================
    // asserts
    // ========================================================================
    #if PLEXDB_DEBUG
        constexpr bool k_assert_enabled = true;
    #else
        constexpr bool k_assert_enabled = false;
    #endif

    using AssertHandler = void(*)(const char* msg, const char* file_name, const char* function_name, unsigned line_number);
    inline AssertHandler g_assert_handler = nullptr;

    // @note not noexcept: the custom assert handler (e.g. Catch2 FAIL) may throw
    constexpr inline void assert_true_always(bool expr, const char* msg, std::source_location loc = std::source_location::current()) {
        if consteval {
            PLEXDB_CONSTEVAL_TRAP(expr);
        } else {
            if (unlikely(!expr)) {
                if (g_assert_handler != nullptr) {
                    g_assert_handler(msg, loc.file_name(), loc.function_name(), loc.line());
                } else {
                    PLEXDB_TRAP;
                }
            }
        }
    }
    constexpr inline void assert_true(bool expr, const char* msg, std::source_location loc = std::source_location::current()) {
        if constexpr (!k_assert_enabled) {
            if consteval {} else {
                return;
            }
        }
        assert_true_always(expr, msg, loc);
    }

    constexpr inline void assert_true_not_implemented(bool expr, const char* msg = not_implemented_msg, std::source_location loc = std::source_location::current()) {
        if (unlikely(!expr)) {
            if (g_assert_handler != nullptr) {
                g_assert_handler(msg, loc.file_name(), loc.function_name(), loc.line());
            } else {
                PLEXDB_TRAP;
            }
        }
    }

    constexpr inline void assert_not_implemented(const char* msg = not_implemented_msg, std::source_location loc = std::source_location::current()) {
        assert_true_not_implemented(false, msg, loc);
    }
    void set_assert_handler(AssertHandler h) noexcept;

    // ========================================================================
    // casts
    // ========================================================================
    template<typename T>
    T&& declval() noexcept; // @note declaration only

    template<typename T>
    constexpr T&& move(T& value) noexcept {
        return static_cast<T&&>(value);
    }

    template<typename T>
    using RemoveCV = typename CVHelper<T>::removed;

    template<typename T>
    using RemoveConst = typename ConstHelper<T>::removed;

    template<typename T>
    using RemoveVolatile = typename VolatileHelper<T>::removed;

    template<typename T>
    using RemoveRef = typename RefHelper<T>::removed;

    template<typename T>
    using RemoveCVRef = RemoveCV<RemoveRef<T>>;

    template<typename T>
    using AddLValueReference = typename LValueRefHelper<T>::added;

    template<typename T>
    using RemoveExtent = typename ArrayHelper<T>::removed;

    template<typename T>
    constexpr T&& forward(RemoveRef<T>& t) noexcept {
        return static_cast<T&&>(t);
    }

    template<typename T>
    constexpr T&& forward(RemoveRef<T>&& t) noexcept {
        static_assert(!LValueRefHelper<T>::is, "forward<T>(T&&) called with T as lvalue reference");
        return static_cast<T&&>(t);
    }

    template<bool b, typename T, typename F>
    using Conditional = typename ConditionalHelper<b, T, F>::type;

    template<typename It>
    using IterValue = RemoveCV<RemoveRef<decltype(*declval<It>())>>;

    // ========================================================================
    // concepts
    // ========================================================================
    template<typename T>
    concept IsCV = CVHelper<T>::is;

    template<typename T>
    concept IsConst = ConstHelper<T>::is;

    template<typename T>
    concept IsVolatile = VolatileHelper<T>::is;

    template<typename T>
    concept IsLValue = LValueRefHelper<T>::is;

    template<typename T>
    concept IsArray = ArrayHelper<T>::is;

    template<typename T>
    concept IsFunction = FunctionHelper<T>::is;

    template<class T>
    concept TriviallyCopyable = __is_trivially_copyable(T);

    template<class T>
    concept TriviallyConstructible = __is_trivially_constructible(T);

    template<typename T, typename U>
    concept SameAs = __is_same_as(T, U);

    template<typename From, typename To>
    concept ConvertibleTo = requires(From f) {
        static_cast<To>(f);
    };

    template<typename T>
    concept CopyAssignable = requires(T& lhs, const T& rhs) {
        { lhs = rhs } -> SameAs<T&>;
    };

    template<typename T>
    concept NoThrowMoveConstructible = __is_nothrow_constructible(T, T&&);

    template<typename T>
    concept NoThrowMoveAssignable = __is_nothrow_assignable(T, T&&);

    template<typename T, typename U>
    concept NoThrowAssignable = __is_nothrow_assignable(T, U);

    template<typename... Types>
    concept AllNoThrowMoveConstructible = (NoThrowMoveConstructible<Types> && ...);

    template<typename... Types>
    concept AllNoThrowMoveAssignable = (NoThrowMoveAssignable<Types> && ...);

    template<typename T, typename ... U>
    concept Either = (SameAs<T, U> || ...);

    // ========================================================================
    // pair
    // ========================================================================
    template<typename A, typename B>
    struct Pair {
        using First = A;
        using Second = B;

        A first{};
        B second{};

        Pair() = default;
        Pair(A first, B second) : first(plexdb::move(first)), second(plexdb::move(second)) {}
    };

    template<typename T> struct IsPairHelper                        { static constexpr bool is = false; };
    template<typename U, typename V> struct IsPairHelper<Pair<U,V>> { static constexpr bool is = true;  };

    template<typename T>
    concept IsPair = IsPairHelper<T>::is;

    // ========================================================================
    // exchange and swap
    // ========================================================================
    template<class T, class U = T>
    constexpr T exchange(T& obj, U&& new_value)
        noexcept(
            NoThrowMoveConstructible<T> &&
            NoThrowAssignable<T&, U>
        )
    {
        T old_value = move(obj);
        obj = forward<U>(new_value);
        return old_value;
    }

    template<typename T>
    constexpr void swap(T& a, T& b)
        noexcept(
            noexcept(T(move(a)))  &&
            noexcept(a = move(b)) &&
            noexcept(b = move(a))
        )
    {
        T tmp = move(a);
        a = move(b);
        b = move(tmp);
    }

    // ========================================================================
    // scope
    // ========================================================================
    template<typename F, typename T>
    concept ScopeStartFunction = requires(F f) {
        {f()} -> SameAs<T>;
    };
    template<typename F, typename T>
    concept ScopeEndFunction = requires(F f, T& t) {
        {f(t)} -> SameAs<void>;
    };

    template<typename T, typename Start, typename End>
        requires ScopeStartFunction<Start, T> && ScopeEndFunction<End, T>
    struct Scope {
        T t;
        End end;
        explicit Scope(Start start, End end) noexcept : t(start()), end(end) {}
        ~Scope() { end(t); }
    };

    // ========================================================================
    // views
    // ========================================================================
    template<typename T, typename Length = size_t>
    struct TArrayView {
        T* ptr;
        Length length;

        TArrayView(T* ptr, Length length): ptr(ptr), length(length) {}
        explicit TArrayView(): ptr(nullptr), length(0) {}

        template<size_t N>
        TArrayView(T (&arr)[N]) : ptr(arr), length(N) {}

        template<typename U = T>
        TArrayView(const TArrayView<RemoveCV<T>, Length>& other) requires (IsConst<U>): ptr(other.ptr), length(other.length) {}

        using Iterator = T*;
        using ConstIterator = const T*;

        Iterator begin() noexcept { return ptr; }
        ConstIterator begin() const noexcept { return ptr; }
        ConstIterator cbegin() const noexcept { return ptr; }

        // @note allows iterator for zero length views
        Iterator end() noexcept { return length > 0 ? ptr + length : ptr; }
        ConstIterator end() const noexcept { return length > 0 ? ptr + length : ptr; }
        ConstIterator cend() const noexcept { return length > 0 ? ptr + length : ptr; }

        AddLValueReference<T> operator[](Length i) const noexcept {
            assert_true(i >= 0 && i < this->length, "out of range");
            return ptr[i];
        }

        template<typename U = T>
        bool operator==(const TArrayView<U,Length>& b) const {
            if (b.length != this->length)
                return false;
            for (Length idx = 0; idx < this->length; idx++) {
                if (this->ptr[idx] != b.ptr[idx])
                    return false;
            }
            return true;
        }
    };
    template<typename T, typename Length>
    TArrayView<T,Length> view_shift_up(const TArrayView<T,Length>& in, Length offset=static_cast<Length>(1)) {
        assert_true(in.length > 0 || offset == 0, "avoid underflow in view shift.");
        return TArrayView<T,Length>{in.ptr + offset, static_cast<Length>(in.length - offset)};
    }
    template<typename T, typename Length>
    TArrayView<T,Length> view_shift_down(const TArrayView<T,Length>& in, Length offset=static_cast<Length>(1)) {
        // @todo overflow assert
        return TArrayView<T,Length>{in.ptr - offset, static_cast<Length>(in.length + offset)};
    }

    template<typename T, typename Length>
    void append_in_place(TArrayView<T,Length>& in, T value) {
        assert_true(in.ptr != nullptr, "cannot append to empty view");
        in.length++;
        in[in.length - 1] = value;
    }

    template<typename T, typename Length = size_t>
    struct CappedTArrayView : TArrayView<T, Length> {
        using Base = TArrayView<T, Length>;

        Length cap;

        CappedTArrayView(T* ptr, Length length, Length cap): Base(ptr, length), cap(cap) {}

        explicit CappedTArrayView(): Base(), cap(0) {}

        template<size_t N>
        CappedTArrayView(T (&arr)[N]): Base(arr), cap(sizeof(T) * N) {}

        template<typename U = T>
        CappedTArrayView(const CappedTArrayView<RemoveCV<T>, Length>& other) requires (IsConst<U>): Base(other.ptr, other.length), cap(other.cap) {}

        // @note allows iterator for zero cap views
        Base::Iterator end() noexcept { return this->cap > 0 ? this->ptr + this->cap : this->ptr; }
        Base::ConstIterator end() const noexcept { return this->cap > 0 ? this->ptr + this->cap : this->ptr; }
        Base::ConstIterator cend() const noexcept { return this->cap > 0 ? this->ptr + this->cap : this->ptr; }

        AddLValueReference<T> operator[](Length i) const noexcept {
            assert_true(i >= 0 && i < this->cap, "out of range");
            return Base::operator[](i);
        }

        template<typename U = T>
        bool operator==(const CappedTArrayView<U,Length>& b) const {
            if (b.cap != this->cap)
                return false;
            for (Length idx = 0; idx < this->cap; idx++) {
                if (this->ptr[idx] != b.ptr[idx])
                    return false;
            }
            return true;
        }
    };

    template<typename Length = size_t, typename Size = size_t, typename Byte = U8>
        requires Either<Byte, U8, const U8, S8, const S8>
    struct ArrayView {
        Byte* ptr;
        Length length;
        Size el_size;

        explicit ArrayView(Byte* ptr, Size el_size, Length length) : ptr(ptr), length(length), el_size(el_size) {}

        template<size_t N>
        explicit ArrayView(Byte (&arr)[N]) : ptr(arr), length(N), el_size(1) {}

        template<typename B = Byte>
        ArrayView(const ArrayView<Length, Size, RemoveConst<B>>& other) requires (IsConst<B>): ptr(other.ptr), length(other.length), el_size(other.el_size) {}

        struct Iterator {
            Byte* current;
            Size el_size;

            Iterator(Byte* p, Size s) : current(p), el_size(s) {}

            Iterator& operator++() { current += el_size; return *this; }
            Iterator operator++(int) { Iterator tmp = *this; ++(*this); return tmp; }

            Byte* operator*() const { return current; }

            bool operator==(const Iterator& other) const { return current == other.current; }
            bool operator!=(const Iterator& other) const { return !(*this == other); }
        };

        struct ConstIterator {
            const Byte* current;
            Size el_size;

            ConstIterator(const Byte* p, Size s) : current(p), el_size(s) {}

            ConstIterator& operator++() { current += el_size; return *this; }
            ConstIterator operator++(int) { ConstIterator tmp = *this; ++(*this); return tmp; }

            const Byte* operator*() const { return current; }

            bool operator==(const ConstIterator& other) const { return current == other.current; }
            bool operator!=(const ConstIterator& other) const { return !(*this == other); }
        };

        Iterator begin() noexcept { return Iterator{ptr, el_size}; }
        ConstIterator begin() const noexcept { return ConstIterator{ptr, el_size}; }
        ConstIterator cbegin() const noexcept { return ConstIterator{ptr, el_size}; }

        Iterator end() noexcept { return Iterator{ptr + length * el_size, el_size}; }
        ConstIterator end() const noexcept { return ConstIterator{ptr + length * el_size, el_size}; }
        ConstIterator cend() const noexcept { return ConstIterator{ptr + length * el_size, el_size}; }

        Byte* operator[](Length i) noexcept {
            assert_true(i >= 0 && i < this->length, "out of range");
            return ptr + i*el_size;
        }
        const Byte* operator[](Length i) const noexcept {
            assert_true(i >= 0 && i < this->length, "out of range");
            return ptr + i*el_size;
        }

        using Idx = decltype(length*el_size);

        template<typename L = Length, typename S = Size, typename B = Byte>
        bool operator==(const ArrayView<L,S,B>& b) const {
            if (b.length*b.el_size != this->length*this->el_size)
                return false;
            for (Idx idx = 0; idx < this->length*this->el_size; idx++) {
                if (this->ptr[idx] != b->ptr[idx])
                    return false;
            }
            return true;
        }

        bool operator==(const U8* b) const { // @note obviously
            for (Idx idx = 0; idx < this->length*this->el_size; idx++) {
                if (this->ptr[idx] != b[idx])
                    return false;
            }
            return true;
        }
    };
    template<typename Length, typename Size, typename Byte>
    ArrayView<Length,Size,Byte> view_shift_up(const ArrayView<Length,Size,Byte>& in, Length offset=static_cast<Length>(1)) {
        assert_true(in.length > 0 || offset == 0, "avoid underflow in view shift.");
        return ArrayView<Length,Size,Byte>{in.ptr + offset*in.el_size, in.el_size, static_cast<Length>(in.length - offset)};
    }
    template<typename Length, typename Size, typename Byte>
    ArrayView<Length,Size,Byte> view_shift_down(const ArrayView<Length,Size,Byte>& in, Length offset=static_cast<Length>(1)) {
        // @todo overflow assert
        return ArrayView<Length,Size,Byte>{in.ptr - offset*in.el_size, in.el_size, static_cast<Length>(in.length + offset)};
    }

    template<typename F, typename T, typename Length = size_t>
    concept TArrayFlushFunction = requires(F f, TArrayView<T,Length>& buffer, Length& length) {
        f(buffer, length);
    };

    template<typename F, typename T, typename Length = size_t>
        requires TArrayFlushFunction<F, T, Length>
    struct FlushableTArray {
        TArrayView<T,Length> buffer;
        Length length;
        F flush;

        FlushableTArray(TArrayView<T,Length> b, F f) : buffer(b), length(0), flush(f) {}
        ~FlushableTArray() { this->flush(this->buffer, this->length); }
    };

    template<typename F, typename T, typename Length = size_t>
        requires TArrayFlushFunction<F, T, Length>
    void flush_if_needed(FlushableTArray<F,T,Length>& arr) {
        assert_true(arr.length <= arr.buffer.length, "invalid flush state, length overflows buffer");

        if (arr.length == arr.buffer.length) {
            arr.flush(arr.buffer, arr.length);
            arr.length = 0;
        }
    }

    template<typename F, typename T, typename Length = size_t>
        requires TArrayFlushFunction<F, T, Length>
    void append(FlushableTArray<F,T,Length>& arr, const TArrayView<T,Length>& postfix) {
        const T* src = postfix.ptr;
        const T* end = postfix.ptr + postfix.length;

        while (src != end) {
            flush_if_needed(arr);

            U64 count = min(static_cast<U64>(end - src), arr.buffer.length - arr.length);
            assert_true(count > 0, "string8 buffer is zero length");

            // @todo memory copy
            for (U64 i = 0; i < count; i++) {
                arr.buffer.ptr[arr.length++] = src[i];
            }

            src += count;
        }
    }

    // ========================================================================
    // binary search
    // ========================================================================
    enum class BinarySearchPolicy { GreaterEqual, Greater };

    template<typename It, typename Offset, typename Key>
    concept BinarySearchIterator = requires(It a, IterValue<It> v, Offset n, Key k) {
        { a + n } -> SameAs<It>;
        { *a };
        { v <  k } -> ConvertibleTo<bool>;
        { v <= k } -> ConvertibleTo<bool>;
    };

    template<BinarySearchPolicy Type, typename It, typename CountType, typename KeyType>
        requires BinarySearchIterator<It, CountType, KeyType>
    constexpr CountType binary_search(It begin, CountType count, const KeyType& key) noexcept {
        CountType L = 0;
        CountType R = count;

        while (L < R) {
            CountType M = L + (R - L) / 2;
            const auto& mid = *(begin + M);

            if constexpr (Type == BinarySearchPolicy::GreaterEqual) {
                if (mid < key)
                    L = M + 1;
                else
                    R = M;
            } else if constexpr (Type == BinarySearchPolicy::Greater) {
                if (mid <= key)
                    L = M + 1;
                else
                    R = M;
            } else {
                static_assert(false);
            }
        }

        return L;
    }

    template<typename T, typename Length>
    constexpr Length binary_search_first_geq(const TArrayView<T,Length>& view, const RemoveCV<T>& key) noexcept {
        return binary_search<BinarySearchPolicy::GreaterEqual>(view.cbegin(), view.length, key);
    }

    template<typename T, typename Length>
    constexpr Length binary_search_first_gt(const TArrayView<T,Length>& view, const RemoveCV<T>& key) noexcept {
        return binary_search<BinarySearchPolicy::Greater>(view.cbegin(), view.length, key);
    }

    // not strictly necessary but can be clearer
    template<typename T, typename Length>
    constexpr Length binary_search_last_leq(const TArrayView<T,Length>& view,
                                            const RemoveCV<T>& key) noexcept {
        auto idx = binary_search_first_gt(view, key);
        return idx == 0 ? 0 : idx - 1;
    }

    template<typename T, typename Length>
    constexpr Length binary_search_last_lt(const TArrayView<T,Length>& view,
                                           const RemoveCV<T>& key) noexcept {
        auto idx = binary_search_first_geq(view, key);
        return idx == 0 ? 0 : idx - 1;
    }

    // ========================================================================
    // optional
    // ========================================================================
    template<typename T>
    class Optional {
    private:
        alignas(T) U8 storage[sizeof(T)];
        bool engaged = false;

        T* ptr() noexcept {
            assert_true(engaged, "accessed data on empty optional");
            return reinterpret_cast<T*>(&storage);
        }

        const T* ptr() const noexcept {
            assert_true(engaged, "accessed data on empty optional");
            return reinterpret_cast<const T*>(&storage);
        }

    public:
        constexpr Optional() noexcept = default;

        constexpr Optional(const T& value) {
            engaged = true;
            new (ptr()) T(value);
        }

        constexpr Optional(T&& value) {
            engaged = true;
            new (ptr()) T(move(value));
        }

        constexpr Optional(const Optional& other) {
            if (other.engaged) {
                engaged = true;
                new (ptr()) T(*other.ptr());
            }
        }

        constexpr Optional(Optional&& other) {
            if (other.engaged) {
                engaged = true;
                new (ptr()) T(move(*other.ptr()));
            }
        }

        ~Optional() {
            reset();
        }

        Optional& operator=(const Optional& other) {
            if (this != &other) {
                if (engaged && other.engaged) {
                    **this = *other;
                } else if (other.engaged) {
                    emplace(*other);
                } else {
                    reset();
                }
            }
            return *this;
        }

        Optional& operator=(Optional&& other) {
            if (this != &other) {
                if (engaged && other.engaged) {
                    **this = move(*other);
                } else if (other.engaged) {
                    emplace(move(*other));
                } else {
                    reset();
                }
            }
            return *this;
        }

        template<typename... Args>
        T& emplace(Args&&... args) {
            reset();
            engaged = true;
            new (ptr()) T(forward<Args>(args)...);
            return *ptr();
        }

        void reset() noexcept {
            if (engaged) {
                ptr()->~T();
                engaged = false;
            }
        }

        constexpr bool has_value() const noexcept {
            return engaged;
        }

        constexpr explicit operator bool() const noexcept {
            return engaged;
        }

        constexpr T& value() & noexcept {
            return *ptr();
        }

        constexpr const T& value() const & noexcept {
            return *ptr();
        }

        constexpr T&& value() && noexcept {
            return move(*ptr());
        }

        constexpr T& operator*() & noexcept { return *ptr(); }
        constexpr const T& operator*() const & noexcept { return *ptr(); }
        constexpr T* operator->() noexcept { return ptr(); }
        constexpr const T* operator->() const noexcept { return ptr(); }
    };

    // ========================================================================
    // type tagged pointer
    // @todo const
    // ========================================================================
    template<typename T, typename V>
    struct Tag {
        V* value;

        Tag(V* in_value): value(in_value) {}
    };

    template<typename T, typename V>
    Tag<T,V> create_tag(V* in_value) {
        return Tag<T,V>(in_value);
    }

    // ========================================================================
    // array placement new
    //   @note builtin placement new may store the length requiring extra allocation
    // ========================================================================
    template <typename T>
    T* default_construct_placement_array(T* array, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            new (&array[i]) T();
        }
        return array;
    }
}

namespace plexdb {
    // ========================================================================
    // type indexing
    // ========================================================================
    template<typename T, typename... Types>
    struct TypeIndexHelper;

    template<typename T, typename First, typename... Rest>
    struct TypeIndexHelper<T, First, Rest...> {
        static constexpr size_t value = SameAs<T, First> ? 0 : 1 + TypeIndexHelper<T, Rest...>::value;
    };

    template<typename T, typename First>
    struct TypeIndexHelper<T, First> {
        static constexpr size_t value = 0;
    };

    export template<typename T, typename... Types>
    inline constexpr size_t TypeIndex = TypeIndexHelper<T, Types...>::value;

    template<size_t I, typename... Types>
    struct TypeAtIndexHelper;

    template<size_t I, typename First, typename... Rest>
    struct TypeAtIndexHelper<I, First, Rest...> {
        using type = typename TypeAtIndexHelper<I - 1, Rest...>::type;
    };

    template<typename First, typename... Rest>
    struct TypeAtIndexHelper<0, First, Rest...> {
        using type = First;
    };

    export template<size_t I, typename... Types>
    using TypeAtIndex = typename TypeAtIndexHelper<I, Types...>::type;

    export template<size_t... Ints>
    struct IndexSequence {
        using type = IndexSequence;
    };

    template<size_t N, size_t... Ints>
    struct MakeIndexSequenceHelper : MakeIndexSequenceHelper<N - 1, N - 1, Ints...> {};

    template<size_t... Ints>
    struct MakeIndexSequenceHelper<0, Ints...> {
        using type = IndexSequence<Ints...>;
    };

    export template<size_t N>
    using MakeIndexSequence = typename MakeIndexSequenceHelper<N>::type;

    export template<typename... Ts>
    using IndexSequenceFor = MakeIndexSequence<sizeof...(Ts)>;

    // ========================================================================
    // decay
    // ========================================================================
    template<typename T>
    struct DecayHelper {
        using U = RemoveRef<T>;

        using type = Conditional<
            IsArray<U>,
            RemoveExtent<U>*,
            Conditional<
                IsFunction<U>,
                U*,
                RemoveCV<U>
            >
        >;
    };

    export template<typename T>
    using Decay = typename DecayHelper<T>::type;

    // ========================================================================
    // type combinatorics
    // ========================================================================
    export template<typename... Ts>
    struct TypeList {};

    export template<typename T, typename... Ts>
    constexpr bool is_in_type_list(TypeList<Ts...>*) {
        return (SameAs<T, Ts> || ...);
    }

    export template<typename T, typename List>
    concept IsInTypeList = is_in_type_list<T>((List*)nullptr);

    template<typename... Lists>
    struct ConcatHelper;

    template<>
    struct ConcatHelper<> {
        using type = TypeList<>;
    };

    template<typename List>
    struct ConcatHelper<List> {
        using type = List;
    };

    template<typename List1, typename List2, typename... Rest>
    struct ConcatHelper<List1, List2, Rest...> {
        using type = typename ConcatHelper<typename ConcatHelper<List1, List2>::type, Rest...>::type;
    };

    template<typename... Ts1, typename... Ts2>
    struct ConcatHelper<TypeList<Ts1...>, TypeList<Ts2...>> {
        using type = TypeList<Ts1..., Ts2...>;
    };

    export template<typename... Lists>
    using Concat = typename ConcatHelper<Lists...>::type;
}
