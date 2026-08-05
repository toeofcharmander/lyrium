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
# Minimal <d3d9.h> providing only the D3DFORMAT and D3DPOOL enumerators. Given
# only to targets that ask for it, so it can never silently shadow the real header
# elsewhere. The Windows build checks its values against the genuine one.
add_library(lyrium_d3d9_shim INTERFACE)
target_include_directories(lyrium_d3d9_shim INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/shim)

function(lyrium_add_test name)
  cmake_parse_arguments(ARG "KNOWN_DEFECT;NEEDS_D3D9_SHIM" "" "SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE lyrium_test_base)
  if (ARG_NEEDS_D3D9_SHIM)
    target_link_libraries(${name} PRIVATE lyrium_d3d9_shim)
  endif()
  add_test(NAME ${name} COMMAND ${name})
  if (ARG_KNOWN_DEFECT)
    set_tests_properties(${name} PROPERTIES LABELS known_defect)
  endif()
endfunction()

lyrium_add_test(abi_probe_test SOURCES unit/abi_probe_test.cpp)
lyrium_add_test(sha256_test SOURCES unit/sha256_test.cpp)
lyrium_add_test(arena_test SOURCES unit/arena_test.cpp)
lyrium_add_test(local_alloc_policy_test SOURCES unit/local_alloc_policy_test.cpp)
lyrium_add_test(page_allocator_test SOURCES unit/page_allocator_test.cpp)
lyrium_add_test(free_list_allocator_test SOURCES unit/free_list_allocator_test.cpp)
lyrium_add_test(targets_table_test SOURCES unit/targets_table_test.cpp)
lyrium_add_test(texture_size_test NEEDS_D3D9_SHIM SOURCES unit/texture_size_test.cpp)
lyrium_add_test(block_rect_test SOURCES unit/block_rect_test.cpp)
lyrium_add_test(mip_layout_test SOURCES unit/mip_layout_test.cpp)
lyrium_add_test(config_parse_test SOURCES unit/config_parse_test.cpp)
lyrium_add_test(va_region_test SOURCES unit/va_region_test.cpp)
lyrium_add_test(free_size_classes_test SOURCES unit/free_size_classes_test.cpp)
lyrium_add_test(engine_health_test SOURCES unit/engine_health_test.cpp)
lyrium_add_test(alloc_attribution_test SOURCES unit/alloc_attribution_test.cpp)
lyrium_add_test(alloc_context_test SOURCES unit/alloc_context_test.cpp)
lyrium_add_test(size_tally_test SOURCES unit/size_tally_test.cpp)
lyrium_add_test(staging_pool_test SOURCES unit/staging_pool_test.cpp)
lyrium_add_test(dirty_levels_test SOURCES unit/dirty_levels_test.cpp)
lyrium_add_test(level_validity_test SOURCES unit/level_validity_test.cpp)
lyrium_add_test(ref_count_test SOURCES unit/ref_count_test.cpp)
lyrium_add_test(never_destroyed_test SOURCES unit/never_destroyed_test.cpp)
lyrium_add_test(texture_desc_test SOURCES unit/texture_desc_test.cpp)
lyrium_add_test(texture_placement_policy_test SOURCES unit/texture_placement_policy_test.cpp)
lyrium_add_test(texture_ledger_test SOURCES unit/texture_ledger_test.cpp)
lyrium_add_test(sample_schedule_test SOURCES unit/sample_schedule_test.cpp)
lyrium_add_test(rescue_policy_test SOURCES unit/rescue_policy_test.cpp)
lyrium_add_test(rescue_coordinator_test SOURCES unit/rescue_coordinator_test.cpp)

