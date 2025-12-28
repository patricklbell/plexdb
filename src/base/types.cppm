module;
#include <limits>
#include <cstdint>
#include <source_location>
#include "macros.h"

export module plexdb.base.types;

namespace plexdb {
    // ========================================================================
    // helper traits
    // ========================================================================
    template<typename T> struct remove_cv                   { using type = T; };
    template<typename T> struct remove_cv<const T>          { using type = T; };
    template<typename T> struct remove_cv<volatile T>       { using type = T; };
    template<typename T> struct remove_cv<const volatile T> { using type = T; };

    template<typename T> struct remove_reference      { using type = T; };
    template<typename T> struct remove_reference<T&>  { using type = T; };
    template<typename T> struct remove_reference<T&&> { using type = T; };
}

export namespace plexdb {
    // ========================================================================
    // numerical types
    // ========================================================================
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

    #ifdef PLEXDB_DEBUG
        constexpr bool k_assert_enabled = true;
    #else
        constexpr bool k_assert_enabled = false;
    #endif

    using AssertHandler = void(*)(const char*, const char*, const char*, unsigned);
    inline AssertHandler g_assert_handler = nullptr;

    inline void assert_true_always(bool expr, const char* msg, std::source_location loc = std::source_location::current()) noexcept {
        if (unlikely(!expr)) {
            if (g_assert_handler != nullptr)
                g_assert_handler(msg, loc.file_name(), loc.function_name(), loc.line());
            PLEXDB_TRAP;
        }
    }
    inline void assert_true(bool expr, const char* msg, std::source_location loc = std::source_location::current()) noexcept {
        if constexpr (!k_assert_enabled)
            return;
        assert_true_always(expr, msg, loc);
    }
    inline void assert_not_implemented(std::source_location loc = std::source_location::current()) noexcept {
        assert_true_always(true, "not implemented", loc);
    }
    void set_assert_handler(AssertHandler h) noexcept;


    // ========================================================================
    // casts
    // ========================================================================
    template<typename T>
    T&& declval() noexcept; // @note declaration only

    template <typename T>
    constexpr T&& move(T& value) noexcept {
        return static_cast<T&&>(value);
    }

    template<typename T>
    using RemoveCV = typename remove_cv<T>::type;
    
    template<typename T>
    using RemoveReference = typename remove_reference<T>::type;

    template<typename T>
    constexpr T&& forward(RemoveReference<T>& t) noexcept {
        return static_cast<T&&>(t);
    }

    template<typename T>
    constexpr T&& forward(RemoveReference<T>&& t) noexcept {
        static_assert(!__is_lvalue_reference(T), "forward<T>(T&&) called with T as lvalue reference");
        return static_cast<T&&>(t);
    }

    template<typename It>
    using IterValue = RemoveCV<RemoveReference<decltype(*declval<It>())>>;

    // ========================================================================
    // concepts
    // ========================================================================
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

    template<typename T, typename U>
    concept NoThrowAssignable = __is_nothrow_assignable(T, U);

    template<typename T, typename ... U>
    concept Either = (SameAs<T, U> || ...);

    // ========================================================================
    // pair
    // ========================================================================
    template<typename A, typename B>
    struct Pair {
        union {
            A key;
            A first;
        };
        union {
            B value;
            B second;
        };

        Pair(A first, B second) : first(first), second(second) {}
    };

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
    // array
    // ========================================================================
    template<typename T, typename Length>
    struct TArrayView {
        T* ptr;
        Length length;

        explicit TArrayView(T* ptr, Length length) : ptr(ptr), length(length) {}
        explicit TArrayView(U8* ptr, Length length) : ptr(reinterpret_cast<T*>(ptr)), length(length) {}

        using iterator = T*;
        using const_iterator = const T*;
        using value_type = T;
        using reference = T&;
        using const_reference = const T&;
        using size_type = Length;

        iterator begin() noexcept { return ptr; }
        const_iterator begin() const noexcept { return ptr; }
        const_iterator cbegin() const noexcept { return ptr; }

        iterator end() noexcept { return ptr + length; }
        const_iterator end() const noexcept { return ptr + length; }
        const_iterator cend() const noexcept { return ptr + length; }

        reference operator[](size_type i) noexcept { return ptr[i]; }
        const_reference operator[](size_type i) const noexcept { return ptr[i]; }
    };
    template<typename T, typename Length>
    TArrayView<T,Length> view_shift_left(const TArrayView<T,Length>& in, Length offset=static_cast<Length>(1)) {
        assert_true(in.length > 0 || offset == 0, "avoid underflow in view shift.");
        return TArrayView<T,Length>{in.ptr + offset, static_cast<Length>(in.length - offset)};
    }
    template<typename T, typename Length>
    TArrayView<T,Length> view_shift_right(const TArrayView<T,Length>& in, Length offset=static_cast<Length>(1)) {
        // @todo overflow assert
        return TArrayView<T,Length>{in.ptr - offset, static_cast<Length>(in.length + offset)};
    }

