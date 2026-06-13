export module plexdb.tagged_union;

import plexdb.base;

export namespace plexdb {
    template<typename... Types>
    struct TaggedUnion {
        static constexpr IndexSequenceFor<Types...> is{};

        static constexpr size_t invalid_index = static_cast<size_t>(-1);

        size_t index = invalid_index;
        alignas(Types...) unsigned char storage[max(sizeof(Types)...)];

    private:
        // ====================================================================
        // destructor
        // ====================================================================
        template<size_t I>
        void destroy_at_index() noexcept {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                reinterpret_cast<T*>(&storage)->~T();
            }
        }

        template<size_t... Is>
        void destroy_impl(size_t idx, IndexSequence<Is...>) noexcept {
            ((idx == Is ? (destroy_at_index<Is>(), void()) : void()), ...);
        }

        void destroy() noexcept {
            if (index != invalid_index) {
                destroy_impl(index, IndexSequenceFor<Types...>{});
                index = invalid_index;
            }
        }

        // ====================================================================
        // copy constructor
        // ====================================================================
        template<size_t I>
        void copy_construct_at_index(const TaggedUnion& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                new (&storage) T(*reinterpret_cast<const T*>(&other.storage));
            }
        }

        template<size_t... Is>
        void copy_construct_impl(const TaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_construct_at_index<Is>(other), void()) : void()), ...);
        }

        // ====================================================================
        // move constructor
        // ====================================================================
        template<size_t I>
        void move_construct_at_index(TaggedUnion&& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                new (&storage) T(move(*reinterpret_cast<T*>(&other.storage)));
            }
        }

        template<size_t... Is>
        void move_construct_impl(TaggedUnion&& other, IndexSequence<Is...>) {
            ((other.index == Is ? (move_construct_at_index<Is>(move(other)), void()) : void()), ...);
        }

        // ====================================================================
        // copy assignment
        // ====================================================================
        template<size_t I>
        void copy_assign_at_index(const TaggedUnion& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                if (index == I) {
                    *reinterpret_cast<T*>(&storage) = *reinterpret_cast<const T*>(&other.storage);
                } else {
                    destroy();
                    new (&storage) T(*reinterpret_cast<const T*>(&other.storage));
                    index = I;
                }
            }
        }

        template<size_t... Is>
        void copy_assign_impl(const TaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_assign_at_index<Is>(other), void()) : void()), ...);
        }

        // ====================================================================
        // move assignment
        // ====================================================================
        template<size_t I>
        void move_assign_at_index(TaggedUnion&& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                if (index == I) {
                    *reinterpret_cast<T*>(&storage) = move(*reinterpret_cast<T*>(&other.storage));
                } else {
                    destroy();
                    new (&storage) T(move(*reinterpret_cast<T*>(&other.storage)));
                    index = I;
                }
            }
        }

        template<size_t... Is>
        void move_assign_impl(TaggedUnion&& other, IndexSequence<Is...>) {
            ((other.index == Is ? (move_assign_at_index<Is>(move(other)), void()) : void()), ...);
        }

    public:
        // ====================================================================
        // constructors
        // ====================================================================
        TaggedUnion() = default;

        template<typename T>
            requires(SameAs<Decay<T>, Types> || ...)
        constexpr TaggedUnion(T&& value) noexcept(NoThrowMoveConstructible<T>) {
            using DecayedT = Decay<T>;

            new (&storage) DecayedT(forward<T>(value));
            index = TypeIndex<DecayedT, Types...>;
        }

        TaggedUnion(const TaggedUnion& other) {
            if (other.index != invalid_index) {
                copy_construct_impl(other, IndexSequenceFor<Types...>{});
                index = other.index;
            }
        }

        TaggedUnion(TaggedUnion&& other) noexcept(AllNoThrowMoveConstructible<Types...>) {
            if (other.index != invalid_index) {
                move_construct_impl(move(other), IndexSequenceFor<Types...>{});
                index = other.index;
            }
        }

        // ====================================================================
        // destructor
        // ====================================================================
        ~TaggedUnion() {
            destroy();
        }

        // ====================================================================
        // assignment
        // ====================================================================
        TaggedUnion& operator=(const TaggedUnion& other) {
            if (this != &other) {
                if (other.index == invalid_index) {
                    destroy();
                } else {
                    copy_assign_impl(other, IndexSequenceFor<Types...>{});
                }
            }
            return *this;
        }

        TaggedUnion& operator=(TaggedUnion&& other) noexcept(AllNoThrowMoveAssignable<Types...> && AllNoThrowMoveConstructible<Types...>) {
            if (this != &other) {
                if (other.index == invalid_index) {
                    destroy();
                } else {
                    move_assign_impl(move(other), IndexSequenceFor<Types...>{});
                }
            }
            return *this;
        }

        // Assign from one of the types
        template<typename T>
            requires(SameAs<Decay<T>, Types> || ...)
        TaggedUnion& operator=(T&& value) {
            using DecayedT             = Decay<T>;
            constexpr size_t new_index = TypeIndex<DecayedT, Types...>;

            if (index == new_index) {
                *reinterpret_cast<DecayedT*>(&storage) = forward<T>(value);
            } else {
                destroy();
                new (&storage) DecayedT(forward<T>(value));
                index = new_index;
            }
            return *this;
        }

        explicit operator bool() const {
            return this->index != invalid_index;
        }
    };

    template<typename T, typename... Types>
    constexpr bool type_matches_tag(const TaggedUnion<Types...>& u) noexcept {
        using DecayedT = Decay<T>;
        size_t idx     = TypeIndex<DecayedT, Types...>;
        return u.index == idx;
    }

    template<typename T, typename... Types>
    T& get(TaggedUnion<Types...>& u) noexcept {
        assert_true(type_matches_tag<T>(u), "reading wrong type from tagged union");
        return *reinterpret_cast<T*>(&u.storage);
    }

    template<typename T, typename... Types>
    const T& get(const TaggedUnion<Types...>& u) noexcept {
        assert_true(type_matches_tag<T>(u), "reading wrong type from tagged union");
        return *reinterpret_cast<const T*>(&u.storage);
    }

    template<typename... Ts>
    struct ExpandTaggedUnionHelper;

    template<typename... Ts>
    struct ExpandTaggedUnionHelper<TypeList<Ts...>> {
        using type = TaggedUnion<Ts...>;
    };

    template<typename Ts>
    using ExpandTaggedUnion = ExpandTaggedUnionHelper<Ts>::type;
}

