export module plexdb.base.functor;

import plexdb.base.types;

export namespace plexdb {
    // @note non-owning
    template<typename Ret, typename... Args>
    struct Functor {
        Ret (*fn)(void*, Args...) = nullptr;
        void* ctx                 = nullptr;

        Ret operator()(Args... args) const {
            return fn(ctx, args...);
        }
        explicit operator bool() const {
            return fn != nullptr;
        }
    };

    template<typename Ret, typename... Args, typename F>
    Functor<Ret, Args...> to_functor(F& f) {
        return {
            [](void* ctx, Args... args) -> Ret {
                return (*static_cast<F*>(ctx))(args...);
            },
            &f};
    }
}
