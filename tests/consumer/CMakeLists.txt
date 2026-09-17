cmake_minimum_required(VERSION 3.25)
project(WeightedPathFabricConsumer LANGUAGES CXX)

# An independent consumer of the installed Weighted Path Fabric package. It sits
# outside the source tree on purpose: it must work from an install prefix alone.
find_package(WeightedPathFabric CONFIG REQUIRED)

if(NOT TARGET SummonSoftwareLabs::WeightedPathFabric)
  message(FATAL_ERROR "the installed package did not export SummonSoftwareLabs::WeightedPathFabric")
endif()

add_executable(wpf_consumer main.cpp)
target_link_libraries(wpf_consumer PRIVATE SummonSoftwareLabs::WeightedPathFabric)
set_target_properties(wpf_consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

message(STATUS "WeightedPathFabric_VERSION=${WeightedPathFabric_VERSION}")
message(STATUS "WeightedPathFabric_WIRE_PROTOCOL_VERSION=${WeightedPathFabric_WIRE_PROTOCOL_VERSION}")
message(STATUS "WeightedPathFabric_PERSISTENCE_FORMAT_VERSION=${WeightedPathFabric_PERSISTENCE_FORMAT_VERSION}")
