#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

namespace lyrium::test
{

// A page source for FreeListAllocator that records what it hands out and what it
// is asked to release, without actually releasing anything. Recording rather than
// freeing is deliberate: it lets a test observe a double release as a failed
// assertion instead of a crash inside the allocator under test.
//
// State is held externally and shared by every copy, because copying a
// FreeListAllocator copies its page source too, and the point of some tests is
// exactly what happens when that occurs.
class PageLedger
{
  public:
    ~PageLedger()
    {
        for (auto *page : live_)
        {
            std::free(page);
        }
    }

    PageLedger() = default;
    PageLedger(const PageLedger &) = delete;
    auto operator=(const PageLedger &) -> PageLedger & = delete;

    auto note_allocated(void *page) -> void
    {
        live_.push_back(page);
        ++allocations_;
    }

    auto note_released(void *page) -> void
    {
        releases_.push_back(page);
    }

    [[nodiscard]] auto allocations() const -> std::size_t
    {
        return allocations_;
    }

    [[nodiscard]] auto releases() const -> std::size_t
    {
        return releases_.size();
    }

    [[nodiscard]] auto releases_of(const void *page) const -> std::size_t
    {
        std::size_t count = 0u;
        for (const auto *released : releases_)
        {
            if (released == page)
            {
                ++count;
            }
        }
        return count;
    }

  private:
    std::vector<void *> live_{};
    std::vector<void *> releases_{};
    std::size_t allocations_{};
};

// Over-allocates deliberately. A test that constructs FreeListAllocator with a
// capacity smaller than sizeof(Node) is probing an arithmetic bug in the
// constructor, and must not also trigger a genuine heap overflow when the
// constructor writes its first Node.
class TrackingPages
{
  public:
    explicit TrackingPages(PageLedger &ledger)
        : ledger_{&ledger}
    {
    }

    auto allocate(std::size_t bytes) -> void *
    {
        static constexpr auto slack = std::size_t{4096};
        auto *page = std::malloc(bytes + slack);
        if (page == nullptr)
        {
            throw std::bad_alloc{};
        }
        ledger_->note_allocated(page);
        return page;
    }

    auto deallocate(void *page) -> void
    {
        ledger_->note_released(page);
    }

  private:
    PageLedger *ledger_;
};

}
