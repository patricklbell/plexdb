export module plexdb.dynamic.tagged_union;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb {
    template<typename... Types>
    struct AutoTaggedUnion {
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
        void copy_construct_at_index(const AutoTaggedUnion& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                new (ptr) T(*reinterpret_cast<const T*>(other.ptr));
            }
        }

        template <size_t... Is>
        void copy_construct_impl(const AutoTaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_construct_at_index<Is>(other), void()) : void()), ...);
        }

        // ====================================================================
        // copy assignment helpers
        // ====================================================================
        template <size_t I>
        void copy_assign_at_index(const AutoTaggedUnion& other) {
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
        void copy_assign_impl(const AutoTaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_assign_at_index<Is>(other), void()) : void()), ...);
        }

        // ====================================================================
        // move assignment helpers
        // ====================================================================
        template <size_t I>
        void move_assign_at_index(AutoTaggedUnion&& other) {
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
        void move_assign_impl(AutoTaggedUnion&& other, IndexSequence<Is...>) {
            ((other.index == Is ? (move_assign_at_index<Is>(move(other)), void()) : void()), ...);
        }

    public:
        // ====================================================================
        // constructors
        // ====================================================================
        AutoTaggedUnion() = default;

        template<typename T>
            requires (SameAs<Decay<T>, Types> || ...)
        AutoTaggedUnion(T&& value) {
            using DecayedT = Decay<T>;
            ptr = os::allocate(max(sizeof(Types)...));
            new (ptr) DecayedT(forward<T>(value));
            index = TypeIndex<DecayedT, Types...>;
        }

        AutoTaggedUnion(const AutoTaggedUnion& other) {
            if (other.ptr != nullptr) {
                ptr = os::allocate(max(sizeof(Types)...));
                copy_construct_impl(other, IndexSequenceFor<Types...>{});
                index = other.index;
            }
        }

        // @note Move steals the allocation; other becomes empty.
        AutoTaggedUnion(AutoTaggedUnion&& other) noexcept {
            ptr         = other.ptr;
            index       = other.index;
            other.ptr   = nullptr;
            other.index = invalid_index;
        }

        // ====================================================================
        // destructor
        // ====================================================================
        ~AutoTaggedUnion() {
            destroy();
        }

        // ====================================================================
        // assignment
        // ====================================================================
        AutoTaggedUnion& operator=(const AutoTaggedUnion& other) {
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

        AutoTaggedUnion& operator=(AutoTaggedUnion&& other) noexcept {
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
        AutoTaggedUnion& operator=(T&& value) {
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
    constexpr bool type_matches_tag(const AutoTaggedUnion<Types...>& u) noexcept {
        using DecayedT = Decay<T>;
        size_t idx = TypeIndex<DecayedT, Types...>;
        return u.index == idx;
    }

    template<typename T, typename... Types>
    T& get(AutoTaggedUnion<Types...>& u) noexcept {
        assert_true(type_matches_tag<T>(u), "reading wrong type from AutoTaggedUnion");
        return *reinterpret_cast<T*>(u.ptr);
    }

    template<typename T, typename... Types>
    const T& get(const AutoTaggedUnion<Types...>& u) noexcept {
        assert_true(type_matches_tag<T>(u), "reading wrong type from AutoTaggedUnion");
        return *reinterpret_cast<const T*>(u.ptr);
    }

    template<typename... Ts>
    struct ExpandAutoTaggedUnionHelper;

    template<typename... Ts>
    struct ExpandAutoTaggedUnionHelper<TypeList<Ts...>> {
        using type = AutoTaggedUnion<Ts...>;
    };

    template<typename Ts>
    using ExpandAutoTaggedUnion = ExpandAutoTaggedUnionHelper<Ts>::type;

    // ========================================================================
    // Static types (indices [0, static_count)) are stored inline in `storage`.
    // Dynamic types (indices [static_count, total)) are heap-allocated via `ptr`.
    // Dynamic types are placed on the heap to break size-circularity (e.g. a
    // type that directly embeds the HybridTaggedUnion), not because they are large.
    // ========================================================================
    template<typename StaticList, typename DynamicList>
    struct HybridTaggedUnion;

    template<typename... STs, typename... DTs>
    struct HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>> {
        static constexpr size_t static_count  = sizeof...(STs);
        static constexpr size_t dynamic_count = sizeof...(DTs);
        static constexpr size_t invalid_index = static_cast<size_t>(-1);

        static constexpr size_t static_storage_size = []() constexpr {
            size_t m = 1;
            ((m = sizeof(STs) > m ? sizeof(STs) : m), ...);
            return m;
        }();

        static constexpr size_t static_storage_align = []() constexpr {
            size_t m = 1;
            ((m = alignof(STs) > m ? alignof(STs) : m), ...);
            return m;
        }();

        // Not constexpr: DTs may be incomplete at class instantiation time.
        // sizeof(DTs) is only evaluated when this function is called.
        static size_t dynamic_allocation_size() noexcept {
            size_t m = 1;
            ((m = sizeof(DTs) > m ? sizeof(DTs) : m), ...);
            return m;
        }

        size_t index = invalid_index;
        alignas(static_storage_align) unsigned char storage[static_storage_size];
        void* ptr = nullptr;

    private:
        template<size_t I>
        void destroy_at_index() noexcept {
            if constexpr (I < static_count) {
                using T = TypeAtIndex<I, STs...>;
                reinterpret_cast<T*>(&storage)->~T();
            } else {
                using T = TypeAtIndex<I - static_count, DTs...>;
                reinterpret_cast<T*>(ptr)->~T();
            }
        }

        template<size_t... Is>
        void destroy_impl(size_t idx, IndexSequence<Is...>) noexcept {
            ((idx == Is ? (destroy_at_index<Is>(), void()) : void()), ...);
        }

        void destroy() noexcept {
            if (index != invalid_index) {
                destroy_impl(index, IndexSequenceFor<STs..., DTs...>{});
                if (ptr != nullptr) { os::deallocate(ptr); ptr = nullptr; }
                index = invalid_index;
            }
        }

        template<size_t I>
        void copy_construct_at_index(const HybridTaggedUnion& other) {
            if constexpr (I < static_count) {
                using T = TypeAtIndex<I, STs...>;
                new (&storage) T(*reinterpret_cast<const T*>(&other.storage));
            } else {
                using T = TypeAtIndex<I - static_count, DTs...>;
                new (ptr) T(*reinterpret_cast<const T*>(other.ptr));
            }
        }

        template<size_t... Is>
        void copy_construct_impl(const HybridTaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_construct_at_index<Is>(other), void()) : void()), ...);
        }

        template<size_t I>
        void move_construct_at_index(HybridTaggedUnion&& other) {
            if constexpr (I < static_count) {
                using T = TypeAtIndex<I, STs...>;
                new (&storage) T(move(*reinterpret_cast<T*>(&other.storage)));
            }
            // Dynamic types are handled by ptr-stealing in the move constructor.
        }

        template<size_t... Is>
        void move_construct_impl(HybridTaggedUnion&& other, IndexSequence<Is...>) {
            ((other.index == Is ? (move_construct_at_index<Is>(move(other)), void()) : void()), ...);
        }

        template<size_t I>
        void copy_assign_at_index(const HybridTaggedUnion& other) {
            if constexpr (I < static_count) {
                using T = TypeAtIndex<I, STs...>;
                if (index == I) {
                    *reinterpret_cast<T*>(&storage) = *reinterpret_cast<const T*>(&other.storage);
                } else {
                    destroy_impl(index, IndexSequenceFor<STs..., DTs...>{});
                    if (ptr != nullptr) { os::deallocate(ptr); ptr = nullptr; }
                    new (&storage) T(*reinterpret_cast<const T*>(&other.storage));
                    index = I;
                }
            } else {
                using T = TypeAtIndex<I - static_count, DTs...>;
                if (index == I) {
                    *reinterpret_cast<T*>(ptr) = *reinterpret_cast<const T*>(other.ptr);
                } else {
                    destroy_impl(index, IndexSequenceFor<STs..., DTs...>{});
                    // Reuse ptr allocation if we already had a dynamic type (same total size).
                    if (ptr == nullptr) ptr = os::allocate(dynamic_allocation_size());
                    new (ptr) T(*reinterpret_cast<const T*>(other.ptr));
                    index = I;
                }
            }
        }

        template<size_t... Is>
        void copy_assign_impl(const HybridTaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_assign_at_index<Is>(other), void()) : void()), ...);
        }

        template<size_t I>
        void move_assign_at_index(HybridTaggedUnion&& other) {
            if constexpr (I < static_count) {
                using T = TypeAtIndex<I, STs...>;
                if (index == I) {
                    *reinterpret_cast<T*>(&storage) = move(*reinterpret_cast<T*>(&other.storage));
                } else {
                    destroy_impl(index, IndexSequenceFor<STs..., DTs...>{});
                    if (ptr != nullptr) { os::deallocate(ptr); ptr = nullptr; }
                    new (&storage) T(move(*reinterpret_cast<T*>(&other.storage)));
                    index = I;
                }
            } else {
                using T = TypeAtIndex<I - static_count, DTs...>;
                if (index == I) {
                    *reinterpret_cast<T*>(ptr) = move(*reinterpret_cast<T*>(other.ptr));
                } else {
                    // Steal the allocation; must invalidate other since its ptr becomes null.
                    destroy_impl(index, IndexSequenceFor<STs..., DTs...>{});
                    if (ptr != nullptr) { os::deallocate(ptr); ptr = nullptr; }
                    ptr = other.ptr;
                    other.ptr = nullptr;
                    other.index = invalid_index;
                    index = I;
                }
            }
        }

        template<size_t... Is>
        void move_assign_impl(HybridTaggedUnion&& other, IndexSequence<Is...>) {
            ((other.index == Is ? (move_assign_at_index<Is>(move(other)), void()) : void()), ...);
        }

    public:
        HybridTaggedUnion() = default;

        template<typename T>
            requires (SameAs<Decay<T>, STs> || ...)
        HybridTaggedUnion(T&& value) {
            using DT = Decay<T>;
            new (&storage) DT(forward<T>(value));
            index = TypeIndex<DT, STs..., DTs...>;
        }

        template<typename T>
            requires (SameAs<Decay<T>, DTs> || ...)
        HybridTaggedUnion(T&& value) {
            using DT = Decay<T>;
            ptr = os::allocate(dynamic_allocation_size());
            new (ptr) DT(forward<T>(value));
            index = TypeIndex<DT, STs..., DTs...>;
        }

        HybridTaggedUnion(const HybridTaggedUnion& other) {
            if (other.index != invalid_index) {
                if (other.index >= static_count)
                    ptr = os::allocate(dynamic_allocation_size());
                copy_construct_impl(other, IndexSequenceFor<STs..., DTs...>{});
                index = other.index;
            }
        }

        HybridTaggedUnion(HybridTaggedUnion&& other) noexcept {
            if (other.index != invalid_index) {
                index = other.index;
                if (other.index >= static_count) {
                    // Steal the heap allocation; invalidate other.
                    ptr = other.ptr;
                    other.ptr = nullptr;
                    other.index = invalid_index;
                } else {
                    // Move-construct into storage; leave other.index so other's dtor can run.
                    move_construct_impl(move(other), IndexSequenceFor<STs...>{});
                }
            }
        }

        ~HybridTaggedUnion() { destroy(); }

        HybridTaggedUnion& operator=(const HybridTaggedUnion& other) {
            if (this == &other) return *this;
            if (other.index == invalid_index) { destroy(); }
            else { copy_assign_impl(other, IndexSequenceFor<STs..., DTs...>{}); }
            return *this;
        }

        HybridTaggedUnion& operator=(HybridTaggedUnion&& other) noexcept {
            if (this == &other) return *this;
            destroy();
            if (other.index != invalid_index)
                move_assign_impl(move(other), IndexSequenceFor<STs..., DTs...>{});
            return *this;
        }

        template<typename T>
            requires ((SameAs<Decay<T>, STs> || ...) && !(SameAs<Decay<T>, DTs> || ...))
        HybridTaggedUnion& operator=(T&& value) {
            using DT = Decay<T>;
            constexpr size_t new_idx = TypeIndex<DT, STs..., DTs...>;
            if (index == new_idx) {
                *reinterpret_cast<DT*>(&storage) = forward<T>(value);
            } else {
                destroy();
                new (&storage) DT(forward<T>(value));
                index = new_idx;
            }
            return *this;
        }

        template<typename T>
            requires (!(SameAs<Decay<T>, STs> || ...) && (SameAs<Decay<T>, DTs> || ...))
        HybridTaggedUnion& operator=(T&& value) {
            using DT = Decay<T>;
            constexpr size_t new_idx = TypeIndex<DT, STs..., DTs...>;
            if (index == new_idx) {
                *reinterpret_cast<DT*>(ptr) = forward<T>(value);
            } else {
                destroy();
                ptr = os::allocate(dynamic_allocation_size());
                new (ptr) DT(forward<T>(value));
                index = new_idx;
            }
            return *this;
        }

        explicit operator bool() const noexcept { return index != invalid_index; }
    };

    template<typename T, typename... STs, typename... DTs>
    constexpr bool type_matches_tag(const HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>& u) noexcept {
        constexpr size_t idx = TypeIndex<Decay<T>, STs..., DTs...>;
        return u.index == idx;
    }

    template<typename T, typename... STs, typename... DTs>
    T& get(HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>& u) noexcept {
        assert_true(type_matches_tag<T>(u), "reading wrong type from HybridTaggedUnion");
        constexpr size_t idx = TypeIndex<Decay<T>, STs..., DTs...>;
        if constexpr (idx < sizeof...(STs)) {
            return *reinterpret_cast<T*>(&u.storage);
        } else {
            return *reinterpret_cast<T*>(u.ptr);
        }
    }

    template<typename T, typename... STs, typename... DTs>
    const T& get(const HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>& u) noexcept {
        assert_true(type_matches_tag<T>(u), "reading wrong type from HybridTaggedUnion");
        constexpr size_t idx = TypeIndex<Decay<T>, STs..., DTs...>;
        if constexpr (idx < sizeof...(STs)) {
            return *reinterpret_cast<const T*>(&u.storage);
        } else {
            return *reinterpret_cast<const T*>(u.ptr);
        }
    }

    template<typename StaticList, typename DynamicList>
    using ExpandHybridTaggedUnion = HybridTaggedUnion<StaticList, DynamicList>;
}

