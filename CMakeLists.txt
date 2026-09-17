cmake_minimum_required(VERSION 3.25)

project(WeightedPathFabric
  VERSION 1.0.0
  DESCRIPTION "Authoritative non-equal path-weight governance runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Weighted-Path-Fabric"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Representation contracts, reported by the installed package and checked
# against include/wpf/version.hpp by the test suite.
set(WPF_WIRE_PROTOCOL_VERSION 1)
set(WPF_PERSISTENCE_FORMAT_VERSION 1)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(WPF_BUILD_TESTS "Build the Weighted Path Fabric test suites" ON)
option(WPF_BUILD_EXAMPLES "Build the Weighted Path Fabric examples" ON)
option(WPF_BUILD_BENCHMARKS "Build the Weighted Path Fabric benchmarks" ON)
option(WPF_BUILD_TOOLS "Build the Weighted Path Fabric command line tools" ON)
option(WPF_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(WPF_ENABLE_ASAN "Instrument first-party code with AddressSanitizer" OFF)
option(WPF_ENABLE_ANALYZE "Run the MSVC static analyzer over first-party code" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Weighted Path Fabric library
# ---------------------------------------------------------------------------
set(WPF_LIBRARY_SOURCES
  src/apportion.cpp
  src/assignment.cpp
  src/digest.cpp
  src/engine.cpp
  src/engine_api.cpp
  src/engine_mutations.cpp
  src/lifecycle.cpp
  src/model.cpp
  src/net.cpp
  src/outcome.cpp
  src/persistence.cpp
  src/snapshot.cpp
  src/types.cpp
  src/version.cpp
  src/weight.cpp
  src/wire.cpp
)

add_library(WeightedPathFabric STATIC ${WPF_LIBRARY_SOURCES})
add_library(SummonSoftwareLabs::WeightedPathFabric ALIAS WeightedPathFabric)
set_target_properties(WeightedPathFabric PROPERTIES EXPORT_NAME WeightedPathFabric)

target_include_directories(WeightedPathFabric
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(WeightedPathFabric PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(WeightedPathFabric PUBLIC ws2_32)
endif()

if(MSVC)
  target_compile_options(WeightedPathFabric PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(WeightedPathFabric PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  if(WPF_WARNINGS_AS_ERRORS)
    target_compile_options(WeightedPathFabric PRIVATE /WX)
  endif()
  if(WPF_ENABLE_ANALYZE)
    # External headers (the Windows SDK in particular) are scoped out of the
    # analyzer by path, never by a blanket warning suppression: first-party code
    # keeps the full /W4 + /analyze treatment.
    target_compile_options(WeightedPathFabric PRIVATE /analyze /external:anglebrackets /external:W0)
  endif()
else()
  target_compile_options(WeightedPathFabric PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
  if(WPF_WARNINGS_AS_ERRORS)
    target_compile_options(WeightedPathFabric PRIVATE -Werror)
  endif()
endif()

if(WPF_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(WeightedPathFabric PUBLIC /fsanitize=address /Zi)
    target_link_options(WeightedPathFabric PUBLIC /INCREMENTAL:NO)
  else()
    target_compile_options(WeightedPathFabric PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(WeightedPathFabric PUBLIC -fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS WeightedPathFabric
  EXPORT WeightedPathFabricTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT WeightedPathFabricTargets
  FILE WeightedPathFabricTargets.cmake
  NAMESPACE SummonSoftwareLabs::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/WeightedPathFabric"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WeightedPathFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/WeightedPathFabricConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/WeightedPathFabric"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/WeightedPathFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/WeightedPathFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/WeightedPathFabricConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/WeightedPathFabric"
)

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
enable_testing()

if(WPF_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(WPF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(WPF_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
if(WPF_BUILD_TESTS)
  add_subdirectory(tests)
endif()
