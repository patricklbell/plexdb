export module plexdb.variant;

import plexdb.base;

export namespace plexdb {
    template<typename... Types>
    struct TaggedUnion {
        static constexpr size_t invalid_index = static_cast<size_t>(-1);

        size_t index = invalid_index;
        alignas(Types...) unsigned char storage[max(sizeof(Types)...)];

    private:
        // ====================================================================
        // destructor
        // ====================================================================
        template <size_t I>
        void destroy_at_index() noexcept {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                reinterpret_cast<T*>(&storage)->~T();
            }
        }
        
        template <size_t... Is>
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
        template <size_t I>
        void copy_construct_at_index(const TaggedUnion& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                new (&storage) T(*reinterpret_cast<const T*>(&other.storage));
            }
        }
        
        template <size_t... Is>
        void copy_construct_impl(const TaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_construct_at_index<Is>(other), void()) : void()), ...);
        }
        
        // ====================================================================
        // move constructor
        // ====================================================================
        template <size_t I>
        void move_construct_at_index(TaggedUnion&& other) {
            if constexpr (I < sizeof...(Types)) {
                using T = TypeAtIndex<I, Types...>;
                new (&storage) T(move(*reinterpret_cast<T*>(&other.storage)));
            }
        }
        
        template <size_t... Is>
        void move_construct_impl(TaggedUnion&& other, IndexSequence<Is...>) {
            ((other.index == Is ? (move_construct_at_index<Is>(move(other)), void()) : void()), ...);
        }
        
        // ====================================================================
        // copy assignment
        // ====================================================================
        template <size_t I>
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
        
        template <size_t... Is>
        void copy_assign_impl(const TaggedUnion& other, IndexSequence<Is...>) {
            ((other.index == Is ? (copy_assign_at_index<Is>(other), void()) : void()), ...);
        }
        
        // ====================================================================
        // move assignment
        // ====================================================================
        template <size_t I>
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
        
        template <size_t... Is>
        void move_assign_impl(TaggedUnion&& other, IndexSequence<Is...>) {
            ((other.index == Is ? (move_assign_at_index<Is>(move(other)), void()) : void()), ...);
        }
        
    public:
        // ====================================================================
        // constructors
        // ====================================================================
        TaggedUnion() = default;

        template<typename T>
            requires (SameAs<Decay<T>, Types> || ...)
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
            requires (SameAs<Decay<T>, Types> || ...)
        TaggedUnion& operator=(T&& value) {
            using DecayedT = Decay<T>;
            constexpr size_t new_index = TypeIndex<T, Types...>;
            
            if (index == new_index) {
                *reinterpret_cast<DecayedT*>(&storage) = forward<T>(value);
            } else {
                destroy();
                new (&storage) DecayedT(forward<T>(value));
                index = new_index;
            }
            return *this;
        }
    };

    template<typename... Types, typename T>
    constexpr bool type_matches_tag(const TaggedUnion<Types...>& u) noexcept {
        return u.index == TypeIndex<T, Types...>;
    }

    template<typename... Types, typename T>
    T& get(TaggedUnion<Types...>& u) noexcept {
        return *reinterpret_cast<T*>(&u.storage);
    }
    
    template<typename... Types, typename T>
    const T& get(const TaggedUnion<Types...>& u) noexcept {
        return *reinterpret_cast<const T*>(&u.storage);
    }
}