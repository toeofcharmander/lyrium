#pragma once

#include <sstream>

#include "lyrium/allocators/std_allocator.h"

namespace lyrium
{

using StringStream = std::basic_stringstream<char, std::char_traits<char>, STDAllocator<char>>;

}