namespace plexdb {
    // ========================================================================
    // visit for HybridTaggedUnion — dispatch via function pointer table
    // ========================================================================
    template<size_t I, typename V, typename Return, typename HTU, typename AllList>
    struct HtuCase;

    template<size_t I, typename V, typename Return, typename HTU, typename... AllTypes>
    struct HtuCase<I, V, Return, HTU, TypeList<AllTypes...>> {
        static Return apply(HTU& u, V& vis) {
            using T = TypeAtIndex<I, AllTypes...>;
            return vis(get<T>(u));
        }
    };

    template<typename Visitor, typename... AllTypes, size_t... Is>
    decltype(auto) htu_visit_impl(auto& u, Visitor&& vis, TypeList<AllTypes...>, IndexSequence<Is...>) {
        using HTU = RemoveRef<decltype(u)>;
        using V   = RemoveRef<Visitor>;
        using ElemRef = Conditional<IsConst<HTU>, const TypeAtIndex<0, AllTypes...>&, TypeAtIndex<0, AllTypes...>&>;
        using Return  = decltype(declval<V&>()(declval<ElemRef>()));
        using Fn      = Return(*)(HTU&, V&);
        static constexpr Fn table[] = { &HtuCase<Is, V, Return, HTU, TypeList<AllTypes...>>::apply... };
        return table[u.index](u, vis);
    }

