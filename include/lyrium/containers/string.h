#pragma once

#include <string>
#include <string_view>

#include "lyrium/allocators/std_allocator.h"

namespace lyrium
{
using String = std::basic_string<char, std::char_traits<char>, STDAllocator<char>>;
using StringView = std::string_view;
}
