#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

#include "lyrium/allocators/global_allocator.h"

namespace lyrium
{

template <class T>
class STDAllocator
{
  public:
    using value_type = T;
    using pointer = value_type *;
    using is_always_equal = std::true_type;

    STDAllocator() = default;

    template <class U>
    constexpr STDAllocator(const STDAllocator<U> &)
    {
    }

    [[nodiscard]] auto allocate(std::size_t n) -> T *
    {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
        {
            throw std::bad_array_new_length{};
        }
        return static_cast<T *>(GlobalAllocator::allocate(n * sizeof(T)));
    }

    auto deallocate(T *p, std::size_t) -> void
    {
        GlobalAllocator::deallocate(p);
    }
};

template <class T, class U>
auto operator==(const STDAllocator<T> &, const STDAllocator<U> &) -> bool
{
    return true;
};

template <class T, class U>
auto operator!=(const STDAllocator<T> &, const STDAllocator<U> &) -> bool
{
    return false;
};
}
