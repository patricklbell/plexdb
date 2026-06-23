export module plexdb.os.function;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb {
    template<typename Signature>
    class AutoFunctor; // forward declaration

    template<typename Ret, typename... Args>
    class AutoFunctor<Ret(Args...)> {
    private:
        struct Vtable {
            Ret (*invoke)(void* data, Args... args);
            void (*destroy)(void* data);
        };

        void*   data   = nullptr;
        Vtable* vtable = nullptr;

        template<typename F>
        static Ret invoke_impl(void* data, Args... args) {
            return (*static_cast<F*>(data))(forward<Args>(args)...);
        }

        template<typename F>
        static void destroy_impl(void* data) {
            static_cast<F*>(data)->~F();
        }

        template<typename F>
        static Vtable* get_vtable() {
            static Vtable vt = {
                .invoke  = &invoke_impl<F>,
                .destroy = &destroy_impl<F>
            };
            return &vt;
        }

    public:
        AutoFunctor() noexcept = default;

        template<typename F>
            requires(!SameAs<RemoveCVRef<F>, AutoFunctor>)
        AutoFunctor(F&& f) {
            using FType = RemoveCVRef<F>;

            this->data = os::allocate(sizeof(FType));
            assert_true(this->data != nullptr, "allocation failed");

            new (this->data) FType(forward<F>(f));
            this->vtable = get_vtable<FType>();
        }

        ~AutoFunctor() {
            reset();
        }

        AutoFunctor(const AutoFunctor&)            = delete;
        AutoFunctor& operator=(const AutoFunctor&) = delete;

        AutoFunctor(AutoFunctor&& other) noexcept
            : data(other.data)
            , vtable(other.vtable) {
            other.data   = nullptr;
            other.vtable = nullptr;
        }

        AutoFunctor& operator=(AutoFunctor&& other) noexcept {
            if (this != &other) {
                reset();
                this->data   = other.data;
                this->vtable = other.vtable;
                other.data   = nullptr;
                other.vtable = nullptr;
            }
            return *this;
        }

        Ret operator()(Args... args) {
            assert_true(this->data != nullptr && this->vtable != nullptr, "called null function");
            if constexpr (SameAs<Ret, void>) {
                if (!this->data || !this->vtable) {
                    return;
                }
            }
            return this->vtable->invoke(this->data, forward<Args>(args)...);
        }

        Ret operator()(Args... args) const {
            assert_true(this->data != nullptr && this->vtable != nullptr, "called null function");
            if constexpr (SameAs<Ret, void>) {
                if (!this->data || !this->vtable) {
                    return;
                }
            }
            return this->vtable->invoke(const_cast<void*>(this->data), forward<Args>(args)...);
        }

        explicit operator bool() const noexcept {
            return this->data != nullptr;
        }

    private:
        void reset() noexcept {
            if (this->data != nullptr && this->vtable != nullptr) {
                this->vtable->destroy(this->data);
                os::deallocate(this->data);
                this->data   = nullptr;
                this->vtable = nullptr;
            }
        }
    };
}
