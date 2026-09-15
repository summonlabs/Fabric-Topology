// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_TOPOLOGY_HPP
#define FABRIC_TOPOLOGY_TOPOLOGY_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fabric_topology/authority.hpp"
#include "fabric_topology/diff.hpp"
#include "fabric_topology/limits.hpp"
#include "fabric_topology/mutations.hpp"
#include "fabric_topology/persistence.hpp"
#include "fabric_topology/publication.hpp"
#include "fabric_topology/registry.hpp"
#include "fabric_topology/snapshot.hpp"
#include "fabric_topology/traversal.hpp"

namespace fabric_topology {

/// Filter applied to edge queries. Defaults return every relationship, including non-current
/// ones, because UNKNOWN must remain observable.
struct EdgeFilter {
    std::optional<RelationClass> relation;
    std::optional<TopologyLayer> layer;
    std::optional<TopologyDomainId> domain;
    std::optional<LifecycleState> lifecycle;
    std::optional<PublisherId> publisher;
    /// When false only LifecycleState::Current relationships are returned.
    bool include_non_current = true;

    [[nodiscard]] static EdgeFilter current_only() noexcept;
};

struct TopologyEngineOptions {
    Limits limits{};
    /// Fabric Registry dependency. When null, every entity reference resolves as unknown and
    /// no node can be added: canonical identity is never invented locally.
    std::shared_ptr<const IEntityDirectory> directory;
    /// Durable image path. Empty keeps the runtime purely in-memory.
    std::string persistence_path;
    PersistenceMode persistence_mode = PersistenceMode::Manual;
    /// Self-check indexes against the authoritative graph after every committed mutation.
    /// Intended for tests and diagnostics; it is O(N) and off by default.
    bool verify_indexes_on_mutation = false;
    /// Coordinator epoch the runtime starts with. A durable recovery raises it.
    CoordinatorEpoch initial_coordinator_epoch{1};
    /// Number of historical snapshots retained for diff/inspection. Old snapshots stay valid
    /// values; this only bounds the engine-side history.
    std::size_t snapshot_history = 8;
};

/// The authoritative topology runtime.
///
/// Thread safety: every public method is safe to call from any thread. Reads take a shared
/// lock; mutations take the authority lock followed by the exclusive state lock, in that
/// order, for the shortest span that keeps state consistent. No callback supplied by a
/// caller is ever invoked while an internal lock is held, and no mutable internal reference
/// escapes: queries return values.
///
/// Ownership: the engine owns its graph. Snapshots own their content. Both are independent of
/// the engine's lifetime once returned.
///
/// Generation semantics: topology_generation() advances by exactly one when authoritative
/// topology state actually changes. An idempotent replay never advances it.
///
/// Stale-state semantics: a mutation whose expected generation, publisher authority, worker
/// boot or coordinator epoch is out of date is rejected before any state is touched.
class TopologyEngine {
public:
    /// Opaque implementation payload. Declared here so the library implementation files can
    /// share it; it carries no public API surface and is never exposed to consumers.
    struct Impl;

    explicit TopologyEngine(TopologyEngineOptions options);
    ~TopologyEngine();

    TopologyEngine(const TopologyEngine&) = delete;
    TopologyEngine& operator=(const TopologyEngine&) = delete;
    TopologyEngine(TopologyEngine&&) = delete;
    TopologyEngine& operator=(TopologyEngine&&) = delete;

    /// Open an engine, conservatively recovering durable topology when persistence_path is
    /// set and the file exists. Recovery never restores live authority and never marks
    /// recovered relationships current.
    [[nodiscard]] static std::unique_ptr<TopologyEngine> open(TopologyEngineOptions options, LoadReport& report);

    // -----------------------------------------------------------------------
    // Domain administration. These are engine-owner operations, not publisher
    // operations: a publisher can never widen its own authority scope.
    // -----------------------------------------------------------------------
    [[nodiscard]] Status define_domain(const DomainDefinition& definition);
    [[nodiscard]] Status define_cross_domain_rule(const CrossDomainRule& rule);
    [[nodiscard]] std::optional<DomainDefinition> domain(const TopologyDomainId& id) const;
    [[nodiscard]] std::vector<DomainDefinition> domains() const;

    // -----------------------------------------------------------------------
    // Authority administration.
    // -----------------------------------------------------------------------
    [[nodiscard]] Status register_publisher(const PublisherRegistration& registration);
    /// Permanently fence one publisher incarnation. The fenced WorkerBootId stays stale for
    /// the lifetime of the runtime and can never mutate topology again.
    [[nodiscard]] Status fence_publisher(const PublisherId& publisher, const WorkerBootId& boot, std::string_view reason);
    /// Fence every publisher holding this boot id. Used when a worker process is observed to
    /// have died without a graceful shutdown.
    [[nodiscard]] std::size_t fence_boot(const WorkerBootId& boot, std::string_view reason, bool include_unregistered = false);
    [[nodiscard]] std::optional<PublisherState> publisher_state(const PublisherId& publisher) const;
    [[nodiscard]] std::vector<PublisherState> publishers() const;

