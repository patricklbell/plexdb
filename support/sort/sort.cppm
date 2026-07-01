module;
#include <cpp-sort/sorters/insertion_sorter.h>
#include <cpp-sort/sorters/pdq_sorter.h>
#include <cpp-sort/sorters/tim_sorter.h>

export module plexdb.support.sort;

import plexdb.base;

export namespace plexdb::support::sort {
    template<typename T, typename L>
    void sort(TArrayView<T, L> view) {
        ::cppsort::pdq_sort(view.ptr, view.ptr + view.length);
    }

    template<typename T, typename L, typename Less>
    void sort(TArrayView<T, L> view, Less less) {
        ::cppsort::pdq_sort(view.ptr, view.ptr + view.length, less);
    }

    template<typename T, typename L>
    void stable_sort(TArrayView<T, L> view) {
        ::cppsort::tim_sort(view.ptr, view.ptr + view.length);
    }

    template<typename T, typename L, typename Less>
    void stable_sort(TArrayView<T, L> view, Less less) {
        ::cppsort::tim_sort(view.ptr, view.ptr + view.length, less);
    }

    template<typename T, typename L>
    void small_sort(TArrayView<T, L> view) {
        ::cppsort::insertion_sort(view.ptr, view.ptr + view.length);
    }

    template<typename T, typename L, typename Less>
    void small_sort(TArrayView<T, L> view, Less less) {
        ::cppsort::insertion_sort(view.ptr, view.ptr + view.length, less);
    }
}
