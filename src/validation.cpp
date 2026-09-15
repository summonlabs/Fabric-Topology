// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "validation.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "fabric_topology/types.hpp"

namespace fabric_topology::internal {

namespace {

[[nodiscard]] bool issue_less(const ValidationIssue& a, const ValidationIssue& b) {
    if (a.code != b.code) {
        return a.code < b.code;
    }
    if (a.node != b.node) {
        return a.node < b.node;
    }
    if (a.edge != b.edge) {
        return a.edge < b.edge;
    }
    return a.detail < b.detail;
}

void add_issue(ValidationReport& report, Outcome outcome, const char* code, const TopologyNodeId& node,
               const TopologyEdgeId& edge, std::string detail) {
    ValidationIssue issue;
    issue.outcome = outcome;
    issue.code = code;
    issue.node = node;
    issue.edge = edge;
    issue.detail = std::move(detail);
    report.issues.push_back(std::move(issue));
    report.valid = false;
}

}  // namespace

ValidationReport validate_graph(const GraphState& state, const IEntityDirectory* directory,
                                const SnapshotScope& scope) {
    ValidationReport report;
    report.generation = state.generation;

    const std::vector<TopologyNode> nodes = state.sorted_nodes(scope);
    const std::vector<TopologyEdge> edges = state.sorted_edges(scope);
    report.nodes = nodes.size();
    report.edges = edges.size();

    for (const TopologyEdge& edge : edges) {
        if (edge.lifecycle == LifecycleState::Current) {
            ++report.current_edges;
        } else {
            ++report.non_current_edges;
        }
    }

    for (const TopologyNode& node : nodes) {
        if (node.node_class == NodeClass::Unknown) {
            add_issue(report, Outcome::StructuralInvariantViolation, "node.class_unknown", node.id, {},
                      node.entity_id);
        }
        if (node.entity_id.empty()) {
            add_issue(report, Outcome::StructuralInvariantViolation, "node.entity_missing", node.id, {},
                      node.id.to_string());
        }
        if (node.generation.is_zero()) {
            add_issue(report, Outcome::StructuralInvariantViolation, "node.generation_zero", node.id, {},
                      node.id.to_string());
        }
        if (node.entity_class != node_class_entity_class(node.node_class)) {
            add_issue(report, Outcome::IncompatibleEntityClass, "node.entity_class_pairing", node.id, {},
                      std::string(to_string(node.entity_class)) + " vs " +
                          to_string(node.node_class));
        }
        if (directory != nullptr) {
            const auto record = directory->lookup(node.entity_id);
            if (!record.has_value()) {
                add_issue(report, Outcome::UnknownEntity, "node.entity_unknown", node.id, {},
                          node.entity_id);
            } else {
                if (record->retired) {
                    add_issue(report, Outcome::Retired, "node.entity_retired", node.id, {},
                              node.entity_id);
                }
                if (record->superseded) {
                    add_issue(report, Outcome::Superseded, "node.entity_superseded", node.id, {},
                              node.entity_id);
                }
                if (record->entity_class != node.entity_class) {
                    add_issue(report, Outcome::IncompatibleEntityClass, "node.entity_class_changed",
                              node.id, {}, to_string(record->entity_class));
                }
                if (record->generation != node.entity_generation &&
                    node.lifecycle == LifecycleState::Current) {
                    add_issue(report, Outcome::StaleEntityGeneration, "node.entity_generation_stale",
                              node.id, {},
                              "registry=" + record->generation.to_string() +
                                  " node=" + node.entity_generation.to_string());
                }
            }
        }
    }

    std::unordered_map<TopologyNodeId, const TopologyNode*> node_by_id;
    node_by_id.reserve(nodes.size());
    for (const TopologyNode& node : nodes) {
        node_by_id.emplace(node.id, &node);
    }

    std::unordered_map<std::string, TopologyEdgeId> seen_keys;
    std::unordered_map<std::string, TopologyEdgeId> exclusive_attachments;
    std::unordered_map<std::string, TopologyEdgeId> endpoint_attachment;
    std::unordered_map<TopologyEdgeId, const TopologyEdge*> edge_by_id;
    edge_by_id.reserve(edges.size());
    for (const TopologyEdge& edge : edges) {
        edge_by_id.emplace(edge.id, &edge);
    }

    for (const TopologyEdge& edge : edges) {
        if (edge.relation == RelationClass::Unknown) {
            add_issue(report, Outcome::StructuralInvariantViolation, "edge.relation_unknown", {}, edge.id,
                      edge.id.to_string());
            continue;
        }
        const RelationRules& rules = relation_rules(edge.relation);

        if (edge.from == edge.to && !rules.self_edge_allowed) {
            add_issue(report, Outcome::InvalidEndpoint, "edge.self_link", edge.from, edge.id,
                      to_string(edge.relation));
        }
        if (!relation_layer_permitted(edge.relation, edge.layer)) {
            add_issue(report, Outcome::StructuralInvariantViolation, "edge.layer_policy", {}, edge.id,
                      to_string(edge.layer));
        }

        const std::string key =
            render_edge_key(make_edge_key(edge.relation, edge.from, edge.to, edge.domain));
        const auto key_it = seen_keys.find(key);
        if (key_it != seen_keys.end()) {
            add_issue(report, Outcome::DuplicateEdge, "edge.duplicate_authoritative", {}, edge.id,
                      key_it->second.to_string());
        } else {
            seen_keys.emplace(key, edge.id);
        }

        const auto from_it = node_by_id.find(edge.from);
        const auto to_it = node_by_id.find(edge.to);
        if (from_it == node_by_id.end() || to_it == node_by_id.end()) {
            if (scope.whole_topology) {
                add_issue(report, Outcome::StructuralInvariantViolation, "edge.orphan", {}, edge.id,
                          edge.from.to_string() + "->" + edge.to.to_string());
            }
            continue;
        }
        const TopologyNode& from_node = *from_it->second;
        const TopologyNode& to_node = *to_it->second;

        if (!relation_endpoints_permitted(edge.relation, from_node.node_class, to_node.node_class)) {
            add_issue(report, Outcome::IncompatibleEntityClass, "edge.endpoint_class_pairing", {},
                      edge.id,
                      render_relation(edge.relation, from_node.node_class, to_node.node_class));
        }
        if (edge.domain != from_node.domain || edge.domain != to_node.domain) {
            const bool permitted_cross =
                edge.cross_domain &&
                (edge.domain == from_node.domain || edge.secondary_domain == from_node.domain) &&
                (edge.domain == to_node.domain || edge.secondary_domain == to_node.domain);
            if (!permitted_cross) {
                add_issue(report, Outcome::DomainViolation, "edge.cross_domain_undeclared", {}, edge.id,
                          edge.domain.to_string());
            }
        }
        if (edge.lifecycle == LifecycleState::Current) {
            if (edge.from_entity_generation != from_node.entity_generation ||
                edge.to_entity_generation != to_node.entity_generation) {
                add_issue(report, Outcome::StaleEntityGeneration, "edge.endpoint_generation_stale", {},
                          edge.id, edge.id.to_string());
            }
            if (from_node.lifecycle != LifecycleState::Current ||
                to_node.lifecycle != LifecycleState::Current) {
                add_issue(report, Outcome::StructuralInvariantViolation, "edge.endpoint_not_current", {},
                          edge.id, edge.id.to_string());
            }
        }
        if (edge.supported_by.has_value()) {
            const auto support_it = edge_by_id.find(*edge.supported_by);
            if (support_it == edge_by_id.end()) {
                add_issue(report, Outcome::StructuralInvariantViolation, "edge.support_missing", {},
                          edge.id, edge.supported_by->to_string());
            } else if (edge.layer == TopologyLayer::Logical &&
                       support_it->second->layer != TopologyLayer::Physical) {
                add_issue(report, Outcome::StructuralInvariantViolation, "edge.support_not_physical", {},
                          edge.id, support_it->second->id.to_string());
            }
        }
        if (edge.superseded_by.has_value() && edge.id == *edge.superseded_by) {
            add_issue(report, Outcome::StructuralInvariantViolation, "edge.self_supersede", {}, edge.id,
                      edge.id.to_string());
        }
        if (edge.attachment.has_value() && edge.lifecycle == LifecycleState::Current &&
            edge.relation == RelationClass::AttachedTo) {
            const auto attach_it = exclusive_attachments.find(edge.attachment->value());
            if (attach_it != exclusive_attachments.end()) {
                add_issue(report, Outcome::RelationshipConflict, "attachment.duplicate_exclusive", {},
                          edge.id, edge.attachment->value());
            } else {
                exclusive_attachments.emplace(edge.attachment->value(), edge.id);
            }
            const std::string slot = edge.from.to_string() + "|" + edge.attachment->value();
            const auto slot_it = endpoint_attachment.find(slot);
            if (slot_it != endpoint_attachment.end()) {
                add_issue(report, Outcome::RelationshipConflict, "attachment.multiple_current", {},
                          edge.id, slot);
            } else {
                endpoint_attachment.emplace(slot, edge.id);
            }
        }
    }

    // Acyclicity per relation class, over the whole scope.
    for (std::uint8_t raw = 1; raw < kRelationClassCount; ++raw) {
        const auto relation = static_cast<RelationClass>(raw);
        if (relation_rules(relation).cycle_rule != CycleRule::Acyclic) {
            continue;
        }
        std::unordered_map<TopologyNodeId, std::vector<TopologyNodeId>> adjacency;
        for (const TopologyEdge& edge : edges) {
            if (edge.relation != relation || edge.lifecycle == LifecycleState::Retired ||
                edge.lifecycle == LifecycleState::Superseded) {
                continue;
            }
            adjacency[edge.from].push_back(edge.to);
        }
        for (auto& entry : adjacency) {
            std::sort(entry.second.begin(), entry.second.end());
            entry.second.erase(std::unique(entry.second.begin(), entry.second.end()), entry.second.end());
        }

        enum class Mark : std::uint8_t { White = 0, Grey = 1, Black = 2 };
        std::unordered_map<TopologyNodeId, Mark> marks;
        std::vector<std::pair<TopologyNodeId, std::size_t>> stack;
        for (const auto& entry : adjacency) {
            if (marks[entry.first] != Mark::White) {
                continue;
            }
            stack.emplace_back(entry.first, 0);
            marks[entry.first] = Mark::Grey;
            while (!stack.empty()) {
                auto& frame = stack.back();
                const auto next_it = adjacency.find(frame.first);
                if (next_it == adjacency.end() || frame.second >= next_it->second.size()) {
                    marks[frame.first] = Mark::Black;
                    stack.pop_back();
                    continue;
                }
                const TopologyNodeId next = next_it->second[frame.second];
                ++frame.second;
                const Mark mark = marks[next];
                if (mark == Mark::Grey) {
                    add_issue(report, Outcome::CycleRejected, "graph.cycle", next, {},
                              to_string(relation));
                    continue;
                }
                if (mark == Mark::White) {
                    marks[next] = Mark::Grey;
                    stack.emplace_back(next, 0);
                }
            }
        }
    }

    std::sort(report.issues.begin(), report.issues.end(), issue_less);
    report.valid = report.issues.empty();
    return report;
}

}  // namespace fabric_topology::internal
