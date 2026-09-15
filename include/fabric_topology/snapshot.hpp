// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_SNAPSHOT_HPP
#define FABRIC_TOPOLOGY_SNAPSHOT_HPP

#include <string>
#include <vector>

#include "fabric_topology/edge.hpp"
#include "fabric_topology/ids.hpp"
#include "fabric_topology/node.hpp"
#include "fabric_topology/types.hpp"

namespace fabric_topology {

/// Scope selector for snapshots, digests, validation and canonical dumps.
struct SnapshotScope {
    /// Whole runtime topology (all scopes).
    bool whole_topology = true;
    /// Restrict to one domain when whole_topology is false.
    TopologyDomainId domain;
    /// Optional additional restriction to one relation class.
    bool filter_relation = false;
    RelationClass relation = RelationClass::Unknown;
    /// Optional additional restriction to one layer.
    bool filter_layer = false;
    TopologyLayer layer = TopologyLayer::Physical;

    [[nodiscard]] static SnapshotScope all() noexcept { return SnapshotScope{}; }
    [[nodiscard]] static SnapshotScope of_domain(TopologyDomainId domain);
    [[nodiscard]] static SnapshotScope of_relation(RelationClass relation);
    [[nodiscard]] static SnapshotScope of_layer(TopologyLayer layer);

    friend bool operator==(const SnapshotScope&, const SnapshotScope&) = default;
};

/// Immutable, self-contained view of authoritative topology at one generation.
///
/// The snapshot owns copies of every node and edge it contains; it never aliases internal
/// mutable state. Old snapshots remain inspectable forever but never masquerade as current:
/// currentness is decided by check_snapshot() against the live engine.
struct TopologySnapshot {
    TopologySnapshotId id;
    /// Ordinal of this snapshot within the runtime, starting at 1.
    SnapshotGeneration snapshot_generation;
    TopologyGeneration topology_generation;
    CoordinatorEpoch coordinator_epoch;
    SnapshotScope scope;
    /// Deterministic digest of exactly the content below.
    std::string digest;
    std::vector<TopologyNode> nodes;  // sorted by node id
    std::vector<TopologyEdge> edges;  // sorted by edge id

    [[nodiscard]] std::size_t node_count() const noexcept { return nodes.size(); }
    [[nodiscard]] std::size_t edge_count() const noexcept { return edges.size(); }

    /// Deterministic rendering of the snapshot content.
    [[nodiscard]] std::string render() const;
};

/// Why a snapshot is or is not current relative to a live engine.
enum class SnapshotVerdict : std::uint8_t {
    Current = 0,
    OlderGeneration = 1,
    NewerGeneration = 2,
    DifferentCoordinatorEpoch = 3,
    ScopeMismatch = 4,
    DigestMismatch = 5,
};

[[nodiscard]] const char* to_string(SnapshotVerdict value) noexcept;

struct SnapshotCurrentness {
    SnapshotVerdict verdict = SnapshotVerdict::OlderGeneration;
    bool current = false;
    std::string detail;

    [[nodiscard]] std::string render() const;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_SNAPSHOT_HPP
