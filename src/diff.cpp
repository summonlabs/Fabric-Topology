// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "engine_impl.hpp"
#include "fabric_topology/digest.hpp"
#include "fabric_topology/topology.hpp"

namespace fabric_topology {

namespace {

[[nodiscard]] bool entry_less(const DiffEntry& a, const DiffEntry& b) {
    const auto left = static_cast<std::uint8_t>(a.kind);
    const auto right = static_cast<std::uint8_t>(b.kind);
    if (left != right) {
        return left < right;
    }
    return a.key < b.key;
}

}  // namespace

const char* to_string(DiffKind value) noexcept {
    switch (value) {
        case DiffKind::NodeAdded: return "NODE_ADDED";
        case DiffKind::NodeRemoved: return "NODE_REMOVED";
        case DiffKind::NodeGenerationChanged: return "NODE_GENERATION_CHANGED";
        case DiffKind::EdgeAdded: return "EDGE_ADDED";
        case DiffKind::EdgeRemoved: return "EDGE_REMOVED";
        case DiffKind::EdgeSuperseded: return "EDGE_SUPERSEDED";
        case DiffKind::AttachmentMoved: return "ATTACHMENT_MOVED";
        case DiffKind::RelationshipChanged: return "RELATIONSHIP_CHANGED";
        case DiffKind::ScopeChanged: return "SCOPE_CHANGED";
        case DiffKind::CurrentnessChanged: return "CURRENTNESS_CHANGED";
    }
    return "NODE_ADDED";
}

std::size_t TopologyDiff::count(DiffKind kind) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        entries.begin(), entries.end(), [kind](const DiffEntry& entry) { return entry.kind == kind; }));
}

std::string TopologyDiff::render() const {
    std::string out;
    out.reserve(128 + entries.size() * 96);
    out += "from_generation=";
    out += from_generation.to_string();
    out += "\nto_generation=";
    out += to_generation.to_string();
    out += "\nentries=";
    out += std::to_string(entries.size());
    out += '\n';
    for (const DiffEntry& entry : entries) {
        out += to_string(entry.kind);
        out += ' ';
        out += entry.key;
        if (!entry.before.empty()) {
            out += " before=";
            out += entry.before;
        }
        if (!entry.after.empty()) {
            out += " after=";
            out += entry.after;
        }
        out += '\n';
    }
    return out;
}

