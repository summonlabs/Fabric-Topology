// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Bounded structural graph traversal. This is deliberately not path computation: no routing,
// no cost model, no constraints and no forwarding decision is expressed or implied.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "engine_impl.hpp"
#include "fabric_topology/topology.hpp"
#include "graph_state.hpp"

namespace fabric_topology {

namespace {

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] bool relation_in(TraversalKind kind, RelationClass relation) {
    switch (kind) {
        case TraversalKind::Neighbors:
        case TraversalKind::ConnectedComponent:
            return true;
        case TraversalKind::Ancestors:
        case TraversalKind::Descendants:
            return relation == RelationClass::Contains || relation == RelationClass::MemberOf ||
                   relation == RelationClass::HostedBy ||
                   relation == RelationClass::FabricMembership;
        case TraversalKind::PhysicalAttachmentChain:
            return relation == RelationClass::AttachedTo ||
                   relation == RelationClass::PresentsEndpoint ||
                   relation == RelationClass::ConnectedTo || relation == RelationClass::UplinkTo ||
                   relation == RelationClass::DownlinkTo || relation == RelationClass::HostedBy;
        case TraversalKind::LogicalDependencyChain:
            return relation == RelationClass::BackedBy || relation == RelationClass::TunneledOver;
    }
    return false;
}

/// Returns the next node when this edge leads away from the current node for this kind.
[[nodiscard]] std::optional<TopologyNodeId> next_node(TraversalKind kind, const TopologyEdge& edge,
                                                      const TopologyNodeId& current) {
    switch (kind) {
        case TraversalKind::Ancestors:
            switch (edge.relation) {
                case RelationClass::Contains:
                    // "a CONTAINS b": the ancestor of b is a.
                    return edge.to == current ? std::optional<TopologyNodeId>{edge.from}
                                              : std::nullopt;
                case RelationClass::MemberOf:
                case RelationClass::HostedBy:
                case RelationClass::FabricMembership:
                    return edge.from == current ? std::optional<TopologyNodeId>{edge.to}
                                                : std::nullopt;
                default:
                    return std::nullopt;
            }
        case TraversalKind::Descendants:
            switch (edge.relation) {
                case RelationClass::Contains:
                    return edge.from == current ? std::optional<TopologyNodeId>{edge.to}
                                                : std::nullopt;
                case RelationClass::MemberOf:
                case RelationClass::HostedBy:
                case RelationClass::FabricMembership:
                    return edge.to == current ? std::optional<TopologyNodeId>{edge.from}
                                              : std::nullopt;
                default:
                    return std::nullopt;
            }
        case TraversalKind::LogicalDependencyChain:
            return edge.from == current ? std::optional<TopologyNodeId>{edge.to} : std::nullopt;
        case TraversalKind::Neighbors:
        case TraversalKind::ConnectedComponent:
        case TraversalKind::PhysicalAttachmentChain:
            if (edge.from == current && edge.to != current) {
                return edge.to;
            }
            if (edge.to == current && edge.from != current) {
                return edge.from;
            }
            return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] bool step_less(const TraversalStep& a, const TraversalStep& b) {
    if (a.depth != b.depth) {
        return a.depth < b.depth;
    }
    return a.node < b.node;
}

}  // namespace

const char* to_string(TraversalKind value) noexcept {
    switch (value) {
        case TraversalKind::Neighbors: return "NEIGHBORS";
        case TraversalKind::Ancestors: return "ANCESTORS";
        case TraversalKind::Descendants: return "DESCENDANTS";
        case TraversalKind::ConnectedComponent: return "CONNECTED_COMPONENT";
        case TraversalKind::PhysicalAttachmentChain: return "PHYSICAL_ATTACHMENT_CHAIN";
        case TraversalKind::LogicalDependencyChain: return "LOGICAL_DEPENDENCY_CHAIN";
    }
    return "NEIGHBORS";
}

std::optional<TraversalKind> traversal_kind_from_string(std::string_view text) noexcept {
    if (text == "NEIGHBORS") return TraversalKind::Neighbors;
    if (text == "ANCESTORS") return TraversalKind::Ancestors;
    if (text == "DESCENDANTS") return TraversalKind::Descendants;
    if (text == "CONNECTED_COMPONENT") return TraversalKind::ConnectedComponent;
    if (text == "PHYSICAL_ATTACHMENT_CHAIN") return TraversalKind::PhysicalAttachmentChain;
    if (text == "LOGICAL_DEPENDENCY_CHAIN") return TraversalKind::LogicalDependencyChain;
    return std::nullopt;
}

std::string TraversalResult::render() const {
    std::string out;
    out.reserve(160 + steps.size() * 48);
    out += "kind=";
    out += to_string(kind);
    out += "\norigin=";
    out += origin.to_string();
    out += "\nsteps=";
    out += std::to_string(steps.size());
    out += "\ntruncated=";
    out += truncated ? "true" : "false";
    out += '\n';
    for (const TraversalStep& step : steps) {
        out += "depth=";
        out += std::to_string(step.depth);
        out += " node=";
        out += step.node.to_string();
        out += " via=";
        out += step.via.to_string();
        out += " relation=";
        out += to_string(step.relation);
        out += '\n';
    }
    return out;
}

TraversalResult TopologyEngine::traverse(const TraversalRequest& request) const {
    TraversalResult result;
    result.kind = request.kind;
    result.origin = request.origin;

    if (request.origin.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "traversal.origin_missing");
        return result;
    }
    const std::size_t max_depth =
        request.max_depth == 0 ? 1 : (std::min)(request.max_depth, impl_->limits.max_traversal_depth);
    const std::size_t max_visited = request.max_visited == 0
                                        ? 1
                                        : (std::min)(request.max_visited,
                                                     impl_->limits.max_traversal_visited);

