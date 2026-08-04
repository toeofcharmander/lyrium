#pragma once

#include <cstdint>

#include <d3d9.h>

namespace lyrium
{

using CreateTextureFn = ::HRESULT(WINAPI *)(
    void *,
    ::UINT,
    ::UINT,
    ::UINT,
    ::DWORD,
    ::D3DFORMAT,
    ::D3DPOOL,
    ::IDirect3DTexture9 **,
    ::HANDLE *);

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

auto unwrap_resettable_texture(::IDirect3DBaseTexture9 *texture) -> ::IDirect3DBaseTexture9 *;
auto rewrap_resettable_texture(::IDirect3DBaseTexture9 *texture) -> ::IDirect3DBaseTexture9 *;

auto before_resettable_texture_reset(::IDirect3DDevice9 *device) -> void;
auto after_resettable_texture_reset() -> void;

}