TopologyDiff TopologyEngine::diff(const TopologySnapshot& before, const TopologySnapshot& after) const {
    TopologyDiff result;
    result.from_generation = before.topology_generation;
    result.to_generation = after.topology_generation;

    std::map<std::string, const TopologyNode*> before_nodes;
    for (const TopologyNode& node : before.nodes) {
        before_nodes.emplace(node.id.value(), &node);
    }
    std::map<std::string, const TopologyNode*> after_nodes;
    for (const TopologyNode& node : after.nodes) {
        after_nodes.emplace(node.id.value(), &node);
    }

    for (const auto& entry : before_nodes) {
        const auto it = after_nodes.find(entry.first);
        if (it == after_nodes.end()) {
            DiffEntry item;
            item.kind = DiffKind::NodeRemoved;
            item.key = entry.first;
            item.node = entry.second->id;
            item.before = render_node(*entry.second);
            result.entries.push_back(std::move(item));
            continue;
        }
        const TopologyNode& lhs = *entry.second;
        const TopologyNode& rhs = *it->second;
        if (lhs.entity_generation != rhs.entity_generation) {
            DiffEntry item;
            item.kind = DiffKind::NodeGenerationChanged;
            item.key = entry.first;
            item.node = lhs.id;
            item.before = lhs.entity_generation.to_string();
            item.after = rhs.entity_generation.to_string();
            result.entries.push_back(std::move(item));
        }
        if (lhs.domain != rhs.domain) {
            DiffEntry item;
            item.kind = DiffKind::ScopeChanged;
            item.key = entry.first;
            item.node = lhs.id;
            item.before = lhs.domain.to_string();
            item.after = rhs.domain.to_string();
            result.entries.push_back(std::move(item));
        }
        if (lhs.lifecycle != rhs.lifecycle) {
            DiffEntry item;
            item.kind = DiffKind::CurrentnessChanged;
            item.key = entry.first;
            item.node = lhs.id;
            item.before = to_string(lhs.lifecycle);
            item.after = to_string(rhs.lifecycle);
            result.entries.push_back(std::move(item));
        }
    }
    for (const auto& entry : after_nodes) {
        if (before_nodes.find(entry.first) != before_nodes.end()) {
            continue;
        }
        DiffEntry item;
        item.kind = DiffKind::NodeAdded;
        item.key = entry.first;
        item.node = entry.second->id;
        item.after = render_node(*entry.second);
        result.entries.push_back(std::move(item));
    }

    std::map<std::string, const TopologyEdge*> before_edges;
    for (const TopologyEdge& edge : before.edges) {
        before_edges.emplace(edge.id.value(), &edge);
    }
    std::map<std::string, const TopologyEdge*> after_edges;
    for (const TopologyEdge& edge : after.edges) {
        after_edges.emplace(edge.id.value(), &edge);
    }

    for (const auto& entry : before_edges) {
        const auto it = after_edges.find(entry.first);
        if (it == after_edges.end()) {
            DiffEntry item;
            item.kind = DiffKind::EdgeRemoved;
            item.key = entry.first;
            item.edge = entry.second->id;
            item.before = render_edge(*entry.second);
            result.entries.push_back(std::move(item));
            continue;
        }
        const TopologyEdge& lhs = *entry.second;
        const TopologyEdge& rhs = *it->second;
        const bool superseded = lhs.lifecycle != rhs.lifecycle &&
                                rhs.lifecycle == LifecycleState::Superseded;
        if (superseded) {
            DiffEntry item;
            item.kind = DiffKind::EdgeSuperseded;
            item.key = entry.first;
            item.edge = lhs.id;
            item.before = to_string(lhs.lifecycle);
            item.after = to_string(rhs.lifecycle);
            result.entries.push_back(std::move(item));
        } else if (lhs.from != rhs.from || lhs.to != rhs.to) {
            DiffEntry item;
            item.kind = DiffKind::AttachmentMoved;
            item.key = entry.first;
            item.edge = lhs.id;
            item.before = lhs.to.to_string();
            item.after = rhs.to.to_string();
            result.entries.push_back(std::move(item));
        }
        if (lhs.domain != rhs.domain) {
            DiffEntry item;
            item.kind = DiffKind::ScopeChanged;
            item.key = entry.first;
            item.edge = lhs.id;
            item.before = lhs.domain.to_string();
            item.after = rhs.domain.to_string();
            result.entries.push_back(std::move(item));
        }
        if (!superseded && lhs.lifecycle != rhs.lifecycle) {
            DiffEntry item;
            item.kind = DiffKind::CurrentnessChanged;
            item.key = entry.first;
            item.edge = lhs.id;
            item.before = to_string(lhs.lifecycle);
            item.after = to_string(rhs.lifecycle);
            result.entries.push_back(std::move(item));
        }
        const bool content_changed =
            lhs.generation != rhs.generation || lhs.evidence_generation != rhs.evidence_generation ||
            lhs.metadata.items() != rhs.metadata.items() || lhs.supported_by != rhs.supported_by ||
            lhs.attachment != rhs.attachment;
        const bool lifecycle_only =
            lhs.lifecycle != rhs.lifecycle && lhs.generation == rhs.generation &&
            lhs.evidence_generation == rhs.evidence_generation &&
            lhs.metadata.items() == rhs.metadata.items() && lhs.supported_by == rhs.supported_by &&
            lhs.attachment == rhs.attachment;
        if (content_changed && !lifecycle_only && !superseded) {
            DiffEntry item;
            item.kind = DiffKind::RelationshipChanged;
            item.key = entry.first;
            item.edge = lhs.id;
            item.before = lhs.generation.to_string();
            item.after = rhs.generation.to_string();
            result.entries.push_back(std::move(item));
        }
    }
    for (const auto& entry : after_edges) {
        if (before_edges.find(entry.first) != before_edges.end()) {
            continue;
        }
        DiffEntry item;
        item.kind = DiffKind::EdgeAdded;
        item.key = entry.first;
        item.edge = entry.second->id;
        item.after = render_edge(*entry.second);
        result.entries.push_back(std::move(item));
    }

    std::sort(result.entries.begin(), result.entries.end(), entry_less);
    result.digest = sha256_hex(result.render());
    return result;
}

TopologyDiff TopologyEngine::diff_against_current(const TopologySnapshot& before) const {
    return diff(before, snapshot(before.scope));
}

}  // namespace fabric_topology
