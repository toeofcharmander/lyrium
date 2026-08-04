#pragma once

#include <atomic>
#include <cstdint>

namespace lyrium::com
{

// A COM reference count that cannot be resurrected.
//
// The problem this solves. An object is kept in a registry so a device reset can
// walk every live instance. The walk takes a reference on each so the object
// cannot die mid-walk. But the object only removes itself from the registry in
// its destructor, which runs *after* Release has already observed the count
// reach zero and committed to deleting it. In that window the walk finds a
// pointer that is still in the registry, calls AddRef on a count of zero, takes
// it back to one, and carries on using an object that is being destroyed.
//
// A plain fetch_add cannot detect this: it succeeds from zero exactly as
// happily as from one. try_add_ref refuses to lift a count off zero, so once an
// object has begun dying it can never be brought back, and a walk that arrives
// late simply skips it.
//
// Kept free of COM and Windows types so the logic can be tested directly. The
// class that uses it is a D3D vtable and can never compile off-target.
class RefCount
{
  public:
    explicit RefCount(std::uint32_t initial = 1u)
        : count_{initial}
    {
    }

    RefCount(const RefCount &) = delete;
    auto operator=(const RefCount &) -> RefCount & = delete;

    auto add_ref() -> std::uint32_t
    {
        return count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    // Returns the remaining count. Zero means the caller owns the object's
    // destruction and no other thread can take a reference from here on.
    [[nodiscard]] auto release() -> std::uint32_t
    {
        return count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    }

    // Takes a reference only if one already exists. False means the object is
    // already dying and must not be touched.
    [[nodiscard]] auto try_add_ref() -> bool
    {
        auto current = count_.load(std::memory_order_acquire);
        while (current != 0u)
        {
            if (count_.compare_exchange_weak(
                    current, current + 1u, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto current() const -> std::uint32_t
    {
        return count_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<std::uint32_t> count_;
};

}
