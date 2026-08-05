// The single source of truth for the pixel format and texture pool values.
//
// These are numerically the D3D9 D3DFMT_* and D3DPOOL_* values, but they are
// listed here rather than taken from <d3d9.h> so that the portable half of the
// project never has to include a Windows header. The list is consumed three
// times:
//
//   texture/texture_desc.h        -> the PixelFormat and TexturePool enums used
//                                    by all policy code
//   tests/shim/d3d9.h             -> a stand-in <d3d9.h> so the remaining
//                                    D3D-typed headers compile on Linux
//   tests/conformance/...cpp      -> static_asserts compiled on Windows against
//                                    the genuine <d3d9.h>
//
// That third expansion is what makes the other two trustworthy: a value that
// drifts from the real header becomes a build error rather than a test that
// quietly measures the wrong thing.
//
// Add values here, never to a consumer directly.

#pragma once

#define LYRIUM_D3D9_FOURCC(a, b, c, d)                                                                                 \
    ((static_cast<unsigned>(a)) | (static_cast<unsigned>(b) << 8) | (static_cast<unsigned>(c) << 16) |                 \
     (static_cast<unsigned>(d) << 24))

#define LYRIUM_D3D9_FORMAT_LIST(X)                                                                                     \
    X(D3DFMT_UNKNOWN, 0)                                                                                               \
    X(D3DFMT_R8G8B8, 20)                                                                                               \
    X(D3DFMT_A8R8G8B8, 21)                                                                                             \
    X(D3DFMT_X8R8G8B8, 22)                                                                                             \
    X(D3DFMT_R5G6B5, 23)                                                                                               \
    X(D3DFMT_X1R5G5B5, 24)                                                                                             \
    X(D3DFMT_A1R5G5B5, 25)                                                                                             \
    X(D3DFMT_A4R4G4B4, 26)                                                                                             \
    X(D3DFMT_A8, 28)                                                                                                   \
    X(D3DFMT_X4R4G4B4, 30)                                                                                             \
    X(D3DFMT_A2B10G10R10, 31)                                                                                          \
    X(D3DFMT_A8B8G8R8, 32)                                                                                             \
    X(D3DFMT_X8B8G8R8, 33)                                                                                             \
    X(D3DFMT_G16R16, 34)                                                                                               \
    X(D3DFMT_A2R10G10B10, 35)                                                                                          \
    X(D3DFMT_A16B16G16R16, 36)                                                                                         \
    X(D3DFMT_P8, 41)                                                                                                   \
    X(D3DFMT_L8, 50)                                                                                                   \
    X(D3DFMT_A8L8, 51)                                                                                                 \
    X(D3DFMT_A4L4, 52)                                                                                                 \
    X(D3DFMT_V8U8, 60)                                                                                                 \
    X(D3DFMT_Q8W8V8U8, 63)                                                                                             \
    X(D3DFMT_V16U16, 64)                                                                                               \
    X(D3DFMT_D32, 71)                                                                                                  \
    X(D3DFMT_D15S1, 73)                                                                                                \
    X(D3DFMT_D24S8, 75)                                                                                                \
    X(D3DFMT_D24X8, 77)                                                                                                \
    X(D3DFMT_D16, 80)                                                                                                  \
    X(D3DFMT_L16, 81)                                                                                                  \
    X(D3DFMT_Q16W16V16U16, 110)                                                                                        \
    X(D3DFMT_R16F, 111)                                                                                                \
    X(D3DFMT_G16R16F, 112)                                                                                             \
    X(D3DFMT_A16B16G16R16F, 113)                                                                                       \
    X(D3DFMT_R32F, 114)                                                                                                \
    X(D3DFMT_G32R32F, 115)                                                                                             \
    X(D3DFMT_A32B32G32R32F, 116)                                                                                       \
    X(D3DFMT_DXT1, LYRIUM_D3D9_FOURCC('D', 'X', 'T', '1'))                                                             \
    X(D3DFMT_DXT2, LYRIUM_D3D9_FOURCC('D', 'X', 'T', '2'))                                                             \
    X(D3DFMT_DXT3, LYRIUM_D3D9_FOURCC('D', 'X', 'T', '3'))                                                             \
    X(D3DFMT_DXT4, LYRIUM_D3D9_FOURCC('D', 'X', 'T', '4'))                                                             \
    X(D3DFMT_DXT5, LYRIUM_D3D9_FOURCC('D', 'X', 'T', '5'))

#define LYRIUM_D3D9_POOL_LIST(X)                                                                                       \
    X(D3DPOOL_DEFAULT, 0)                                                                                              \
    X(D3DPOOL_MANAGED, 1)                                                                                              \
    X(D3DPOOL_SYSTEMMEM, 2)                                                                                            \
    X(D3DPOOL_SCRATCH, 3)
