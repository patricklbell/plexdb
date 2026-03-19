export module plexdb.os.dynamic_tagged_union;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb {
    template<typename... Types>
    struct DynamicTaggedUnion {
        static constexpr IndexSequenceFor<Types...> is{};

        static constexpr size_t invalid_index = static_cast<size_t>(-1);

        size_t index = invalid_index;
        void*  ptr   = nullptr;

    private:
        // ====================================================================
        // destructor
        // ====================================================================
        template <size_t I>
        void destroy_at_index() noexcept {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                reinterpret_cast<T*>(ptr)->~T();
            }
        }

        template <size_t... Is>
        void destroy_impl(size_t idx, IndexSequence<Is...>) noexcept {
            ((idx == Is ? (destroy_at_index<Is>(), void()) : void()), ...);
        }

        void destroy() noexcept {
            if (ptr != nullptr) {
                destroy_impl(index, IndexSequenceFor<Types...>{});
                os::deallocate(ptr);
                ptr   = nullptr;
                index = invalid_index;
            }
        }

        // ====================================================================
        // copy into already-allocated ptr
        // ====================================================================
        template <size_t I>
        void copy_construct_at_index(const DynamicTaggedUnion& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                new (ptr) T(*reinterpret_cast<const T*>(other.ptr));
            }
        }

        template <size_t... Is>
        void copy_construct_impl(const DynamicTaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_construct_at_index<Is>(other), void()) : void()), ...);
        }

        // ====================================================================
        // copy assignment helpers
        // ====================================================================
        template <size_t I>
        void copy_assign_at_index(const DynamicTaggedUnion& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                if (index == I) {
                    *reinterpret_cast<T*>(ptr) = *reinterpret_cast<const T*>(other.ptr);
                } else {
                    destroy_impl(index, IndexSequenceFor<Types...>{});
                    new (ptr) T(*reinterpret_cast<const T*>(other.ptr));
                    index = I;
                }
            }
        }

        template <size_t... Is>
        void copy_assign_impl(const DynamicTaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_assign_at_index<Is>(other), void()) : void()), ...);
        }

        // ====================================================================
        // move assignment helpers
        // ====================================================================
        template <size_t I>
        void move_assign_at_index(DynamicTaggedUnion&& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                if (index == I) {
                    *reinterpret_cast<T*>(ptr) = move(*reinterpret_cast<T*>(other.ptr));
                } else {
                    destroy_impl(index, IndexSequenceFor<Types...>{});
                    new (ptr) T(move(*reinterpret_cast<T*>(other.ptr)));
                    index = I;
                }
            }
        }

        template <size_t... Is>
        void move_assign_impl(DynamicTaggedUnion&& other, IndexSequence<Is...>) {
            ((other.index == Is ? (move_assign_at_index<Is>(move(other)), void()) : void()), ...);
        }

    public:
        // ====================================================================
        // constructors
        // ====================================================================
        DynamicTaggedUnion() = default;

        template<typename T>
            requires (SameAs<Decay<T>, Types> || ...)
        DynamicTaggedUnion(T&& value) {
            using DecayedT = Decay<T>;
            ptr = os::allocate(max(sizeof(Types)...));
            new (ptr) DecayedT(forward<T>(value));
            index = TypeIndex<DecayedT, Types...>;
        }

        DynamicTaggedUnion(const DynamicTaggedUnion& other) {
            if (other.ptr != nullptr) {
                ptr = os::allocate(max(sizeof(Types)...));
                copy_construct_impl(other, IndexSequenceFor<Types...>{});
                index = other.index;
            }
        }

        // @note Move steals the allocation; other becomes empty.
        DynamicTaggedUnion(DynamicTaggedUnion&& other) noexcept {
            ptr         = other.ptr;
            index       = other.index;
            other.ptr   = nullptr;
            other.index = invalid_index;
        }

        // ====================================================================
        // destructor
        // ====================================================================
        ~DynamicTaggedUnion() {
            destroy();
        }

        // ====================================================================
        // assignment
        // ====================================================================
        DynamicTaggedUnion& operator=(const DynamicTaggedUnion& other) {
            if (this == &other) return *this;

            if (other.ptr == nullptr) {
                destroy();
            } else if (ptr == nullptr) {
                ptr = os::allocate(max(sizeof(Types)...));
                copy_construct_impl(other, IndexSequenceFor<Types...>{});
                index = other.index;
            } else {
                copy_assign_impl(other, IndexSequenceFor<Types...>{});
            }
            return *this;
        }

        DynamicTaggedUnion& operator=(DynamicTaggedUnion&& other) noexcept {
            if (this == &other) return *this;

            destroy();
            ptr         = other.ptr;
            index       = other.index;
            other.ptr   = nullptr;
            other.index = invalid_index;
            return *this;
        }

        template<typename T>
            requires (SameAs<Decay<T>, Types> || ...)
        DynamicTaggedUnion& operator=(T&& value) {
            using DecayedT = Decay<T>;
            constexpr size_t new_index = TypeIndex<DecayedT, Types...>;

            if (ptr == nullptr) {
                ptr = os::allocate(max(sizeof(Types)...));
                new (ptr) DecayedT(forward<T>(value));
                index = new_index;
            } else if (index == new_index) {
                *reinterpret_cast<DecayedT*>(ptr) = forward<T>(value);
            } else {
                destroy_impl(index, IndexSequenceFor<Types...>{});
                new (ptr) DecayedT(forward<T>(value));
                index = new_index;
            }
            return *this;
        }

        explicit operator bool() const noexcept {
            return ptr != nullptr;
        }
    };

    template<typename T, typename... Types>
    constexpr bool type_matches_tag(const DynamicTaggedUnion<Types...>& u) noexcept {
        using DecayedT = Decay<T>;
        size_t idx = TypeIndex<DecayedT, Types...>;
        return u.index == idx;
    }

    template<typename T, typename... Types>
    T& get(DynamicTaggedUnion<Types...>& u) noexcept {
        assert_true(type_matches_tag<T>(u), "reading wrong type from DynamicTaggedUnion");
        return *reinterpret_cast<T*>(u.ptr);
    }

    template<typename T, typename... Types>
    const T& get(const DynamicTaggedUnion<Types...>& u) noexcept {
        assert_true(type_matches_tag<T>(u), "reading wrong type from DynamicTaggedUnion");
        return *reinterpret_cast<const T*>(u.ptr);
    }

    template<typename... Ts>
    struct ExpandDynamicTaggedUnionHelper;

    template<typename... Ts>
    struct ExpandDynamicTaggedUnionHelper<TypeList<Ts...>> {
        using type = DynamicTaggedUnion<Ts...>;
    };

    template<typename Ts>
    using ExpandDynamicTaggedUnion = ExpandDynamicTaggedUnionHelper<Ts>::type;
}

