// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Internal engine implementation shared by the topology engine, the reconciler and the
// persistence layer. Not installed and not part of the public API.

#ifndef FABRIC_TOPOLOGY_SRC_ENGINE_IMPL_HPP
#define FABRIC_TOPOLOGY_SRC_ENGINE_IMPL_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fabric_topology/topology.hpp"
#include "graph_state.hpp"

namespace fabric_topology::internal {

/// A normalized relationship candidate, produced from an AddRelationshipRequest or from one
/// observed relationship inside a publication. Both entry points run exactly the same
/// validation.
struct RelationshipCandidate {
    TopologyEdgeId id;
    RelationshipId relationship_id;
    RelationClass relation = RelationClass::Unknown;
    TopologyNodeId from;
    TopologyNodeId to;
    TopologyLayer layer = TopologyLayer::Physical;
    TopologyDomainId domain;
    bool cross_domain = false;
    TopologyDomainId secondary_domain;
    EvidenceGeneration evidence_generation;
    std::optional<TopologyEdgeId> supported_by;
    std::optional<AttachmentId> attachment;
    bool exclusive_attachment = false;
    std::string source_relationship_id;
    Metadata metadata;
    PublicationType evidence_type = PublicationType::Unsupported;
    DiscoverySource source = DiscoverySource::Unknown;
};

/// Outcome of validating one relationship against authoritative state.
struct RelationshipValidation {
    Status status;
    bool ok = false;
};

/// Validate one relationship candidate against a graph state. Performs every structural
/// check: relation semantics, endpoint existence and lifecycle, endpoint class pairing,
/// layer policy, self-edges, scope/domain rules and containment-style acyclicity.
[[nodiscard]] RelationshipValidation validate_relationship_candidate(const GraphState& state,
                                                                     const RelationshipCandidate& candidate,
                                                                     bool is_new);

/// Resolve the authority scope of a relationship from its endpoints and the request.
struct DomainResolution {
    Status status;
    bool ok = false;
    TopologyDomainId domain;
    bool cross_domain = false;
    TopologyDomainId secondary_domain;
};

[[nodiscard]] DomainResolution resolve_relationship_domain(const GraphState& state,
                                                           const TopologyDomainId& from_domain,
                                                           const TopologyDomainId& to_domain,
                                                           const TopologyDomainId& requested,
                                                           RelationClass relation,
                                                           bool allow_cross_domain);

/// Bounded record of recently processed mutation attempts, used to recognise a replay of an
/// already-applied attempt instead of applying it twice.
class AttemptLedger {
public:
    static constexpr std::size_t kCapacity = 8192;

    void record(const std::string& key);
    [[nodiscard]] bool seen(const std::string& key) const;

private:
    std::unordered_set<std::string> keys_;
    std::deque<std::string> order_;
};

}  // namespace fabric_topology::internal

namespace fabric_topology {

/// Pimpl payload of TopologyEngine. Defined here so the reconciler and the persistence layer
/// can share it without exposing internals through the installed headers.
struct TopologyEngine::Impl {
    explicit Impl(TopologyEngineOptions options_in);

    TopologyEngineOptions options;
    Limits limits;
    std::shared_ptr<const IEntityDirectory> directory;

    /// Guards the authoritative graph. Reads take it shared, mutations exclusive.
    mutable std::shared_mutex state_mutex;
    /// Guards publisher authority and the coordinator epoch. Always acquired before
    /// state_mutex, and never acquired while state_mutex is held.
    mutable std::mutex authority_mutex;
    /// Guards the durable image. Always acquired last.
    mutable std::mutex persistence_mutex;

    internal::GraphState graph;
    CoordinatorEpoch coordinator_epoch;
    std::unordered_map<std::string, PublisherState> publishers;
    /// Worker boot identifiers that are permanently stale for this runtime.
    std::unordered_set<std::string> fenced_boots;
    internal::AttemptLedger attempts;

    std::vector<TopologySnapshot> snapshots;

    std::uint64_t committed_mutations = 0;
    std::uint64_t idempotent_mutations = 0;
    std::uint64_t rejected_mutations = 0;
    std::uint64_t generation_advances = 0;
    std::uint64_t publications_committed = 0;
    std::uint64_t publications_rejected = 0;

    // -- authority (authority_mutex must be held) -------------------------
    [[nodiscard]] Status check_authority_locked(const AuthorityContext& authority,
                                                const TopologyDomainId& domain, GrantMode required,
                                                PublisherState** out_state);
    [[nodiscard]] bool boot_is_fenced_locked(std::string_view boot) const;

    // -- durability ------------------------------------------------------
    /// Persist the current graph. persistence_mutex is taken internally; state_mutex must
    /// already be held exclusively by the caller so the written image is consistent.
    [[nodiscard]] Status persist_locked() const;
    [[nodiscard]] Status persist_to_locked(const std::string& path) const;

    // -- statistics ------------------------------------------------------
    void note_committed() noexcept { ++committed_mutations; }
    void note_idempotent() noexcept { ++idempotent_mutations; }
    void note_rejected() noexcept { ++rejected_mutations; }
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_SRC_ENGINE_IMPL_HPP
