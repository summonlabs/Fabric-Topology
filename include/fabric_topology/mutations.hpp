// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_MUTATIONS_HPP
#define FABRIC_TOPOLOGY_MUTATIONS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "fabric_topology/diff.hpp"
#include "fabric_topology/metadata.hpp"
#include "fabric_topology/result.hpp"
#include "fabric_topology/types.hpp"

namespace fabric_topology {

struct MutationResult {
    Explanation status;
    TopologyGeneration generation;
    std::optional<TopologyNodeId> node;
    std::optional<TopologyEdgeId> edge;
    TopologyDiff diff;

    /// Committed or Idempotent: the request was accepted and the runtime is consistent.
    [[nodiscard]] bool accepted() const noexcept;
    /// The request actually changed authoritative state (generation advanced).
    [[nodiscard]] bool changed() const noexcept;
    [[nodiscard]] std::string render() const;
};

struct AddNodeRequest {
    /// Optional explicit node id. When empty a deterministic id is derived from the domain and
    /// the canonical entity identity.
    std::optional<TopologyNodeId> node_id;
    std::string entity_id;
    NodeClass node_class = NodeClass::Unknown;
    TopologyTier tier = TopologyTier::Unspecified;
    TopologyDomainId domain;
    TopologyGeneration expected_generation;
    Metadata metadata;

    friend bool operator==(const AddNodeRequest&, const AddNodeRequest&) = default;
};

struct RemoveNodeRequest {
    TopologyNodeId node;
    TopologyGeneration expected_generation;
    /// Relationships must be retired first unless the caller explicitly cascades.
    bool cascade_relationships = false;
    std::string reason;

    friend bool operator==(const RemoveNodeRequest&, const RemoveNodeRequest&) = default;
};

struct AddRelationshipRequest {
    std::optional<TopologyEdgeId> edge_id;
    RelationshipId relationship_id;
    RelationClass relation = RelationClass::Unknown;
    TopologyNodeId from;
    TopologyNodeId to;
    /// Required when the relation class allows both layers; the runtime never guesses.
    std::optional<TopologyLayer> layer;
    /// Empty derives the domain from the endpoints, which must then agree.
    TopologyDomainId domain;
    TopologyGeneration expected_generation;
    EvidenceGeneration evidence_generation;
    std::optional<TopologyEdgeId> supported_by;
    std::optional<AttachmentId> attachment;
    /// Marks an ATTACHED_TO relationship as occupying an exclusive attachment slot.
    bool exclusive_attachment = false;
    /// Set only when an engine-configured cross-domain rule permits this exact relation.
    bool allow_cross_domain = false;
    Metadata metadata;

    friend bool operator==(const AddRelationshipRequest&, const AddRelationshipRequest&) = default;
};

struct UpdateEvidenceRequest {
    TopologyEdgeId edge;
    EvidenceGeneration evidence_generation;
    LifecycleState lifecycle = LifecycleState::Current;
    TopologyGeneration expected_generation;
    EdgeGeneration expected_edge_generation;
    /// When true, replaces the relationship metadata with the supplied metadata.
    bool replace_metadata = false;
    Metadata metadata;
    std::string reason;

    friend bool operator==(const UpdateEvidenceRequest&, const UpdateEvidenceRequest&) = default;
};

struct SupersedeRequest {
    TopologyEdgeId edge;
    /// Optional replacement relationship that takes over semantics.
    std::optional<TopologyEdgeId> replacement;
    TopologyGeneration expected_generation;
    std::string reason;

    friend bool operator==(const SupersedeRequest&, const SupersedeRequest&) = default;
};

struct RetireRequest {
    TopologyEdgeId edge;
    TopologyGeneration expected_generation;
    std::string reason;

    friend bool operator==(const RetireRequest&, const RetireRequest&) = default;
};

/// Atomic attachment move: either the whole move commits or nothing changes. There is no
/// committed intermediate state in which both exclusive physical attachments are current.
struct MoveAttachmentRequest {
    TopologyEdgeId attachment_edge;
    TopologyNodeId new_target;
    TopologyDomainId domain;
    std::optional<TopologyEdgeId> new_edge_id;
    TopologyGeneration expected_generation;
    EdgeGeneration expected_edge_generation;
    EvidenceGeneration evidence_generation;
    AttachmentId attachment;
    Metadata metadata;
    std::string reason;

    friend bool operator==(const MoveAttachmentRequest&, const MoveAttachmentRequest&) = default;
};

/// Bind a node to a new registry entity generation (or to a successor identity). Old
/// relationships do not silently inherit: they become revalidation-required or superseded.
struct ReplaceEndpointGenerationRequest {
    TopologyNodeId node;
    EntityGeneration new_entity_generation;
    /// When non-empty the node is re-bound to this successor canonical identity.
    std::string successor_entity_id;
    bool rebind_identity = false;
    NodeGeneration expected_node_generation;
    TopologyGeneration expected_generation;
    std::string reason;

    friend bool operator==(const ReplaceEndpointGenerationRequest&, const ReplaceEndpointGenerationRequest&) = default;
};

struct AttachEndpointRequest {
    TopologyNodeId endpoint;
    TopologyNodeId target;
    TopologyDomainId domain;
    std::optional<TopologyEdgeId> edge_id;
    AttachmentId attachment;
    bool exclusive = true;
    TopologyGeneration expected_generation;
    EvidenceGeneration evidence_generation;
    Metadata metadata;

    friend bool operator==(const AttachEndpointRequest&, const AttachEndpointRequest&) = default;
};

struct DetachEndpointRequest {
    TopologyNodeId endpoint;
    AttachmentId attachment;
    TopologyGeneration expected_generation;
    std::string reason;

    friend bool operator==(const DetachEndpointRequest&, const DetachEndpointRequest&) = default;
};

struct RevalidateRequest {
    TopologyEdgeId edge;
    EvidenceGeneration evidence_generation;
    TopologyGeneration expected_generation;
    std::string reason;

    friend bool operator==(const RevalidateRequest&, const RevalidateRequest&) = default;
};

/// Structural validation report for a scope.
struct ValidationIssue {
    Outcome outcome = Outcome::StructuralInvariantViolation;
    std::string code;
    TopologyNodeId node;
    TopologyEdgeId edge;
    std::string detail;
};

struct ValidationReport {
    bool valid = true;
    TopologyGeneration generation;
    std::size_t nodes = 0;
    std::size_t edges = 0;
    std::size_t current_edges = 0;
    std::size_t non_current_edges = 0;
    std::vector<ValidationIssue> issues;  // deterministic order: (code, node, edge, detail)

    [[nodiscard]] std::string render() const;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_MUTATIONS_HPP
