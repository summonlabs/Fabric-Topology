// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_DIFF_HPP
#define FABRIC_TOPOLOGY_DIFF_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fabric_topology/ids.hpp"

namespace fabric_topology {

/// Deterministic topology diff classes.
enum class DiffKind : std::uint8_t {
    NodeAdded = 0,
    NodeRemoved = 1,
    NodeGenerationChanged = 2,
    EdgeAdded = 3,
    EdgeRemoved = 4,
    EdgeSuperseded = 5,
    AttachmentMoved = 6,
    RelationshipChanged = 7,
    ScopeChanged = 8,
    CurrentnessChanged = 9,
};

inline constexpr std::uint8_t kDiffKindCount = 10;

[[nodiscard]] const char* to_string(DiffKind value) noexcept;

struct DiffEntry {
    DiffKind kind = DiffKind::NodeAdded;
    /// Stable sort key: node id for node entries, edge id for edge entries.
    std::string key;
    TopologyNodeId node;
    TopologyEdgeId edge;
    std::string before;
    std::string after;

    friend bool operator==(const DiffEntry&, const DiffEntry&) = default;
};

/// Ordered, stable diff. Entry order is fully determined by (kind, key), never by container
/// iteration order.
struct TopologyDiff {
    std::vector<DiffEntry> entries;
    /// Digest over the canonical rendering of entries.
    std::string digest;
    TopologyGeneration from_generation;
    TopologyGeneration to_generation;

    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] std::size_t count(DiffKind kind) const noexcept;
    [[nodiscard]] std::string render() const;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_DIFF_HPP
