// Weighted Path Fabric - version and representation version reporting.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>

namespace wpf {

/// Product version. Kept coherent with the CMake project() version and the CLI.
inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr const char* kVersionString = "1.0.0";

/// Independently versioned representation contracts. These do NOT advance merely
/// because the product version advances.
inline constexpr std::uint16_t kWireProtocolVersion = 1;
inline constexpr std::uint16_t kPersistenceFormatVersion = 1;
inline constexpr std::uint16_t kNormalizationSemanticsVersion = 1;
inline constexpr std::uint16_t kApportionmentAlgorithmVersion = 1;
inline constexpr std::uint16_t kAssignmentEncodingVersion = 1;

/// Name of the apportionment method implemented by this runtime.
inline constexpr const char* kApportionmentAlgorithmName = "largest-remainder-hamilton";

/// Deterministic multi-line report of every versioned contract.
std::string version_report();

}  // namespace wpf
