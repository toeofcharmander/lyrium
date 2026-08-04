# The portable half of the project: a header-only INTERFACE library carrying the
# include path, the warning set, and the language level. Both the shipping DLL
# and the test suite consume it, so it must configure without a Windows toolchain.
add_library(lyrium INTERFACE)

target_include_directories(lyrium INTERFACE ${PROJECT_SOURCE_DIR}/include)

target_compile_options(lyrium INTERFACE
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
target_compile_features(lyrium INTERFACE cxx_std_26)
target_compile_definitions(lyrium INTERFACE -DNOMINMAX)

# std::print and std::stacktrace live in libstdc++exp on GCC 14+. The probe is
# what decides whether it is available for the ABI currently being configured.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
check_cxx_source_compiles("int main() { return 0; }" LYRIUM_HAVE_STDCXXEXP)
unset(CMAKE_REQUIRED_LIBRARIES)
if (LYRIUM_HAVE_STDCXXEXP)
  target_link_libraries(lyrium INTERFACE stdc++exp)
endif()

if (NOT LYRIUM_BUILD_DLL)
  return()
endif()

install(TARGETS lyrium DESTINATION bin)

add_library(d3d9 SHARED
  d3d9.cpp
  overlay.cpp
  engine_hooks.cpp
  resettable_texture.cpp
  d3d9.def
)
target_link_libraries(d3d9 PRIVATE lyrium imguilib dwmapi psapi)
target_compile_options(d3d9 PRIVATE -Wno-microsoft-exception-spec)

# The game loads this DLL by name, so the filename must stay exactly d3d9.dll --
# PREFIX "" is what stops MinGW emitting libd3d9.dll.
set_target_properties(d3d9 PROPERTIES PREFIX "")

# The DLL is dropped next to daorigins.exe and must stand alone: no dependency on
# libstdc++-6.dll, libgcc_s_dw2-1.dll or libwinpthread-1.dll.
if (LYRIUM_STATIC_RUNTIME)
  target_link_options(d3d9 PRIVATE -static)
endif()
