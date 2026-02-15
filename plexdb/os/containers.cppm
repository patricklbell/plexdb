// @todo benchmark
export module plexdb.os.containers;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb {
    template <typename T, typename Size = U64>
    struct DynamicArray;

    template <typename T, typename Size = U64>
    void clear(struct DynamicArray<T, Size>& arr);

    template <typename T, typename Size>
    void reserve(DynamicArray<T,Size>& arr, Size new_capacity);

    // @todo move construct vs zero initialize
    template <typename T, typename Size=U64>
    struct DynamicArray {
        T*  ptr      = nullptr;
        Size length   = 0;
        Size capacity = 0;

        DynamicArray() = default;

        DynamicArray(const DynamicArray& other) : ptr(nullptr), length(0), capacity(0) {
            if (other.length > 0) {
                reserve(*this, other.length);
                for (Size i = 0; i < other.length; ++i) {
                    new (ptr + i) T(other.ptr[i]);
                }
                length = other.length;
            }
        }

        DynamicArray(DynamicArray&& other) noexcept : ptr(other.ptr), length(other.length), capacity(other.capacity) {
            other.ptr = nullptr;
            other.length = 0;
            other.capacity = 0;
        }

        DynamicArray& operator=(const DynamicArray& other) {
            if (this != &other) {
                clear(*this);
                reserve(*this, other.length);
                for (Size i = 0; i < other.length; ++i) {
                    new (ptr + i) T(other.ptr[i]);
                }
                length = other.length;
            }
            return *this;
        }

        DynamicArray& operator=(DynamicArray&& other) noexcept {
            if (this != &other) {
                clear(*this);
                if (ptr) {
                    os::deallocate(ptr);
                }

                ptr = other.ptr;
                length = other.length;
                capacity = other.capacity;

                other.ptr = nullptr;
                other.length = 0;
                other.capacity = 0;
            }
            return *this;
        }

        ~DynamicArray() {
            clear(*this);
            if (ptr) {
                os::deallocate(ptr);
                ptr = nullptr;
                capacity = 0;
            }
        }

        T* begin() noexcept { return ptr; }
        T* end()   noexcept { return ptr + length; }

        const T* begin() const noexcept { return ptr; }
        const T* end()   const noexcept { return ptr + length; }

        const T* cbegin() const noexcept { return ptr; }
        const T* cend()   const noexcept { return ptr + length; }

        T& operator[](Size index) noexcept {
            return ptr[index];
        }

        const T& operator[](Size index) const noexcept {
            return ptr[index];
        }
    };

    template <typename T, typename Size>
    void reserve(DynamicArray<T,Size>& arr, Size new_capacity) {
        if (new_capacity <= arr.capacity)
            return;

        Size bytes = new_capacity * sizeof(T);
        T* new_ptr = reinterpret_cast<T*>(os::allocate(bytes));

        // move-construct into new memory
        for (Size i = 0; i < arr.length; ++i) {
            new (new_ptr + i) T(move(arr.ptr[i]));
            arr.ptr[i].~T();
        }

        os::deallocate(arr.ptr);

        arr.ptr = new_ptr;
        arr.capacity = new_capacity;
    }

    template <typename T, typename Size>
    void push_back(DynamicArray<T,Size>& arr, const T& value) {
        if (arr.length == arr.capacity) {
            Size new_capacity = arr.capacity == 0 ? 4 : arr.capacity * 2;
            reserve(arr, new_capacity);
        }

        new (arr.ptr + arr.length) T(value);
        ++arr.length;
    }

    template <typename T, typename Size>
    void push_back(DynamicArray<T,Size>& arr, T&& value) {
        if (arr.length == arr.capacity) {
            Size new_capacity = arr.capacity == 0 ? 4 : arr.capacity * 2;
            reserve(arr, new_capacity);
        }

        new (arr.ptr + arr.length) T(move(value));
        ++arr.length;
    }

    template <typename T, typename Size>
    void pop_back(DynamicArray<T,Size>& arr) {
        if (arr.length == 0)
            return;

        --arr.length;
        arr.ptr[arr.length].~T();
    }

    template <typename T, typename Size>
    void clear(DynamicArray<T,Size>& arr) {
        for (Size i = 0; i < arr.length; ++i)
            arr.ptr[i].~T();

        arr.length = 0;
    }
}