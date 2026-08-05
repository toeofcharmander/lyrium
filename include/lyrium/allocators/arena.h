#pragma once

#include <cstddef>

#include <new>

namespace lyrium
{

// A fixed contiguous region, carved up internally, that the process sees as one
// reservation for its entire lifetime.
//
// This is containment rather than compaction. The address space cannot be
// defragmented -- in a 32-bit process the address is the handle, and moving an
// allocation would mean rewriting every pointer its owner holds, which is
// impossible for memory the D3D9 runtime hands to the display driver. What can
// be done is to stop the churn cutting up the *shared* address space.
//
// The engine already proves the shape works: it reserves one ~850 MB block at
// startup and sub-allocates inside it, so however finely that fragments,
// Windows sees a single entry and no other allocation is affected. The runtime's
// managed texture duplicates are the large unpooled churn left over, and each
// one above the NT heap's 508 KB threshold becomes its own separate reservation
// -- which is why a live session showed 101.6 MB free below the 2 GB line split
// across 339 blocks averaging 300 KB, with the largest at 9.2 MB.
//
// Inside here that fragmentation still happens, and it heals: every free
// coalesces with its neighbours, so a churn of varying sizes returns to one
// large block instead of accumulating slivers. That is what the surrounding
// address space cannot do for itself, and it is invisible from outside either
// way.
//
// Ownership is decidable in two comparisons because the region is contiguous and
// exclusively ours. That is the property that makes it safe to interpose on
// another module's frees: a pointer is ours or it is not, with no false
// positives, so a foreign pointer can always be passed through untouched.
class Arena
{
  public:
    // Deliberately takes memory rather than acquiring it, so the reservation
    // strategy is the caller's and this stays testable off Windows.
    Arena(std::byte *base, std::size_t size)
        : base_{base}
        , size_{size}
    {
        if (base_ != nullptr && size_ >= sizeof(Block))
        {
            head_ = ::new (static_cast<void *>(base_))
                Block{.size = size_ - sizeof(Block), .free = true, .next = nullptr, .prev = nullptr};
        }
    }

    Arena(const Arena &) = delete;
    auto operator=(const Arena &) -> Arena & = delete;

    [[nodiscard]] auto owns(const void *allocation) const -> bool
    {
        if (allocation == nullptr || base_ == nullptr)
        {
            return false;
        }
        const auto *address = static_cast<const std::byte *>(allocation);
        return address >= base_ && address < base_ + size_;
    }

    // Returns nullptr when the request cannot be served. The caller is expected
    // to fall back to the real allocator, so a full arena degrades to the
    // behaviour that existed before it rather than failing anything.
    [[nodiscard]] auto allocate(std::size_t bytes) -> void *
    {
        if (bytes == 0u || head_ == nullptr)
        {
            return nullptr;
        }

        const auto wanted = aligned(bytes);
        for (auto *block = head_; block != nullptr; block = block->next)
        {
            if (!block->free || block->size < wanted)
            {
                continue;
            }

            // Split only when the remainder can hold a header and something
            // worth allocating; otherwise hand over the slack rather than
            // creating a block nothing can ever use.
            if (block->size >= wanted + sizeof(Block) + alignment)
            {
                auto *rest = ::new (static_cast<void *>(payload_of(block) + wanted)) Block{
                    .size = block->size - wanted - sizeof(Block), .free = true, .next = block->next, .prev = block};
                if (rest->next != nullptr)
                {
                    rest->next->prev = rest;
                }
                block->next = rest;
                block->size = wanted;
            }

            block->free = false;
            ++live_;
            return payload_of(block);
        }
        return nullptr;
    }

    // False means the pointer is not ours and the caller must pass it to the
    // real deallocator. Never guesses: a foreign pointer is refused, not freed.
    auto deallocate(void *allocation) -> bool
    {
        if (!owns(allocation))
        {
            return false;
        }

        auto *block = block_of(static_cast<std::byte *>(allocation));
        if (block->free)
        {
            return true;
        }
        block->free = true;
        --live_;

        coalesce(block->prev != nullptr && block->prev->free ? block->prev : block);
        return true;
    }

    [[nodiscard]] auto live_allocations() const -> std::size_t
    {
        return live_;
    }

    [[nodiscard]] auto capacity() const -> std::size_t
    {
        return size_;
    }

    // The largest request the arena could serve right now. The same number the
    // address space reports as its largest free block, for the same reason.
    [[nodiscard]] auto largest_free() const -> std::size_t
    {
        auto largest = std::size_t{0};
        for (const auto *block = head_; block != nullptr; block = block->next)
        {
            if (block->free && block->size > largest)
            {
                largest = block->size;
            }
        }
        return largest;
    }

  private:
    static constexpr auto alignment = alignof(std::max_align_t);

    struct Block
    {
        std::size_t size;
        bool free;
        Block *next;
        Block *prev;
    };

    static constexpr auto aligned(std::size_t bytes) -> std::size_t
    {
        return (bytes + alignment - 1u) & ~(alignment - 1u);
    }

    static auto payload_of(Block *block) -> std::byte *
    {
        return reinterpret_cast<std::byte *>(block) + sizeof(Block);
    }

    static auto block_of(std::byte *payload) -> Block *
    {
        return reinterpret_cast<Block *>(payload - sizeof(Block));
    }

    // Merges forward from the earliest free neighbour, so a free between two
    // free blocks becomes one block rather than three. This is the healing the
    // surrounding address space cannot perform on itself.
    static auto coalesce(Block *block) -> void
    {
        while (block->free && block->next != nullptr && block->next->free)
        {
            auto *victim = block->next;
            block->size += sizeof(Block) + victim->size;
            block->next = victim->next;
            if (block->next != nullptr)
            {
                block->next->prev = block;
            }
        }
    }

    std::byte *base_;
    std::size_t size_;
    Block *head_{nullptr};
    std::size_t live_{0};
};

}
