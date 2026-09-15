// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_VERSION_HPP
#define FABRIC_TOPOLOGY_VERSION_HPP

#include <string>

namespace fabric_topology {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr const char* kVersionString = "1.0.0";
inline constexpr const char* kProductName = "Fabric Topology";
inline constexpr const char* kVendorName = "Summon Software Labs";
inline constexpr const char* kLibraryNamespace = "SummonSoftwareLabs";

/// Deterministic textual version, e.g. "1.0.0".
[[nodiscard]] std::string version_string();

/// Deterministic product banner, e.g. "Fabric Topology 1.0.0 (Summon Software Labs)".
[[nodiscard]] std::string product_banner();

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_VERSION_HPP
