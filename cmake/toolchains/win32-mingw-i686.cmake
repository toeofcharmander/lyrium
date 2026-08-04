# winlibs i686 MinGW-w64 GCC -- the shipping toolchain for d3d9.dll.
#
# This compiler is natively 32-bit (-dumpmachine reports i686-w64-mingw32), so
# there is deliberately no -m32 here. 32-bit-ness is a property of the toolchain,
# not a flag, and passing -m32 to a single-target compiler only invites confusion.
#
# Deliberately does NOT set CMAKE_SYSTEM_NAME: this builds on Windows for
# Windows, so it is a native build, not a cross build.
#
# By default the compilers are found on PATH, which keeps this file free of any
# machine-specific path. Set the LYRIUM_MINGW32_ROOT environment variable to pin
# a specific toolchain root instead.

if (DEFINED ENV{LYRIUM_MINGW32_ROOT})
  set(_lyrium_mingw_root "$ENV{LYRIUM_MINGW32_ROOT}")
  set(CMAKE_C_COMPILER "${_lyrium_mingw_root}/bin/gcc.exe")
  set(CMAKE_CXX_COMPILER "${_lyrium_mingw_root}/bin/g++.exe")
  set(CMAKE_RC_COMPILER "${_lyrium_mingw_root}/bin/windres.exe")
  set(CMAKE_FIND_ROOT_PATH "${_lyrium_mingw_root}")
  unset(_lyrium_mingw_root)
else()
  set(CMAKE_C_COMPILER gcc.exe)
  set(CMAKE_CXX_COMPILER g++.exe)
endif()
