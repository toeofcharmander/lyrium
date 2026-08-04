#pragma once

#include <stacktrace>

#include "lyrium/allocators/std_allocator.h"

namespace lyrium
{

using StackTrace = std::basic_stacktrace<STDAllocator<std::stacktrace_entry>>;

}
