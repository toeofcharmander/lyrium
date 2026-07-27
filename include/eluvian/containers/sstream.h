#pragma once

#include <sstream>

#include "eluvian/allocators/std_allocator.h"

namespace eluvian
{

using StringStream = std::basic_stringstream<char, std::char_traits<char>, STDAllocator<char>>;

}
