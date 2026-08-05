#pragma once

#include <atomic>
#include <cstdint>

namespace lyrium::diag
{

// How many calls, how many bytes, and the largest single one.
//
// Three numbers because two of them are not enough. A total alone cannot tell a
// thousand small calls from one large one, and that distinction is the entire
// question here: the MANAGED texture duplicates are 140 allocations averaging
// 1.8 MB, and what makes them fragment the address space is their individual
// size, not their sum.
//
// The count matters on its own. When the heap arena served zero allocations, the
// number that proved the hook was installed and being reached -- rather than
// silently not firing -- was its call count. A tally with a count and no bytes
// is a working probe watching the wrong function; a tally with nothing at all is
// a probe that never ran.
//
// Relaxed ordering: these sit on an allocation path and are read every few
// seconds by the sampler thread for a log line. A count that trails by a few
// nanoseconds costs nothing; ordering every call would not.
class SizeTally
{
  public:
    auto note(std::uint64_t bytes) -> void
    {
        count_.fetch_add(1u, std::memory_order_relaxed);
        bytes_.fetch_add(bytes, std::memory_order_relaxed);

        auto seen = largest_.load(std::memory_order_relaxed);
        while (bytes > seen && !largest_.compare_exchange_weak(seen, bytes, std::memory_order_relaxed))
        {
        }
    }

    [[nodiscard]] auto count() const -> std::uint64_t
    {
        return count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] auto bytes() const -> std::uint64_t
    {
        return bytes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] auto largest() const -> std::uint64_t
    {
        return largest_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> count_{};
    std::atomic<std::uint64_t> bytes_{};
    std::atomic<std::uint64_t> largest_{};
};

}