namespace plexdb {
    // ========================================================================
    // visit
    // ========================================================================
    template<size_t I, typename V, typename Return, typename DTU, typename... Types>
    struct DtuCase {
        static Return apply(DTU& u, V& vis) {
            using T = TypeAtIndex<I, Types...>;
            return vis(get<T>(u));
        }
    };

    template<typename Visitor, typename... Types, size_t... Is>
    decltype(auto) dtu_visit_impl(auto& u, Visitor&& vis, IndexSequence<Is...>) {
        using DTU = RemoveRef<decltype(u)>;
        using V   = RemoveRef<Visitor>;

        using ElemRef = Conditional<IsConst<DTU>, const TypeAtIndex<0, Types...>&, TypeAtIndex<0, Types...>&>;
        using Return  = decltype(declval<V&>()(declval<ElemRef>()));
        using Fn      = Return(*)(DTU&, V&);

        static constexpr Fn table[] = { &DtuCase<Is, V, Return, DTU, Types...>::apply... };
        return table[u.index](u, vis);
    }

    export template<typename Visitor, typename... Types>
    decltype(auto) visit(DynamicTaggedUnion<Types...>& u, Visitor&& vis) {
        assert_true(u.ptr != nullptr, "visiting empty DynamicTaggedUnion");
        return dtu_visit_impl<Visitor, Types...>(u, forward<Visitor>(vis), IndexSequenceFor<Types...>{});
    }

    export template<typename Visitor, typename... Types>
    decltype(auto) visit(const DynamicTaggedUnion<Types...>& u, Visitor&& vis) {
        assert_true(u.ptr != nullptr, "visiting empty DynamicTaggedUnion");
        return dtu_visit_impl<Visitor, Types...>(u, forward<Visitor>(vis), IndexSequenceFor<Types...>{});
    }
}
