# Weighted Path Fabric - benchmarks.
# Copyright 2026 Summon Software Labs.

# The root project enables testing after this directory has been added, so this
# directory enables testing for its own scope. Without it the generated root
# registry would descend into this directory and find no CTestTestfile.cmake.
enable_testing()

add_executable(wpf_bench wpf_bench.cpp)
target_link_libraries(wpf_bench PRIVATE SummonSoftwareLabs::WeightedPathFabric)
if(MSVC)
  target_compile_options(wpf_bench PRIVATE /W4 /permissive- /utf-8 /EHsc)
  if(WPF_WARNINGS_AS_ERRORS)
    target_compile_options(wpf_bench PRIVATE /WX)
  endif()
else()
  target_compile_options(wpf_bench PRIVATE -Wall -Wextra)
  if(WPF_WARNINGS_AS_ERRORS)
    target_compile_options(wpf_bench PRIVATE -Werror)
  endif()
endif()
add_test(NAME wpf_bench COMMAND wpf_bench)
set_tests_properties(wpf_bench PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
