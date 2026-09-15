// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_TRAVERSAL_HPP
#define FABRIC_TOPOLOGY_TRAVERSAL_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "fabric_topology/ids.hpp"
#include "fabric_topology/result.hpp"
#include "fabric_topology/types.hpp"

namespace fabric_topology {

/// Structural graph traversal only. These operations inspect the governed topology graph.
/// They deliberately do NOT compute network paths: routing, constraints, cost, ECMP,
/// traffic engineering and path authority belong to later runtimes in the stack.
enum class TraversalKind : std::uint8_t {
    /// One or more hops in either direction over current relationships.
    Neighbors = 0,
    /// Follow CONTAINS / MEMBER_OF / HOSTED_BY edges toward their parents.
    Ancestors = 1,
    /// Follow CONTAINS / MEMBER_OF / HOSTED_BY edges toward their children.
    Descendants = 2,
    /// Whole connected component over a chosen relation set.
    ConnectedComponent = 3,
    /// Follow ATTACHED_TO / PRESENTS_ENDPOINT / CONNECTED_TO structure outward.
    PhysicalAttachmentChain = 4,
    /// Follow BACKED_BY / TUNNELED_OVER toward the supporting structure.
    LogicalDependencyChain = 5,
};

inline constexpr std::uint8_t kTraversalKindCount = 6;

[[nodiscard]] const char* to_string(TraversalKind value) noexcept;
[[nodiscard]] std::optional<TraversalKind> traversal_kind_from_string(std::string_view text) noexcept;

struct TraversalRequest {
    TraversalKind kind = TraversalKind::Neighbors;
    TopologyNodeId origin;
    std::size_t max_depth = 1;
    std::size_t max_visited = 1000;
    std::optional<RelationClass> relation;
    std::optional<TopologyLayer> layer;
    /// Non-current relationships are skipped by default. UNKNOWN and
    /// REVALIDATION_REQUIRED relationships are never silently treated as current.
    bool include_non_current = false;

    friend bool operator==(const TraversalRequest&, const TraversalRequest&) = default;
};

struct TraversalStep {
    TopologyNodeId node;
    std::size_t depth = 0;
    TopologyEdgeId via;
    RelationClass relation = RelationClass::Unknown;

    friend bool operator==(const TraversalStep&, const TraversalStep&) = default;
};

struct TraversalResult {
    Explanation status;
    TraversalKind kind = TraversalKind::Neighbors;
    TopologyNodeId origin;
    /// Ordered by (depth, node id): deterministic regardless of traversal interleaving.
    std::vector<TraversalStep> steps;
    bool truncated = false;

    [[nodiscard]] std::string render() const;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_TRAVERSAL_HPP
