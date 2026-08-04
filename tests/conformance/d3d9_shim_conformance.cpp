// Compiled as part of the Windows DLL build, against the genuine <d3d9.h>.
//
// This translation unit exists so the values in the portable half of the project
// are proved rather than trusted. Nothing links it -- the static_asserts are the
// entire product. A value that drifts from the real header becomes a build error
// here instead of a Linux unit test that quietly measures the wrong thing.
//
// It deliberately does NOT include tests/shim/d3d9.h, which would defeat the
// purpose by shadowing the header it is meant to be checked against.

#include <d3d9.h>

#include "lyrium/diag/va_region.h"
#include "lyrium/texture/texture_desc.h"

// 1. The raw value list, used by both the shim and the PixelFormat enum.
#define LYRIUM_D3D9_CONFORMS(name, value)                                                                              \
    static_assert(                                                                                                     \
        static_cast<unsigned>(name) == static_cast<unsigned>(value),                                                   \
        #name " in lyrium/texture/pixel_format_values.inl does not match the real <d3d9.h>");

LYRIUM_D3D9_FORMAT_LIST(LYRIUM_D3D9_CONFORMS)
LYRIUM_D3D9_POOL_LIST(LYRIUM_D3D9_CONFORMS)

#undef LYRIUM_D3D9_CONFORMS

// 2. The vocabulary enums the policy layer speaks. These are what make it safe
//    for policy code to be tested on Linux and then handed real D3D values at
//    runtime without a translation table.
#define LYRIUM_PIXEL_FORMAT_CONFORMS(name, value)                                                                      \
    static_assert(                                                                                                     \
        static_cast<unsigned>(lyrium::texture::PixelFormat::name) == static_cast<unsigned>(name),                       \
        "PixelFormat::" #name " does not match the real D3DFORMAT value");

LYRIUM_D3D9_FORMAT_LIST(LYRIUM_PIXEL_FORMAT_CONFORMS)

#undef LYRIUM_PIXEL_FORMAT_CONFORMS

#define LYRIUM_TEXTURE_POOL_CONFORMS(name, value)                                                                      \
    static_assert(                                                                                                     \
        static_cast<unsigned>(lyrium::texture::TexturePool::name) == static_cast<unsigned>(name),                       \
        "TexturePool::" #name " does not match the real D3DPOOL value");

LYRIUM_D3D9_POOL_LIST(LYRIUM_TEXTURE_POOL_CONFORMS)

#undef LYRIUM_TEXTURE_POOL_CONFORMS

// 3. The usage bits the placement policy consults. A wrong value here would make
//    the policy relocate a render target or a dynamic texture, which is the exact
//    class of mistake that broke bink video playback upstream.
static_assert(lyrium::texture::usage::render_target.bits == D3DUSAGE_RENDERTARGET);
static_assert(lyrium::texture::usage::depth_stencil.bits == D3DUSAGE_DEPTHSTENCIL);
static_assert(lyrium::texture::usage::dynamic.bits == D3DUSAGE_DYNAMIC);
static_assert(lyrium::texture::usage::autogen_mipmap.bits == D3DUSAGE_AUTOGENMIPMAP);

static_assert(
    lyrium::texture::usage::not_relocatable.bits ==
        (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC | D3DUSAGE_AUTOGENMIPMAP),
    "the disqualifying usage set must match the mask used by the live CreateTexture hook");

// 4. The address-space region constants mirrored in diag/va_region.h so that the
//    classification can be tested without windows.h. A wrong value here would
//    misclassify free memory as committed, which is the number the rescue policy
//    reads.
static_assert(static_cast<unsigned>(lyrium::diag::RegionState::committed) == MEM_COMMIT);
static_assert(static_cast<unsigned>(lyrium::diag::RegionState::reserved) == MEM_RESERVE);
static_assert(static_cast<unsigned>(lyrium::diag::RegionState::free) == MEM_FREE);

static_assert(static_cast<unsigned>(lyrium::diag::RegionType::private_memory) == MEM_PRIVATE);
static_assert(static_cast<unsigned>(lyrium::diag::RegionType::mapped) == MEM_MAPPED);
static_assert(static_cast<unsigned>(lyrium::diag::RegionType::image) == MEM_IMAGE);
