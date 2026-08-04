cmake_minimum_required(VERSION 3.25)

project(
  lyrium
  VERSION "0.0.1"
  DESCRIPTION "D3D9 proxy that fights address-space fragmentation in Dragon Age: Origins"
  LANGUAGES CXX
)

include(FetchContent)

option(LYRIUM_BUILD_DLL "Build the shipping d3d9.dll (needs a Windows MinGW toolchain)" ON)
option(LYRIUM_BUILD_TESTS "Build the unit tests (needs googletest)" OFF)
option(LYRIUM_STATIC_RUNTIME "Link the C++ runtime statically into the DLL" ON)

set(CMAKE_SKIP_PACKAGE_ALL_DEPENDENCY ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(FETCHCONTENT_QUIET FALSE)

# NOTE: no CMAKE_*_FLAGS mutation here. 32-bit-ness comes from the toolchain file
# (cmake/toolchains/*.cmake) so that it is in effect before compiler detection.
# Setting -m32 after project() leaves CMAKE_SIZEOF_VOID_P at 8 and makes every
# try_compile answer for the wrong ABI. Static linking is a property of the
# shipping DLL, so it lives on that target in src/CMakeLists.txt.

if (LYRIUM_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    SYSTEM)
  FetchContent_MakeAvailable(googletest)
endif()

# ImGui is only needed by the overlay, which only exists in the DLL. Fetching it
# for a test-only configure would demand a Windows toolchain we do not have.
if (LYRIUM_BUILD_DLL)
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.6-docking
    SYSTEM)
  FetchContent_MakeAvailable(imgui)
  add_library(imguilib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  )
  target_include_directories(imguilib SYSTEM PUBLIC ${imgui_SOURCE_DIR})
endif()

add_subdirectory(src)

if (LYRIUM_BUILD_TESTS)
  enable_testing()
  include(CTest)
  add_subdirectory(tests)
endif()

set(CPACK_PACKAGE_NAME "lyrium")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_SOURCE_IGNORE_FILES "build")
include(CPack)
