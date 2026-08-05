#pragma once

#include <cstdint>

namespace lyrium::diag
{

// Which of lyrium's own hooks an allocation happened inside.
//
// Attribution by stack walk cannot answer this, and two live sessions proved it
// rather than suggested it: every one of 256 records resolved to ntdll.dll,
// KERNEL32.DLL or KERNELBASE.dll. RtlCaptureStackBackTrace follows the EBP
// chain, optimised 32-bit system code omits frame pointers, and the walk
// therefore stops inside the allocator that made the call. There is no depth of
// frames that fixes that.
//
// So mark the window instead of reconstructing it. lyrium already intercepts the
// exact calls that would create a MANAGED duplicate, so setting a marker across
// one costs a thread-local store and answers a sharper question than "which
// module": was this allocation caused by a texture create?
//
// Thread-local at the call site rather than here: two render threads must not
// see each other's window, and this header stays portable so the scoping is
// testable.
enum class AllocContext : std::uint32_t
{
    none,
    // Inside IDirect3DDevice9::CreateTexture, which is where the runtime would
    // allocate a MANAGED texture's system-memory duplicate.
    d3d_create_texture,
    // Inside one of the engine's own texture paths, which calls the above.
    engine_texture,
    device_reset,
};

inline constexpr auto alloc_context_count = std::uint32_t{4};

[[nodiscard]] constexpr auto name_of(AllocContext context) -> const char *
{
    switch (context)
    {
        case AllocContext::d3d_create_texture: return "d3d_create";
        case AllocContext::engine_texture: return "engine_texture";
        case AllocContext::device_reset: return "device_reset";
        case AllocContext::none: break;
    }
    return "none";
}

// Opens a window and restores whatever was open before it.
//
// Restores rather than clears because the windows nest: the engine's texture
// create calls D3D's, and a hook returning must not blank a window its caller
// still has open.
class AllocContextScope
{
  public:
    AllocContextScope(AllocContext &slot, AllocContext context)
        : slot_{&slot}
        , previous_{slot}
    {
        *slot_ = context;
    }

    ~AllocContextScope()
    {
        *slot_ = previous_;
    }

    AllocContextScope(const AllocContextScope &) = delete;
    auto operator=(const AllocContextScope &) -> AllocContextScope & = delete;

  private:
    AllocContext *slot_;
    AllocContext previous_;
};

}
