#pragma once

#include "lyrium/allocators/std_allocator.h"
#include "lyrium/texture/texture_ledger.h"

namespace lyrium
{

// The ledger as the DLL instantiates it, bound to the private heap.
//
// This binding is the reason BasicTextureLedger is templated at all: the rule
// that lyrium never allocates from the game's CRT heap is honoured here, at the
// instantiation, rather than by making the ledger itself unportable. Tests bind
// the same template to std::allocator and exercise identical logic.
using DllTextureLedger = texture::BasicTextureLedger<STDAllocator>;

// Defined in src/d3d9.cpp. Deliberately a stepping stone: a later step gives
// ownership to a composition root and injects it, at which point this accessor
// and the function-local static behind it both go away.
auto texture_ledger() -> DllTextureLedger &;

}
