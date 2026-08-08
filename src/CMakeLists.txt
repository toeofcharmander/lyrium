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

# lyrium ships under either of two filenames, from the same sources.
#
#   d3d9.dll     the graphics proxy. Exports Direct3DCreate9 and loads the system
#                DLL behind it. Cannot coexist with ReShade, which wants that name.
#   dinput8.dll  takes an earlier, unrelated slot the game also imports, forwards
#                DirectInput straight through, and reaches Direct3DCreate9 by
#                patching the game's import table. Leaves d3d9.dll for ReShade.
#
# The difference is the .def and one extra translation unit. d3d9.cpp detects which
# it is at run time by whether the import swap actually changed anything.
function(lyrium_add_proxy name)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  add_library(${name} SHARED
    d3d9.cpp
    overlay.cpp
    engine_hooks.cpp
    resettable_texture.cpp
    ${ARG_SOURCES}
  )
  target_link_libraries(${name} PRIVATE lyrium imguilib dwmapi psapi)
  target_compile_options(${name} PRIVATE -Wno-microsoft-exception-spec)

  # The game loads these by name, so the filenames must stay exact --
  # PREFIX "" is what stops MinGW emitting libd3d9.dll.
  set_target_properties(${name} PROPERTIES PREFIX "")

  # Dropped next to daorigins.exe and must stand alone: no dependency on
  # libstdc++-6.dll, libgcc_s_dw2-1.dll or libwinpthread-1.dll.
  if (LYRIUM_STATIC_RUNTIME)
    target_link_options(${name} PRIVATE -static)
  endif()
endfunction()

lyrium_add_proxy(d3d9 SOURCES d3d9.def)
lyrium_add_proxy(dinput8 SOURCES dinput8.cpp dinput8.def)

# Proves the enum values in tests/shim/d3d9_enum_list.inl match the genuine
# <d3d9.h>. It is compiled here because this is the only configuration that has
# the real header. Nothing links it -- the static_asserts are the whole product,
# so a value that drifts breaks this build instead of silently making the Linux
# texture-sizing tests measure the wrong thing.
add_library(lyrium_d3d9_shim_conformance OBJECT
  ${PROJECT_SOURCE_DIR}/tests/conformance/d3d9_shim_conformance.cpp
)
target_include_directories(lyrium_d3d9_shim_conformance PRIVATE
  ${PROJECT_SOURCE_DIR}/tests
  ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(lyrium_d3d9_shim_conformance PRIVATE cxx_std_26)
add_dependencies(d3d9 lyrium_d3d9_shim_conformance)
add_dependencies(dinput8 lyrium_d3d9_shim_conformance)
