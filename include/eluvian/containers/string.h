#pragma once

#include <string>
#include <string_view>

#include "eluvian/allocators/std_allocator.h"

namespace eluvian
{
using String = std::basic_string<char, std::char_traits<char>, STDAllocator<char>>;
using StringView = std::string_view;
}
