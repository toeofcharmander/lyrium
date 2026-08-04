// Minimal stand-in for the DirectX <d3d9.h>, sufficient for the portable headers.
//
// diag/texture_size.h needs D3DFORMAT and D3DPOOL enumerators and makes no D3D
// calls at all, so a pair of enums is enough to compile and test it on Linux.
//
// This file deliberately contains no values of its own. Everything comes from
// d3d9_enum_list.inl, which the Windows build checks against the genuine header.
//
// Only targets that need those enums get tests/shim on their include path, so
// this can never silently shadow the real <d3d9.h> in the DLL build.

#pragma once

#include "lyrium/texture/pixel_format_values.inl"

#define LYRIUM_D3D9_ENUMERATOR(name, value) name = (value),

enum D3DFORMAT : unsigned
{
    LYRIUM_D3D9_FORMAT_LIST(LYRIUM_D3D9_ENUMERATOR)
};

enum D3DPOOL : unsigned
{
    LYRIUM_D3D9_POOL_LIST(LYRIUM_D3D9_ENUMERATOR)
};

#undef LYRIUM_D3D9_ENUMERATOR
