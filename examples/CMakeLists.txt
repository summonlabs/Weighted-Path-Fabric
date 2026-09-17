# Weighted Path Fabric - examples.
# Copyright 2026 Summon Software Labs.

# The root project enables testing after this directory has been added, so this
# directory enables testing for its own scope. Without it the generated root
# registry would descend into this directory and find no CTestTestfile.cmake.
enable_testing()

# Every example is a standalone executable over the public API, and every
# example is also a ctest case. The warning policy is the policy of the test
# suites: nothing is suppressed.
function(wpf_add_example target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE SummonSoftwareLabs::WeightedPathFabric)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    if(WPF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(WPF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

wpf_add_example(wpf_example_01_configured_weighting wpf_example_01_configured_weighting.cpp)
wpf_add_example(wpf_example_02_scale_equivalent_ratios wpf_example_02_scale_equivalent_ratios.cpp)
wpf_add_example(wpf_example_03_eligibility_loss wpf_example_03_eligibility_loss.cpp)
wpf_add_example(wpf_example_04_member_restoration wpf_example_04_member_restoration.cpp)
wpf_add_example(wpf_example_05_zero_weight_member wpf_example_05_zero_weight_member.cpp)
wpf_add_example(wpf_example_06_batch_weight_update wpf_example_06_batch_weight_update.cpp)
wpf_add_example(wpf_example_07_deterministic_rebalance wpf_example_07_deterministic_rebalance.cpp)
wpf_add_example(wpf_example_08_stale_path_authority wpf_example_08_stale_path_authority.cpp)
wpf_add_example(wpf_example_09_coordinator_restart wpf_example_09_coordinator_restart.cpp)
wpf_add_example(wpf_example_10_worker_reincarnation wpf_example_10_worker_reincarnation.cpp)