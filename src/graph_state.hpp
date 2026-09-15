// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Internal authoritative graph storage. Not installed and not part of the public API.

#ifndef FABRIC_TOPOLOGY_SRC_GRAPH_STATE_HPP
#define FABRIC_TOPOLOGY_SRC_GRAPH_STATE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fabric_topology/authority.hpp"
#include "fabric_topology/codec.hpp"
#include "fabric_topology/diff.hpp"
#include "fabric_topology/edge.hpp"
#include "fabric_topology/limits.hpp"
#include "fabric_topology/node.hpp"
#include "fabric_topology/result.hpp"
#include "fabric_topology/snapshot.hpp"

namespace fabric_topology::internal {

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& key) const noexcept {
        std::size_t seed = key.relation == RelationClass::Unknown ? 0U : static_cast<std::size_t>(key.relation);
        seed = seed * 1099511628211ULL + key.first.hash();
        seed = seed * 1099511628211ULL + key.second.hash();
        seed = seed * 1099511628211ULL + key.domain.hash();
        return seed;
    }
};

struct DomainIndex {
    std::unordered_set<TopologyNodeId> nodes;
    std::unordered_set<TopologyEdgeId> edges;
};

/// Authoritative topology state plus every derived index.
///
/// Every mutation goes through the insert/erase/update helpers below so indexes can never
/// drift from the authoritative maps. verify_indexes() recomputes every index from scratch
/// and compares, and is used by the integrity self-check and the property tests.
class GraphState {
public:
    explicit GraphState(const Limits& limits);

    Limits limits;

    TopologyGeneration generation;
    SnapshotGeneration snapshot_counter;

    std::unordered_map<TopologyNodeId, TopologyNode> nodes;
    std::unordered_map<TopologyEdgeId, TopologyEdge> edges;

    std::unordered_map<std::string, TopologyNodeId> node_by_entity;
    std::unordered_map<EdgeKey, TopologyEdgeId, EdgeKeyHash> edge_by_key;
    std::unordered_map<RelationshipId, TopologyEdgeId> edge_by_relationship;

    /// Every edge incident to a node (both directions), outgoing edges, incoming edges.
    /// Buckets are hash sets: insertion and removal are O(1) regardless of graph density, and
    /// callers always sort before exposing an order.
    std::unordered_map<TopologyNodeId, std::unordered_set<TopologyEdgeId>> incident;
    std::unordered_map<TopologyNodeId, std::unordered_set<TopologyEdgeId>> outgoing;
    std::unordered_map<TopologyNodeId, std::unordered_set<TopologyEdgeId>> incoming;

    std::unordered_map<TopologyDomainId, DomainIndex> domains;
    std::unordered_map<TopologyDomainId, DomainDefinition> domain_definitions;
    std::vector<CrossDomainRule> cross_domain_rules;

    std::unordered_map<std::uint8_t, std::unordered_set<TopologyEdgeId>> edge_by_relation;
    std::unordered_map<std::uint8_t, std::unordered_set<TopologyEdgeId>> edge_by_layer;
    std::unordered_map<std::uint8_t, std::unordered_set<TopologyEdgeId>> edge_by_lifecycle;
    std::unordered_map<std::string, std::unordered_set<TopologyEdgeId>> edge_by_publisher;
    std::unordered_map<std::string, std::unordered_set<TopologyEdgeId>> edge_by_attachment;

    // --- node operations -------------------------------------------------
    bool insert_node(const TopologyNode& node);
    bool erase_node(const TopologyNodeId& id);
    /// Replace a node. The caller must pass the complete new value while the map still holds
    /// the previous one; use mutate_node when the change is expressed as an in-place edit.
    bool update_node(const TopologyNode& node);
    /// Apply an edit to a copy of the stored node and re-index it atomically. This is the
    /// only safe way to change a node that was read out of the graph.
    template <class Fn>
    bool mutate_node(const TopologyNodeId& id, Fn&& edit);

    // --- edge operations -------------------------------------------------
    bool insert_edge(const TopologyEdge& edge);
    bool erase_edge(const TopologyEdgeId& id);
    /// Replace an edge. The caller must pass the complete new value while the map still holds
    /// the previous one; use mutate_edge when the change is expressed as an in-place edit.
    bool update_edge(const TopologyEdge& edge);
    /// Apply an edit to a copy of the stored edge and re-index it atomically. This is the only
    /// safe way to change an edge that was read out of the graph: unindexing always uses the
    /// value that is actually stored, so no index can retain a stale membership.
    template <class Fn>
    bool mutate_edge(const TopologyEdgeId& id, Fn&& edit);

    /// Drop every derived index and rebuild it from `nodes` and `edges`.
    void rebuild_indexes();

    /// Recompute every index and compare it with the maintained one.
    [[nodiscard]] Status verify_indexes() const;

    // --- lookups ---------------------------------------------------------
    [[nodiscard]] const TopologyNode* find_node(const TopologyNodeId& id) const;
    [[nodiscard]] const TopologyEdge* find_edge(const TopologyEdgeId& id) const;
    [[nodiscard]] const TopologyNodeId* find_node_by_entity(std::string_view entity_id) const;
    [[nodiscard]] const TopologyEdgeId* find_edge_by_key(const EdgeKey& key) const;
    [[nodiscard]] const TopologyEdgeId* find_edge_by_relationship(const RelationshipId& id) const;

    [[nodiscard]] bool node_in_scope(const TopologyNode& node, const SnapshotScope& scope) const;
    [[nodiscard]] bool edge_in_scope(const TopologyEdge& edge, const SnapshotScope& scope) const;