    std::shared_lock lock(impl_->state_mutex);
    if (impl_->graph.find_node(request.origin) == nullptr) {
        result.status = make_status(Outcome::NotFound, "traversal.origin_not_found");
        result.status.with("origin", request.origin.to_string());
        return result;
    }

    std::unordered_set<TopologyNodeId> visited;
    visited.insert(request.origin);
    std::deque<std::pair<TopologyNodeId, std::size_t>> queue;
    queue.emplace_back(request.origin, 0);

    while (!queue.empty()) {
        const std::pair<TopologyNodeId, std::size_t> frame = queue.front();
        queue.pop_front();
        const TopologyNodeId current = frame.first;
        const std::size_t depth = frame.second;
        if (depth >= max_depth) {
            continue;
        }
        const auto incident_it = impl_->graph.incident.find(current);
        if (incident_it == impl_->graph.incident.end()) {
            continue;
        }
        std::vector<TopologyEdgeId> edges(incident_it->second.begin(), incident_it->second.end());
        std::sort(edges.begin(), edges.end());
        for (const TopologyEdgeId& edge_id : edges) {
            const TopologyEdge* edge = impl_->graph.find_edge(edge_id);
            if (edge == nullptr) {
                continue;
            }
            if (edge->lifecycle == LifecycleState::Retired ||
                edge->lifecycle == LifecycleState::Superseded) {
                continue;
            }
            if (edge->lifecycle != LifecycleState::Current && !request.include_non_current) {
                continue;
            }
            if (request.relation.has_value() && edge->relation != *request.relation) {
                continue;
            }
            if (request.layer.has_value() && edge->layer != *request.layer) {
                continue;
            }
            if (!relation_in(request.kind, edge->relation)) {
                continue;
            }
            const std::optional<TopologyNodeId> next = next_node(request.kind, *edge, current);
            if (!next.has_value() || *next == current) {
                continue;
            }
            if (!visited.insert(*next).second) {
                continue;
            }
            if (result.steps.size() >= max_visited) {
                result.truncated = true;
                break;
            }
            TraversalStep step;
            step.node = *next;
            step.depth = depth + 1;
            step.via = edge->id;
            step.relation = edge->relation;
            result.steps.push_back(step);
            queue.emplace_back(*next, depth + 1);
        }
        if (result.truncated) {
            break;
        }
    }

    std::sort(result.steps.begin(), result.steps.end(), step_less);
    result.status = make_status(Outcome::Ok, "traversal.completed");
    result.status.with("kind", to_string(request.kind));
    result.status.with("depth_limit", std::to_string(max_depth));
    result.status.with("visited_limit", std::to_string(max_visited));
    if (result.truncated) {
        result.status.with("truncated", "true");
    }
    return result;
}

}  // namespace fabric_topology