    export template<typename Visitor, typename... STs, typename... DTs>
    decltype(auto) visit(HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>& u, Visitor&& vis) {
        assert_true(u.index != HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>::invalid_index, "visiting empty HybridTaggedUnion");
        return htu_visit_impl(u, forward<Visitor>(vis), TypeList<STs..., DTs...>{}, IndexSequenceFor<STs..., DTs...>{});
    }

    export template<typename Visitor, typename... STs, typename... DTs>
    decltype(auto) visit(const HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>& u, Visitor&& vis) {
        assert_true(u.index != HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>::invalid_index, "visiting empty HybridTaggedUnion");
        return htu_visit_impl(u, forward<Visitor>(vis), TypeList<STs..., DTs...>{}, IndexSequenceFor<STs..., DTs...>{});
    }

    export template<typename... STs, typename... DTs>
    bool operator==(const HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>& a,
                    const HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>& b) {
        if (a.index != b.index) return false;
        if (a.index == HybridTaggedUnion<TypeList<STs...>, TypeList<DTs...>>::invalid_index) return true;
        return visit(a, [&b](const auto& av) -> bool {
            using T = RemoveCVRef<decltype(av)>;
            return av == get<T>(b);
        });
    }
}

namespace plexdb {
    // ========================================================================
    // visit for AutoTaggedUnion
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
    decltype(auto) visit(AutoTaggedUnion<Types...>& u, Visitor&& vis) {
        assert_true(u.ptr != nullptr, "visiting empty AutoTaggedUnion");
        return dtu_visit_impl<Visitor, Types...>(u, forward<Visitor>(vis), IndexSequenceFor<Types...>{});
    }

    export template<typename Visitor, typename... Types>
    decltype(auto) visit(const AutoTaggedUnion<Types...>& u, Visitor&& vis) {
        assert_true(u.ptr != nullptr, "visiting empty AutoTaggedUnion");
        return dtu_visit_impl<Visitor, Types...>(u, forward<Visitor>(vis), IndexSequenceFor<Types...>{});
    }
}