    [[nodiscard]] std::vector<TopologyNode> sorted_nodes(const SnapshotScope& scope) const;
    [[nodiscard]] std::vector<TopologyEdge> sorted_edges(const SnapshotScope& scope) const;
    [[nodiscard]] std::vector<TopologyEdgeId> sorted_incident(const TopologyNodeId& id) const;

    [[nodiscard]] bool would_create_cycle(RelationClass relation, const TopologyNodeId& from,
                                          const TopologyNodeId& to) const;

    [[nodiscard]] std::string topology_digest(const SnapshotScope& scope) const;
    [[nodiscard]] std::string render_canonical(const SnapshotScope& scope) const;

private:
    using EdgeBuckets = std::unordered_set<TopologyEdgeId>;

    void index_edge(const TopologyEdge& edge);
    void unindex_edge(const TopologyEdge& edge);
    void index_node(const TopologyNode& node);
    void unindex_node(const TopologyNode& node);

    static void add_to(EdgeBuckets& bucket, const TopologyEdgeId& id) { bucket.insert(id); }
    static void remove_from(EdgeBuckets& bucket, const TopologyEdgeId& id) { bucket.erase(id); }

    /// Insert into a keyed bucket index, creating the bucket when needed.
    template <class Key>
    static void index_into(std::unordered_map<Key, EdgeBuckets>& index, const Key& key,
                           const TopologyEdgeId& id) {
        index[key].insert(id);
    }

    /// Remove from a keyed bucket index and drop the bucket once it becomes empty, so the
    /// maintained index is always identical to a freshly rebuilt one.
    template <class Key>
    static void unindex_from(std::unordered_map<Key, EdgeBuckets>& index, const Key& key,
                             const TopologyEdgeId& id) {
        const auto it = index.find(key);
        if (it == index.end()) {
            return;
        }
        it->second.erase(id);
        if (it->second.empty()) {
            index.erase(it);
        }
    }
};

template <class Fn>
bool GraphState::mutate_node(const TopologyNodeId& id, Fn&& edit) {
    const auto it = nodes.find(id);
    if (it == nodes.end()) {
        return false;
    }
    TopologyNode updated = it->second;
    edit(updated);
    unindex_node(it->second);
    it->second = std::move(updated);
    index_node(it->second);
    return true;
}

template <class Fn>
bool GraphState::mutate_edge(const TopologyEdgeId& id, Fn&& edit) {
    const auto it = edges.find(id);
    if (it == edges.end()) {
        return false;
    }
    TopologyEdge updated = it->second;
    edit(updated);
    unindex_edge(it->second);
    it->second = std::move(updated);
    index_edge(it->second);
    return true;
}

/// Canonical byte encoding of one node / one edge. Transient, process-local incarnation data
/// (publisher, worker boot, coordinator epoch, publication id, evidence stream position and
/// the validation bookkeeping generations) is deliberately excluded, so two independently
/// built encodings of the same authoritative topology produce identical bytes.
void canonical_encode_node(const TopologyNode& node, RecordWriter& writer);
void canonical_encode_edge(const TopologyEdge& edge, RecordWriter& writer);

/// Transactional undo log for in-place single-entity mutations.
class StateTxn {
public:
    explicit StateTxn(GraphState& state) : state_(state) {}

    void note_node_inserted(const TopologyNodeId& id);
    void note_node_erased(const TopologyNode& previous);
    void note_node_updated(const TopologyNode& previous);
    void note_edge_inserted(const TopologyEdgeId& id);
    void note_edge_erased(const TopologyEdge& previous);
    void note_edge_updated(const TopologyEdge& previous);
    void note_generation(TopologyGeneration previous);
    void note_domain(const TopologyDomainId& id, bool existed);

    void rollback();
    void commit() noexcept { active_ = false; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    enum class Kind : std::uint8_t {
        NodeInserted,
        NodeErased,
        NodeUpdated,
        EdgeInserted,
        EdgeErased,
        EdgeUpdated,
        GenerationChanged,
        DomainCreated,
    };

    struct Entry {
        Kind kind;
        TopologyNodeId node_id;
        TopologyEdgeId edge_id;
        TopologyNode node;
        TopologyEdge edge;
        TopologyGeneration generation;
        TopologyDomainId domain;
    };

    GraphState& state_;
    std::vector<Entry> entries_;
    bool active_ = true;
};

// -- shared validation helpers used by the engine and the reconciler ---------

/// Canonical endpoint order for a relation: undirected relationships are stored in a single
/// normalized direction so one semantic edge can never exist twice.
void normalize_undirected_endpoints(RelationClass relation, TopologyNodeId& from, TopologyNodeId& to);

/// Effective layer for a relation, given an optional caller-supplied layer.
[[nodiscard]] std::optional<TopologyLayer> resolve_layer(RelationClass relation,
                                                         std::optional<TopologyLayer> requested);

/// Deterministic node identifier derived from domain and canonical entity identity. Replays
/// of the same observation therefore converge on the same node.
[[nodiscard]] TopologyNodeId derive_node_id(const TopologyDomainId& domain, std::string_view entity_id);

/// Deterministic edge identifier derived from the canonical relationship key.
[[nodiscard]] TopologyEdgeId derive_edge_id(const EdgeKey& key);

[[nodiscard]] std::string derive_relationship_id(const EdgeKey& key);

/// Render a topology generation deterministically for explanations.
[[nodiscard]] std::string render_generation(TopologyGeneration generation);

}  // namespace fabric_topology::internal

#endif  // FABRIC_TOPOLOGY_SRC_GRAPH_STATE_HPP
