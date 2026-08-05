#pragma once

#include <cstdint>

#include <d3d9.h>

namespace lyrium
{

using CreateTextureFn = ::HRESULT(
    WINAPI *)(void *, ::UINT, ::UINT, ::UINT, ::DWORD, ::D3DFORMAT, ::D3DPOOL, ::IDirect3DTexture9 **, ::HANDLE *);

using TextureDestroyedFn = void (*)(::IDirect3DTexture9 *);

struct ResettableTextureShape
{
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t levels;
    std::uint32_t usage;
    ::D3DFORMAT format;
};

auto make_resettable_texture(
    ::IDirect3DDevice9 *device,
    CreateTextureFn create_texture,
    ::IDirect3DTexture9 *texture,
    const ResettableTextureShape &shape,
    TextureDestroyedFn destroyed) -> ::IDirect3DTexture9 *;

// Returns the texture that should actually be handed to D3D, with a reference
// taken. The caller MUST Release it once the D3D call has returned. Returning an
// unowned pointer was a use-after-free: a concurrent device reset could release
// the inner texture between the lookup and the bind.
//
// Returns nullptr when the wrapper is already dying or its inner texture cannot
// be recreated, which the caller should treat as "bind nothing".
auto acquire_bound_texture(::IDirect3DBaseTexture9 *texture) -> ::IDirect3DBaseTexture9 *;
auto rewrap_resettable_texture(::IDirect3DBaseTexture9 *texture) -> ::IDirect3DBaseTexture9 *;

auto before_resettable_texture_reset(::IDirect3DDevice9 *device) -> void;
auto after_resettable_texture_reset() -> void;

}
