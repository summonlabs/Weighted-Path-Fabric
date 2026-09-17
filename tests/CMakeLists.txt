# Weighted Path Fabric - test suites.
# Copyright 2026 Summon Software Labs.

add_library(wpf_test_support STATIC
  support/bigint.cpp
  support/oracle.cpp
  support/process.cpp
  support/raw_socket.cpp
  support/test_harness.cpp
)
target_include_directories(wpf_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(wpf_test_support PUBLIC WeightedPathFabric)

if(MSVC)
  target_compile_options(wpf_test_support PRIVATE /W4 /permissive- /utf-8 /EHsc)
  if(WPF_WARNINGS_AS_ERRORS)
    target_compile_options(wpf_test_support PRIVATE /WX)
  endif()
else()
  target_compile_options(wpf_test_support PRIVATE -Wall -Wextra)
  if(WPF_WARNINGS_AS_ERRORS)
    target_compile_options(wpf_test_support PRIVATE -Werror)
  endif()
endif()

function(wpf_add_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE wpf_test_support)
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

wpf_add_test(wpf_types_tests types_test.cpp)
# The CMake project version and the representation contracts must agree with the
# values compiled into the headers; the types suite asserts this.
target_compile_definitions(wpf_types_tests PRIVATE
  WPF_CMAKE_PRODUCT_VERSION="${PROJECT_VERSION}"
  WPF_CMAKE_WIRE_PROTOCOL_VERSION=${WPF_WIRE_PROTOCOL_VERSION}
  WPF_CMAKE_PERSISTENCE_FORMAT_VERSION=${WPF_PERSISTENCE_FORMAT_VERSION})
wpf_add_test(wpf_apportion_tests apportion_test.cpp)
wpf_add_test(wpf_rebalance_tests rebalance_test.cpp)
wpf_add_test(wpf_lifecycle_tests lifecycle_test.cpp)
wpf_add_test(wpf_engine_tests engine_test.cpp)
wpf_add_test(wpf_limits_tests limits_test.cpp)
wpf_add_test(wpf_property_tests property_test.cpp)
wpf_add_test(wpf_race_tests race_test.cpp)
wpf_add_test(wpf_persistence_tests persistence_test.cpp)
wpf_add_test(wpf_wire_tests wire_test.cpp)

if(WPF_BUILD_TOOLS)
  wpf_add_test(wpf_distributed_tests distributed_test.cpp)
  target_compile_definitions(wpf_distributed_tests
    PRIVATE WPF_TOOL_DIR="$<TARGET_FILE_DIR:wpf-coordinator>")
  add_dependencies(wpf_distributed_tests wpf-coordinator wpf-publisher wpf)
endif()
