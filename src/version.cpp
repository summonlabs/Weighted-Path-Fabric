// Weighted Path Fabric - version reporting.
// Copyright 2026 Summon Software Labs.
#include "wpf/version.hpp"

#include <string>

namespace wpf {
namespace {
std::string line(const char* key, const std::string& value) {
  std::string out = "  ";
  out += key;
  out += ": ";
  out += value;
  out += "\n";
  return out;
}
}  // namespace

std::string version_report() {
  std::string out = "Weighted Path Fabric ";
  out += kVersionString;
  out += "\n";
  out += line("product_version", kVersionString);
  out += line("wire_protocol_version", std::to_string(kWireProtocolVersion));
  out += line("persistence_format_version", std::to_string(kPersistenceFormatVersion));
  out += line("normalization_semantics_version", std::to_string(kNormalizationSemanticsVersion));
  out += line("apportionment_algorithm", std::string(kApportionmentAlgorithmName) + " v" +
                                          std::to_string(kApportionmentAlgorithmVersion));
  out += line("assignment_encoding_version", std::to_string(kAssignmentEncodingVersion));
  return out;
}

}  // namespace wpf
