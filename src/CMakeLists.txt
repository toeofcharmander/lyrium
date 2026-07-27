add_library(eluvian INTERFACE)

target_include_directories(eluvian INTERFACE ${PROJECT_SOURCE_DIR}/include)

target_sources(eluvian INTERFACE
  ${PROJECT_SOURCE_DIR}/include/eluvian/allocators/page_allocator.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/allocators/free_list_allocator.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/allocators/global_allocator.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/hooks/com_hook.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/config.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/log.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/sha256.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/dao/targets.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/dao/inline_hook.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/dao/engine_hooks.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/diag/process_info.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/diag/sampler.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/diag/texture_size.h
  ${PROJECT_SOURCE_DIR}/include/eluvian/diag/va_space.h
)

install(TARGETS eluvian DESTINATION bin)

target_compile_options(eluvian INTERFACE
  -Wall
  -Wextra
  -pedantic
  -Werror
  -Wconversion-null
  -Wmissing-declarations
  -Woverlength-strings
  -Wpointer-arith
  -Wunused-local-typedefs
  -Wunused-result
  -Wvarargs
  -Wvla
  -Wwrite-strings
  -Wno-cast-function-type
)
target_compile_features(eluvian INTERFACE cxx_std_26)
target_compile_definitions(eluvian INTERFACE -DNOMINMAX)

# stdc++exp is a libstdc++ library (it carries the C++23 <stacktrace> and <print>
# support). It does not exist when the toolchain uses libc++, so only ask for it
# where it is actually available.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
check_cxx_source_compiles("int main() { return 0; }" ELUVIAN_HAVE_STDCXXEXP)
unset(CMAKE_REQUIRED_LIBRARIES)
if (ELUVIAN_HAVE_STDCXXEXP)
  target_link_libraries(eluvian INTERFACE stdc++exp)
endif()

add_library(d3d9 SHARED
  d3d9.cpp
  engine_hooks.cpp
  d3d9.def
)
# psapi for the module and working set queries, d3d9 for the device interfaces.
target_link_libraries(d3d9 PRIVATE eluvian imguilib dwmapi psapi)
set_target_properties(d3d9 PROPERTIES PREFIX "")
