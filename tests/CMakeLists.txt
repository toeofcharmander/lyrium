# The entire premise of this suite is ABI parity with the shipping 32-bit DLL.
# A 64-bit configure would exercise different pointer arithmetic in the allocators
# and the address-space math, and would silently pass tests that mean nothing.
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "lyrium tests must be configured for a 32-bit ABI, but CMAKE_SIZEOF_VOID_P is ${CMAKE_SIZEOF_VOID_P}.\n"
    "Use:  cmake --preset tests-linux32     (requires g++-multilib)")
endif()

# Everything a test needs: the production headers with their warning set,
# GoogleTest, and tests/ on the include path for support/ and shim/.
add_library(lyrium_test_base INTERFACE)
target_link_libraries(lyrium_test_base INTERFACE lyrium GTest::gtest GTest::gtest_main)
target_include_directories(lyrium_test_base INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Some tests drive process-global counters into states that would corrupt every
# later assertion in the same binary, so each test target is its own executable.
# KNOWN_DEFECT marks a test that pins a real bug and is expected to fail until
# that bug is fixed; those are excluded from the green loop by the test preset.
function(lyrium_add_test name)
  cmake_parse_arguments(ARG "KNOWN_DEFECT" "" "SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE lyrium_test_base)
  add_test(NAME ${name} COMMAND ${name})
  if (ARG_KNOWN_DEFECT)
    set_tests_properties(${name} PROPERTIES LABELS known_defect)
  endif()
endfunction()

lyrium_add_test(abi_probe_test SOURCES unit/abi_probe_test.cpp)
lyrium_add_test(sha256_test SOURCES unit/sha256_test.cpp)
lyrium_add_test(page_allocator_test SOURCES unit/page_allocator_test.cpp)
lyrium_add_test(free_list_allocator_test SOURCES unit/free_list_allocator_test.cpp)
