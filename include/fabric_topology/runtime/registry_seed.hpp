// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_RUNTIME_REGISTRY_SEED_HPP
#define FABRIC_TOPOLOGY_RUNTIME_REGISTRY_SEED_HPP

#include <string>
#include <vector>

#include "fabric_topology/registry.hpp"
#include "fabric_topology/result.hpp"

namespace fabric_topology::runtime {

/// File-backed Fabric Registry seed.
///
/// Fabric Topology never mints canonical identity: it consumes it. In a real deployment the
/// identity authority is Fabric Registry, reached through IEntityDirectory. This adapter is
/// the shipped stand-in used by the tools, the examples and the multi-process proofs, so a
/// coordinator process can hold the same canonical identities its publishers reference.
///
/// Format (deterministic UTF-8 text, one record per line):
///   entity <identifier> <CLASS> <generation> [superseded-by <identifier>] [retired]
/// Lines starting with '#' and blank lines are ignored. Unknown classes, malformed
/// identifiers and duplicate identities are rejected.
[[nodiscard]] Status load_registry_seed(const std::string& path, IEntityDirectory& directory);

/// Write canonical identities in the format above. Output is fully deterministic.
[[nodiscard]] Status write_registry_seed(const std::string& path,
                                         const std::vector<EntityRecord>& records);

}  // namespace fabric_topology::runtime

#endif  // FABRIC_TOPOLOGY_RUNTIME_REGISTRY_SEED_HPP