namespace plexdb {
    template<size_t I, typename V, typename Return, typename TU, typename... Types>
    struct Case {
        static Return apply(TU& u, V& vis) {
            using T = TypeAtIndex<I, Types...>;
            return vis(get<T>(u));
        }
    };

    template<typename Visitor, typename... Types, size_t... Is>
    decltype(auto) visit_impl(auto& u, Visitor&& vis, IndexSequence<Is...>) {
        using TU = RemoveRef<decltype(u)>;
        using V  = RemoveRef<Visitor>;

        using ElemRef = Conditional<IsConst<TU>, const TypeAtIndex<0, Types...>&, TypeAtIndex<0, Types...>&>;
        using Return  = decltype(declval<V&>()(declval<ElemRef>()));
        using Fn      = Return (*)(TU&, V&);

        static constexpr Fn table[] = {&Case<Is, V, Return, TU, Types...>::apply...};

        return table[u.index](u, vis);
    }

    export template<typename Visitor, typename... Types>
    decltype(auto) visit(TaggedUnion<Types...>& u, Visitor&& vis) {
        assert_true(u.index != TaggedUnion<Types...>::invalid_index, "visiting empty TaggedUnion");
        return visit_impl<Visitor, Types...>(u, forward<Visitor>(vis), IndexSequenceFor<Types...>{});
    }

    export template<typename Visitor, typename... Types>
    decltype(auto) visit(const TaggedUnion<Types...>& u, Visitor&& vis) {
        assert_true(u.index != TaggedUnion<Types...>::invalid_index, "visiting empty TaggedUnion");
        return visit_impl<Visitor, Types...>(u, forward<Visitor>(vis), IndexSequenceFor<Types...>{});
    }

    export template<typename... Types>
    bool operator==(const TaggedUnion<Types...>& a, const TaggedUnion<Types...>& b) {
        if (a.index != b.index) {
            return false;
        }
        if (a.index == TaggedUnion<Types...>::invalid_index) {
            return true;
        }
        return visit(a, [&b](const auto& av) -> bool {
            using T = RemoveCVRef<decltype(av)>;
            return av == get<T>(b);
        });
    }
}