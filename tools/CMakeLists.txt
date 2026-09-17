# Weighted Path Fabric - command line tools.
# Copyright 2026 Summon Software Labs.

add_library(wpf_tool_support STATIC tool_support.cpp)
target_link_libraries(wpf_tool_support PUBLIC SummonSoftwareLabs::WeightedPathFabric)

function(wpf_add_tool target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE wpf_tool_support)
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
  install(TARGETS ${target} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endfunction()

if(MSVC)
  target_compile_options(wpf_tool_support PRIVATE /W4 /permissive- /utf-8 /EHsc)
  if(WPF_WARNINGS_AS_ERRORS)
    target_compile_options(wpf_tool_support PRIVATE /WX)
  endif()
else()
  target_compile_options(wpf_tool_support PRIVATE -Wall -Wextra)
endif()

wpf_add_tool(wpf-coordinator wpf_coordinator.cpp)
wpf_add_tool(wpf-publisher wpf_publisher.cpp)
wpf_add_tool(wpf wpf_cli.cpp)

add_test(NAME wpf_cli_version COMMAND wpf version)
add_test(NAME wpf_cli_lifecycle_table COMMAND wpf lifecycle-table)
add_test(NAME wpf_cli_persistence_format COMMAND wpf persistence-format)
