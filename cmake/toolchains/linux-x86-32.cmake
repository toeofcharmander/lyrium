# 32-bit Linux toolchain for the lyrium test suite.
#
# The point of this file is ABI parity with the shipping 32-bit Windows DLL:
# sizeof(void*) == 4 and sizeof(size_t) == 4, so the free-list allocator and the
# address-space arithmetic are exercised at the width they actually ship with.
#
# -m32 goes in the *_INIT variables so that it is in effect before CMake probes
# the compiler. Setting it after project() leaves CMAKE_SIZEOF_VOID_P at 8 and
# makes every try_compile and check_cxx_source_compiles answer for the wrong ABI.
#
# Deliberately does NOT set CMAKE_SYSTEM_NAME: this is a native Linux build, and
# setting it would switch CMake into cross-compiling mode for no benefit.
#
# Requires g++-multilib.

set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-m32")
