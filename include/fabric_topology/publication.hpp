// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_PUBLICATION_HPP
#define FABRIC_TOPOLOGY_PUBLICATION_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "fabric_topology/authority.hpp"
#include "fabric_topology/diff.hpp"
#include "fabric_topology/edge.hpp"
#include "fabric_topology/metadata.hpp"
#include "fabric_topology/node.hpp"
#include "fabric_topology/result.hpp"

namespace fabric_topology {

/// One observed topology node inside a publication. Observations are assertions by a
/// publisher, not authoritative state: they are validated, scoped and generation-fenced
/// before they can influence authoritative topology.
struct ObservedNode {
    /// Optional publisher-chosen node id. When empty the runtime derives a deterministic id
    /// from domain + entity identity, so replays converge on the same node.
    TopologyNodeId node_id;
    std::string entity_id;
    NodeClass node_class = NodeClass::Unknown;
    TopologyTier tier = TopologyTier::Unspecified;
    /// Empty means "the publication's domain".
    TopologyDomainId domain;
    EntityGeneration entity_generation;
    Metadata metadata;
    /// false declares the observation that this participant is absent from the scope.
    bool present = true;

    friend bool operator==(const ObservedNode&, const ObservedNode&) = default;
};

/// One observed relationship inside a publication.
struct ObservedEdge {
    std::optional<TopologyEdgeId> edge_id;
    RelationshipId relationship_id;
    RelationClass relation = RelationClass::Unknown;
    TopologyNodeId from;
    TopologyNodeId to;
    /// Empty means "derive from the relation class policy".
    std::optional<TopologyLayer> layer;
    /// Empty means "the publication's domain".
    TopologyDomainId domain;
    EvidenceGeneration evidence_generation;
    std::string source_relationship_id;
    std::optional<TopologyEdgeId> supported_by;
    std::optional<AttachmentId> attachment;
    bool exclusive_attachment = false;
    Metadata metadata;
    bool present = true;

    friend bool operator==(const ObservedEdge&, const ObservedEdge&) = default;
};

/// A publisher's assertion about part or all of one authority scope.
struct Publication {
    PublicationId id;
    /// Scope this publication claims. Must be granted to the publisher.
    TopologyDomainId domain;
    ScopeKind scope_kind = ScopeKind::AdministrativeDomain;
    PublicationMode mode = PublicationMode::Incremental;
    PublicationType type = PublicationType::Unsupported;
    DiscoverySource source = DiscoverySource::Unknown;
    /// Topology generation the publisher believes it is building on. Checked before mutation
    /// for optimistic-concurrency modes.
    TopologyGeneration expected_generation;
    EvidenceGeneration evidence_generation;
    /// Explicitly declared absences, honoured only for AuthoritativeWrite grants.
    std::vector<RelationshipId> declared_absent_relationships;
    std::vector<ObservedNode> nodes;
    std::vector<ObservedEdge> edges;
    std::string note;

    friend bool operator==(const Publication&, const Publication&) = default;
};

/// Deterministic outcome of a publication, including the derived diff.
struct PublicationResult {
    Explanation status;
    TopologyGeneration generation;
    TopologyDiff diff;
    std::size_t nodes_added = 0;
    std::size_t nodes_updated = 0;
    std::size_t nodes_removed = 0;
    std::size_t nodes_unchanged = 0;
    std::size_t edges_added = 0;
    std::size_t edges_updated = 0;
    std::size_t edges_removed = 0;
    std::size_t edges_superseded = 0;
    std::size_t edges_unchanged = 0;
    std::size_t edges_conflicted = 0;
    std::size_t edges_rejected = 0;
    bool generation_advanced = false;

    [[nodiscard]] bool committed() const noexcept;
    [[nodiscard]] std::string render() const;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_PUBLICATION_HPP
