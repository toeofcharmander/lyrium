#pragma once

#include <stacktrace>

#include "eluvian/allocators/std_allocator.h"

namespace eluvian
{

using StackTrace = std::basic_stacktrace<STDAllocator<std::stacktrace_entry>>;

}
