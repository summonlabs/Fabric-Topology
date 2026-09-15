// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_NODE_HPP
#define FABRIC_TOPOLOGY_NODE_HPP

#include <string>

#include "fabric_topology/ids.hpp"
#include "fabric_topology/metadata.hpp"
#include "fabric_topology/provenance.hpp"
#include "fabric_topology/types.hpp"

namespace fabric_topology {

/// A topology node: a reference to a canonical Fabric Registry identity, placed in the
/// governed topology graph at a specific entity generation.
///
/// ENTITY EXISTS (registry) / ENTITY PARTICIPATES IN CURRENT TOPOLOGY (this node's
/// lifecycle) / TOPOLOGY RELATIONSHIP IS CURRENT (each edge's lifecycle) are three separate
/// facts and are represented separately.
struct TopologyNode {
    TopologyNodeId id;
    /// Canonical Fabric Registry identity referenced by this node.
    std::string entity_id;
    EntityClass entity_class = EntityClass::Unknown;
    /// Registry generation of that identity at the time the node was bound.
    EntityGeneration entity_generation;
    NodeClass node_class = NodeClass::Unknown;
    TopologyTier tier = TopologyTier::Unspecified;
    /// Authority scope this node belongs to.
    TopologyDomainId domain;
    /// Per-node topology generation: advances when node topology state changes.
    NodeGeneration generation;
    TopologyGeneration created_generation;
    TopologyGeneration last_validated_generation;
    /// Currentness of this node in the authoritative topology.
    LifecycleState lifecycle = LifecycleState::Unknown;
    Provenance provenance;
    Metadata metadata;

    friend bool operator==(const TopologyNode&, const TopologyNode&) = default;
};

[[nodiscard]] std::string render_node(const TopologyNode& node);

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_NODE_HPP
