// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_EDGE_HPP
#define FABRIC_TOPOLOGY_EDGE_HPP

#include <optional>
#include <string>

#include "fabric_topology/ids.hpp"
#include "fabric_topology/metadata.hpp"
#include "fabric_topology/provenance.hpp"
#include "fabric_topology/types.hpp"

namespace fabric_topology {

/// A topology relationship between two topology nodes.
///
/// An edge means: "these endpoints are related by this governed topology relationship under
/// this topology generation and current authority". It does NOT mean the link is up, the
/// path is permitted, capacity is available or traffic can pass. Operational condition,
/// reachability, routing and path authority belong to other runtimes.
struct TopologyEdge {
    TopologyEdgeId id;
    /// Stable relationship identity supplied by (or derived from) the asserting authority.
    /// Distinct from the edge id so terminology matches operator expectations.
    RelationshipId relationship_id;
    RelationClass relation = RelationClass::Unknown;
    TopologyNodeId from;
    TopologyNodeId to;
    /// Physical or logical layer. Never ambiguous: the relation class decides the policy and
    /// the edge records the resolved value.
    TopologyLayer layer = TopologyLayer::Physical;
    TopologyDomainId domain;
    /// True when this edge crosses two distinct authority scopes under an explicit,
    /// engine-configured cross-domain rule.
    bool cross_domain = false;
    /// Second scope of a cross-domain edge (equals domain otherwise).
    TopologyDomainId secondary_domain;
    /// Per-edge topology generation: advances when the relationship itself changes.
    EdgeGeneration generation;
    TopologyGeneration created_generation;
    TopologyGeneration last_validated_generation;
    /// Evidence stream position that last justified this relationship.
    EvidenceGeneration evidence_generation;
    /// Registry generations of both endpoints at bind time. A relationship bound to an older
    /// entity generation can never silently become current again.
    EntityGeneration from_entity_generation;
    EntityGeneration to_entity_generation;
    LifecycleState lifecycle = LifecycleState::Unknown;
    /// For logical relationships: the physical relationship this one derives from, when the
    /// publisher supplies one. Fabric Topology never invents the supporting path.
    std::optional<TopologyEdgeId> supported_by;
    /// Set when this relationship was replaced by a newer one.
    std::optional<TopologyEdgeId> superseded_by;
    /// Set for ATTACHED_TO edges that participate in exclusive physical attachment.
    std::optional<AttachmentId> attachment;
    Provenance provenance;
    Metadata metadata;

    friend bool operator==(const TopologyEdge&, const TopologyEdge&) = default;
};

/// Canonical deduplication key for a relationship: relation class plus endpoint pair, with the
/// pair normalized for undirected relations so one structural relationship has exactly one
/// authoritative edge.
struct EdgeKey {
    RelationClass relation = RelationClass::Unknown;
    TopologyNodeId first;
    TopologyNodeId second;
    TopologyDomainId domain;

    friend bool operator==(const EdgeKey&, const EdgeKey&) = default;
};

[[nodiscard]] EdgeKey make_edge_key(RelationClass relation, const TopologyNodeId& from,
                                    const TopologyNodeId& to, const TopologyDomainId& domain);
[[nodiscard]] std::string render_edge_key(const EdgeKey& key);
[[nodiscard]] std::string render_edge(const TopologyEdge& edge);

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_EDGE_HPP
