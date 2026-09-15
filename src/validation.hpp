// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_SRC_VALIDATION_HPP
#define FABRIC_TOPOLOGY_SRC_VALIDATION_HPP

#include "fabric_topology/mutations.hpp"
#include "fabric_topology/registry.hpp"
#include "fabric_topology/snapshot.hpp"
#include "graph_state.hpp"

namespace fabric_topology::internal {

/// Structural validation of a graph state: every invariant the runtime enforces is audited
/// here. Used both by TopologyEngine::validate and by the reconciler, which validates the
/// whole candidate graph before an authoritative snapshot is allowed to commit.
[[nodiscard]] ValidationReport validate_graph(const GraphState& state, const IEntityDirectory* directory,
                                              const SnapshotScope& scope);

}  // namespace fabric_topology::internal

#endif  // FABRIC_TOPOLOGY_SRC_VALIDATION_HPP
