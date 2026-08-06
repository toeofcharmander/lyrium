#pragma once

#include <bit>
#include <cstdint>

// A log2 histogram of allocation sizes, which is what decides the side pool's
// threshold and its size.
//
// Both figures have to come from a session rather than from a guess. Too high a
// threshold leaves the 8-64 MB class still carving Main Pool and the session still
// dies; too low and the arena fills with the small-block population that is doing
// the pinning, at which point sizing it means holding most of the pool. The
// cumulative `bytes_at_or_above` answers "how many bytes would a threshold of N
// have diverted", which is exactly the arena budget.
//
// Buckets are powers of two, so a threshold between them is answered as the bucket
// below it. That over-reports rather than under-reports, which is the safe
// direction when the answer is used to size an arena.

namespace lyrium::dao
{

struct SizeHistogram
{
    // 32 covers every std::uint32_t size, and the largest request measured so far
    // is 71.6 MB, which lands in bucket 26.
    static constexpr auto bucket_count = std::size_t{32};

    std::uint64_t counts[bucket_count];
    std::uint64_t bytes[bucket_count];
};

// Index of the highest set bit: bucket i holds sizes in [2^i, 2^(i+1)), so the
// index reads directly as "at least 2^i bytes". Zero is folded into bucket 0
// rather than left undefined -- the engine's allocator returns null for a zero-size
// request without consulting the pool, and those still reach here.
[[nodiscard]] constexpr auto size_bucket(std::uint32_t size) -> std::size_t
{
    return size < 2u ? 0u : static_cast<std::size_t>(31 - std::countl_zero(size));
}

constexpr auto note_size(SizeHistogram &histogram, std::uint32_t size) -> void
{
    const auto bucket = size_bucket(size);
    ++histogram.counts[bucket];
    histogram.bytes[bucket] += size;
}

// Total bytes recorded in every bucket whose lower bound is at or above the
// threshold. A threshold that is not a power of two rounds down to the bucket
// containing it, so the answer includes some allocations smaller than asked for.
[[nodiscard]] constexpr auto bytes_at_or_above(const SizeHistogram &histogram, std::uint64_t threshold)
    -> std::uint64_t
{
    const auto first = threshold == 0u ? std::size_t{0} : size_bucket(static_cast<std::uint32_t>(threshold));
    auto total = std::uint64_t{0};
    for (auto bucket = first; bucket < SizeHistogram::bucket_count; ++bucket)
    {
        total += histogram.bytes[bucket];
    }
    return total;
}

}