    template<typename Length, typename Size = U64>
    struct ArrayView {
        U8* ptr;
        Length length;
        Size el_size;

        explicit ArrayView(U8* ptr, Size el_size, Length length) : ptr(ptr), el_size(el_size), length(length) {}

        struct iterator {
            U8* current;
            Size el_size;

            iterator(U8* p, Size s) : current(p), el_size(s) {}

            iterator& operator++() { current += el_size; return *this; }
            iterator operator++(int) { iterator tmp = *this; ++(*this); return tmp; }

            U8* operator*() const { return current; }

            bool operator==(const iterator& other) const { return current == other.current; }
            bool operator!=(const iterator& other) const { return !(*this == other); }
        };

        struct const_iterator {
            const U8* current;
            Size el_size;

            const_iterator(const U8* p, Size s) : current(p), el_size(s) {}

            const_iterator& operator++() { current += el_size; return *this; }
            const_iterator operator++(int) { const_iterator tmp = *this; ++(*this); return tmp; }

            const U8* operator*() const { return current; }

            bool operator==(const const_iterator& other) const { return current == other.current; }
            bool operator!=(const const_iterator& other) const { return !(*this == other); }
        };

        iterator begin() noexcept { return iterator{ptr, el_size}; }
        const_iterator begin() const noexcept { return const_iterator{ptr, el_size}; }
        const_iterator cbegin() const noexcept { return const_iterator{ptr, el_size}; }

        iterator end() noexcept { return iterator{ptr + length * el_size, el_size}; }
        const_iterator end() const noexcept { return const_iterator{ptr + length * el_size, el_size}; }
        const_iterator cend() const noexcept { return const_iterator{ptr + length * el_size, el_size}; }

        U8* operator[](Length i) noexcept { return ptr + i*el_size; }
        const U8* operator[](Length i) const noexcept { return ptr + i*el_size; }
    };
    template<typename Length, typename Size>
    ArrayView<Length,Size> view_shift_left(const ArrayView<Length,Size>& in, Length offset=static_cast<Length>(1)) {
        assert_true(in.length > 0 || offset == 0, "avoid underflow in view shift.");
        return ArrayView<Length,Size>{in.ptr + offset*in.el_size, in.el_size, static_cast<Length>(in.length - offset)};
    }
    template<typename Length, typename Size>
    ArrayView<Length,Size> view_shift_right(const ArrayView<Length,Size>& in, Length offset=static_cast<Length>(1)) {
        // @todo overflow assert
        return ArrayView<Length,Size>{in.ptr - offset*in.el_size, in.el_size, static_cast<Length>(in.length + offset)};
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
    constexpr Length binary_search_first_geq(const TArrayView<T,Length>& view, const T& key) noexcept {
        return binary_search<BinarySearchPolicy::GreaterEqual>(view.cbegin(), view.length, key);
    }

    template<typename T, typename Length>
    constexpr Length binary_search_first_gt(const TArrayView<T,Length>& view, const T& key) noexcept {
        return binary_search<BinarySearchPolicy::Greater>(view.cbegin(), view.length, key);
    }

    // ========================================================================
    // optional
    // ========================================================================
    template <typename T>
    class Optional {
    private:
        alignas(T) U8 storage[sizeof(T)];
        bool engaged = false;

        T* ptr() noexcept {
            return reinterpret_cast<T*>(&storage);
        }

        const T* ptr() const noexcept {
            return reinterpret_cast<const T*>(&storage);
        }

    public:
        constexpr Optional() noexcept = default;

        constexpr Optional(const T& value) {
            new (ptr()) T(value);
            engaged = true;
        }

        constexpr Optional(T&& value) noexcept {
            new (ptr()) T(move(value));
            engaged = true;
        }

        constexpr Optional(const Optional& other) {
            if (other.engaged) {
                new (ptr()) T(*other.ptr());
                engaged = true;
            }
        }

        constexpr Optional(Optional&& other) noexcept {
            if (other.engaged) {
                new (ptr()) T(move(*other.ptr()));
                engaged = true;
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

        Optional& operator=(Optional&& other) noexcept {
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

        template <typename... Args>
        T& emplace(Args&&... args) {
            reset();
            new (ptr()) T(forward<Args>(args)...);
            engaged = true;
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
}