    /// Advance the coordinator epoch after a coordinator restart. Every live publisher
    /// registration is invalidated, and every dynamic (published) relationship is moved to
    /// RevalidationRequired without changing its durable identity.
    [[nodiscard]] CoordinatorEpoch advance_coordinator_epoch(std::string_view reason);
    [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept;

    // -----------------------------------------------------------------------
    // Mutations. Each takes the authority context of the caller and returns a structured
    // outcome plus the (unchanged or advanced) topology generation.
    // -----------------------------------------------------------------------
    [[nodiscard]] MutationResult add_node(const AuthorityContext& authority, const AddNodeRequest& request);
    [[nodiscard]] MutationResult remove_node(const AuthorityContext& authority, const RemoveNodeRequest& request);
    [[nodiscard]] MutationResult add_relationship(const AuthorityContext& authority, const AddRelationshipRequest& request);
    [[nodiscard]] MutationResult update_relationship_evidence(const AuthorityContext& authority, const UpdateEvidenceRequest& request);
    [[nodiscard]] MutationResult supersede_relationship(const AuthorityContext& authority, const SupersedeRequest& request);
    [[nodiscard]] MutationResult retire_relationship(const AuthorityContext& authority, const RetireRequest& request);
    [[nodiscard]] MutationResult move_attachment(const AuthorityContext& authority, const MoveAttachmentRequest& request);
    [[nodiscard]] MutationResult replace_endpoint_generation(const AuthorityContext& authority, const ReplaceEndpointGenerationRequest& request);
    [[nodiscard]] MutationResult attach_endpoint(const AuthorityContext& authority, const AttachEndpointRequest& request);
    [[nodiscard]] MutationResult detach_endpoint(const AuthorityContext& authority, const DetachEndpointRequest& request);
    [[nodiscard]] MutationResult revalidate_relationship(const AuthorityContext& authority, const RevalidateRequest& request);
    [[nodiscard]] PublicationResult publish(const AuthorityContext& authority, const Publication& publication);

    // -----------------------------------------------------------------------
    // Queries. All return values; no internal container is exposed.
    // -----------------------------------------------------------------------
    [[nodiscard]] TopologyGeneration generation() const noexcept;
    [[nodiscard]] std::size_t node_count() const noexcept;
    [[nodiscard]] std::size_t edge_count() const noexcept;

    [[nodiscard]] std::optional<TopologyNode> node(const TopologyNodeId& id) const;
    [[nodiscard]] std::optional<TopologyEdge> edge(const TopologyEdgeId& id) const;
    [[nodiscard]] std::optional<TopologyNode> node_for_entity(std::string_view entity_id) const;
    [[nodiscard]] std::optional<TopologyEdge> relationship(const RelationshipId& id) const;

    [[nodiscard]] std::vector<TopologyEdge> edges_for_node(const TopologyNodeId& id, const EdgeFilter& filter = {}) const;
    [[nodiscard]] std::vector<TopologyNodeId> neighbors(const TopologyNodeId& id, const EdgeFilter& filter = {}) const;
    [[nodiscard]] std::vector<TopologyEdge> edges_by_relation(RelationClass relation, const EdgeFilter& filter = {}) const;
    [[nodiscard]] std::vector<TopologyEdge> edges_in_domain(const TopologyDomainId& domain, const EdgeFilter& filter = {}) const;
    [[nodiscard]] std::vector<TopologyNodeId> domain_members(const TopologyDomainId& domain) const;
    [[nodiscard]] std::vector<TopologyEdge> edges_from_publisher(const PublisherId& publisher, const EdgeFilter& filter = {}) const;
    [[nodiscard]] std::vector<TopologyEdge> physical_relationships(const EdgeFilter& filter = {}) const;
    [[nodiscard]] std::vector<TopologyEdge> logical_relationships(const EdgeFilter& filter = {}) const;
    [[nodiscard]] std::vector<TopologyNode> all_nodes() const;
    [[nodiscard]] std::vector<TopologyEdge> all_edges() const;
    [[nodiscard]] std::vector<TopologyNodeId> entity_members(const std::string& anchor_entity_id, RelationClass relation) const;

    [[nodiscard]] TraversalResult traverse(const TraversalRequest& request) const;

    // -----------------------------------------------------------------------
    // Snapshots, digests, diffs.
    // -----------------------------------------------------------------------
    [[nodiscard]] TopologySnapshot snapshot(const SnapshotScope& scope = {}) const;
    [[nodiscard]] std::string digest(const SnapshotScope& scope = {}) const;
    [[nodiscard]] SnapshotCurrentness check_snapshot(const TopologySnapshot& snapshot) const;
    [[nodiscard]] TopologyDiff diff(const TopologySnapshot& before, const TopologySnapshot& after) const;
    [[nodiscard]] TopologyDiff diff_against_current(const TopologySnapshot& before) const;
    [[nodiscard]] std::optional<TopologySnapshot> snapshot_history(std::size_t index) const;
    [[nodiscard]] std::size_t snapshot_count() const;

    // -----------------------------------------------------------------------
    // Validation and explanations.
    // -----------------------------------------------------------------------
    [[nodiscard]] ValidationReport validate(const SnapshotScope& scope = {}) const;
    [[nodiscard]] Status explain_relationship(const TopologyEdgeId& id, Explanation& out) const;
    [[nodiscard]] Status explain_node(const TopologyNodeId& id, Explanation& out) const;
    [[nodiscard]] Status explain_generation(TopologyGeneration generation, Explanation& out) const;

    // -----------------------------------------------------------------------
    // Persistence.
    // -----------------------------------------------------------------------
    [[nodiscard]] Status save() const;
    [[nodiscard]] Status save_to(const std::string& path) const;
    [[nodiscard]] LoadReport load_from(const std::string& path);
    [[nodiscard]] const std::string& persistence_path() const noexcept;

    // -----------------------------------------------------------------------
    // Introspection.
    // -----------------------------------------------------------------------
    [[nodiscard]] const Limits& limits() const noexcept;
    [[nodiscard]] std::string render_canonical(const SnapshotScope& scope = {}) const;
    [[nodiscard]] std::string statistics() const;
    /// Full internal consistency check across authoritative state and every index.
    [[nodiscard]] Status verify_integrity() const;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_TOPOLOGY_HPP
