// Compiled as part of the Windows DLL build, against the genuine <d3d9.h>.
//
// This translation unit exists purely so the enum values in tests/shim are proved
// rather than trusted. Every entry in the shared list is static_asserted against
// the real header, so a wrong value becomes a build error here instead of a unit
// test on Linux that quietly measures the wrong thing.
//
// It deliberately does NOT include tests/shim/d3d9.h -- that would defeat the
// purpose by shadowing the header it is meant to be checked against.

#include <d3d9.h>

#include "shim/d3d9_enum_list.inl"

#define LYRIUM_D3D9_CONFORMS(name, value)                                                                              \
    static_assert(                                                                                                     \
        static_cast<unsigned>(name) == static_cast<unsigned>(value),                                                   \
        #name " in tests/shim/d3d9_enum_list.inl does not match the real <d3d9.h>");

LYRIUM_D3D9_FORMAT_LIST(LYRIUM_D3D9_CONFORMS)
LYRIUM_D3D9_POOL_LIST(LYRIUM_D3D9_CONFORMS)

#undef LYRIUM_D3D9_CONFORMS
