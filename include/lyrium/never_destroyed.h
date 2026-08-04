#pragma once

#include <cstddef>
#include <new>
#include <utility>

namespace lyrium
{

// Holds an object that is constructed once and never destroyed.
//
// The rule this encodes, which applies to this DLL as a whole: objects with
// static storage duration are never destroyed; objects with automatic or dynamic
// storage duration keep their destructors, unchanged.
//
// Why that is correct rather than lazy. RAII's contract is "release before it
// leaks", and at process exit there is nothing left to release -- the kernel
// reclaims every handle, page and heap when the process object dies. What static
// destructors do at that point is not cleanup, it is risk: they run after
// DllMain returns, under the loader lock, with every other thread already
// terminated by ExitProcess wherever it happened to be. A thread killed while
// holding a mutex holds it forever, so any exit-time code that takes a lock can
// deadlock inside the loader with no way out.
//
// That is not hypothetical for the two singletons this is used on. Both own a
// worker thread, and their destructors locked a mutex the worker may have died
// holding and then joined a thread that will never wake. Note also that no safe
// destructor for them can be written at all: destroying a joinable std::thread
// calls std::terminate(). Suppression is the only option that exists, not a
// shortcut past a hard one.
//
// The load-bearing property is that NeverDestroyed<T> is itself trivially
// destructible for every T, so as a function-local static it registers no
// __cxa_atexit entry and never enters the exit sequence at all.
//
// The in-tree precedent is exact: GlobalAllocator::heap() calls HeapCreate and
// never HeapDestroy, which is what keeps the private heap alive for every
// container that frees into it. This names that existing exception so it reads
// as a decision rather than an oversight.
template <class T>
class NeverDestroyed
{
  public:
    template <class... Args>
    explicit NeverDestroyed(Args &&...args)
    {
        ::new (static_cast<void *>(storage_)) T{std::forward<Args>(args)...};
    }

    NeverDestroyed(const NeverDestroyed &) = delete;
    auto operator=(const NeverDestroyed &) -> NeverDestroyed & = delete;

    // Deliberately does not call ~T. See above.
    ~NeverDestroyed() = default;

    [[nodiscard]] auto get() -> T &
    {
        return *std::launder(reinterpret_cast<T *>(storage_));
    }

    [[nodiscard]] auto get() const -> const T &
    {
        return *std::launder(reinterpret_cast<const T *>(storage_));
    }

    [[nodiscard]] auto operator*() -> T &
    {
        return get();
    }

    [[nodiscard]] auto operator->() -> T *
    {
        return &get();
    }

  private:
    alignas(T) std::byte storage_[sizeof(T)];
};

}
