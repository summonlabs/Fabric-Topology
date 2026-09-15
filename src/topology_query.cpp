// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Queries, structural validation, deterministic explanations and introspection.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine_impl.hpp"
#include "fabric_topology/topology.hpp"
#include "graph_state.hpp"
#include "validation.hpp"

namespace fabric_topology {

namespace {

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] bool matches_filter(const TopologyEdge& edge, const EdgeFilter& filter) {
    if (filter.relation.has_value() && edge.relation != *filter.relation) {
        return false;
    }
    if (filter.layer.has_value() && edge.layer != *filter.layer) {
        return false;
    }
    if (filter.domain.has_value() && edge.domain != *filter.domain &&
        !(edge.cross_domain && edge.secondary_domain == *filter.domain)) {
        return false;
    }
    if (filter.lifecycle.has_value() && edge.lifecycle != *filter.lifecycle) {
        return false;
    }
    if (filter.publisher.has_value() && edge.provenance.publisher != *filter.publisher) {
        return false;
    }
    if (!filter.include_non_current && edge.lifecycle != LifecycleState::Current) {
        return false;
    }
    return true;
}

}  // namespace

EdgeFilter EdgeFilter::current_only() noexcept {
    EdgeFilter filter;
    filter.include_non_current = false;
    filter.lifecycle = LifecycleState::Current;
    return filter;
}

// ---------------------------------------------------------------------------
// Basic counters
// ---------------------------------------------------------------------------

TopologyGeneration TopologyEngine::generation() const noexcept {
    std::shared_lock lock(impl_->state_mutex);
    return impl_->graph.generation;
}

std::size_t TopologyEngine::node_count() const noexcept {
    std::shared_lock lock(impl_->state_mutex);
    return impl_->graph.nodes.size();
}

std::size_t TopologyEngine::edge_count() const noexcept {
    std::shared_lock lock(impl_->state_mutex);
    return impl_->graph.edges.size();
}

const Limits& TopologyEngine::limits() const noexcept { return impl_->limits; }

const std::string& TopologyEngine::persistence_path() const noexcept {
    return impl_->options.persistence_path;
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

std::optional<TopologyNode> TopologyEngine::node(const TopologyNodeId& id) const {
    std::shared_lock lock(impl_->state_mutex);
    const TopologyNode* found = impl_->graph.find_node(id);
    if (found == nullptr) {
        return std::nullopt;
    }
    return *found;
}

std::optional<TopologyEdge> TopologyEngine::edge(const TopologyEdgeId& id) const {
    std::shared_lock lock(impl_->state_mutex);
    const TopologyEdge* found = impl_->graph.find_edge(id);
    if (found == nullptr) {
        return std::nullopt;
    }
    return *found;
}

std::optional<TopologyNode> TopologyEngine::node_for_entity(std::string_view entity_id) const {
    std::shared_lock lock(impl_->state_mutex);
    const TopologyNodeId* id = impl_->graph.find_node_by_entity(entity_id);
    if (id == nullptr) {
        return std::nullopt;
    }
    const TopologyNode* found = impl_->graph.find_node(*id);
    if (found == nullptr) {
        return std::nullopt;
    }
    return *found;
}

std::optional<TopologyEdge> TopologyEngine::relationship(const RelationshipId& id) const {
    std::shared_lock lock(impl_->state_mutex);
    const TopologyEdgeId* edge_id = impl_->graph.find_edge_by_relationship(id);
    if (edge_id == nullptr) {
        return std::nullopt;
    }
    const TopologyEdge* found = impl_->graph.find_edge(*edge_id);
    if (found == nullptr) {
        return std::nullopt;
    }
    return *found;
}

// ---------------------------------------------------------------------------
// Scoped queries
// ---------------------------------------------------------------------------

std::vector<TopologyEdge> TopologyEngine::edges_for_node(const TopologyNodeId& id,
                                                         const EdgeFilter& filter) const {
    std::shared_lock lock(impl_->state_mutex);
    std::vector<TopologyEdge> result;
    const auto it = impl_->graph.incident.find(id);
    if (it == impl_->graph.incident.end()) {
        return result;
    }
    for (const TopologyEdgeId& edge_id : it->second) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found != nullptr && matches_filter(*found, filter)) {
            result.push_back(*found);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TopologyEdge& a, const TopologyEdge& b) { return a.id < b.id; });
    return result;
}

std::vector<TopologyNodeId> TopologyEngine::neighbors(const TopologyNodeId& id,
                                                      const EdgeFilter& filter) const {
    std::shared_lock lock(impl_->state_mutex);
    std::vector<TopologyNodeId> result;
    const auto it = impl_->graph.incident.find(id);
    if (it == impl_->graph.incident.end()) {
        return result;
    }
    for (const TopologyEdgeId& edge_id : it->second) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found == nullptr || !matches_filter(*found, filter)) {
            continue;
        }
        if (found->from != id) {
            result.push_back(found->from);
        }
        if (found->to != id) {
            result.push_back(found->to);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<TopologyEdge> TopologyEngine::edges_by_relation(RelationClass relation,
                                                            const EdgeFilter& filter) const {
    std::shared_lock lock(impl_->state_mutex);
    std::vector<TopologyEdge> result;
    const auto it = impl_->graph.edge_by_relation.find(static_cast<std::uint8_t>(relation));
    if (it == impl_->graph.edge_by_relation.end()) {
        return result;
    }
    for (const TopologyEdgeId& edge_id : it->second) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found != nullptr && matches_filter(*found, filter)) {
            result.push_back(*found);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TopologyEdge& a, const TopologyEdge& b) { return a.id < b.id; });
    return result;
}

std::vector<TopologyEdge> TopologyEngine::edges_in_domain(const TopologyDomainId& domain,
                                                          const EdgeFilter& filter) const {
    EdgeFilter scoped = filter;
    if (!scoped.domain.has_value()) {
        scoped.domain = domain;
    }
    std::shared_lock lock(impl_->state_mutex);
    std::vector<TopologyEdge> result;
    const auto it = impl_->graph.domains.find(domain);
    if (it == impl_->graph.domains.end()) {
        return result;
    }
    for (const TopologyEdgeId& edge_id : it->second.edges) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found != nullptr && matches_filter(*found, scoped)) {
            result.push_back(*found);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TopologyEdge& a, const TopologyEdge& b) { return a.id < b.id; });
    return result;
}

std::vector<TopologyNodeId> TopologyEngine::domain_members(const TopologyDomainId& domain) const {
    std::shared_lock lock(impl_->state_mutex);
    std::vector<TopologyNodeId> result;
    const auto it = impl_->graph.domains.find(domain);
    if (it == impl_->graph.domains.end()) {
        return result;
    }
    result.assign(it->second.nodes.begin(), it->second.nodes.end());
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<TopologyEdge> TopologyEngine::edges_from_publisher(const PublisherId& publisher,
                                                               const EdgeFilter& filter) const {
    std::shared_lock lock(impl_->state_mutex);
    std::vector<TopologyEdge> result;
    for (const auto& entry : impl_->graph.edges) {
        if (entry.second.provenance.publisher != publisher) {
            continue;
        }
        if (matches_filter(entry.second, filter)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TopologyEdge& a, const TopologyEdge& b) { return a.id < b.id; });
    return result;
}

std::vector<TopologyEdge> TopologyEngine::physical_relationships(const EdgeFilter& filter) const {
    EdgeFilter scoped = filter;
    scoped.layer = TopologyLayer::Physical;
    std::shared_lock lock(impl_->state_mutex);
    std::vector<TopologyEdge> result;
    const auto it = impl_->graph.edge_by_layer.find(static_cast<std::uint8_t>(TopologyLayer::Physical));
    if (it == impl_->graph.edge_by_layer.end()) {
        return result;
    }
    for (const TopologyEdgeId& edge_id : it->second) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found != nullptr && matches_filter(*found, scoped)) {
            result.push_back(*found);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TopologyEdge& a, const TopologyEdge& b) { return a.id < b.id; });
    return result;
}

std::vector<TopologyEdge> TopologyEngine::logical_relationships(const EdgeFilter& filter) const {
    EdgeFilter scoped = filter;
    scoped.layer = TopologyLayer::Logical;
    std::shared_lock lock(impl_->state_mutex);
    std::vector<TopologyEdge> result;
    const auto it = impl_->graph.edge_by_layer.find(static_cast<std::uint8_t>(TopologyLayer::Logical));
    if (it == impl_->graph.edge_by_layer.end()) {
        return result;
    }
    for (const TopologyEdgeId& edge_id : it->second) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found != nullptr && matches_filter(*found, scoped)) {
            result.push_back(*found);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TopologyEdge& a, const TopologyEdge& b) { return a.id < b.id; });
    return result;
}

std::vector<TopologyNode> TopologyEngine::all_nodes() const {
    std::shared_lock lock(impl_->state_mutex);
    return impl_->graph.sorted_nodes(SnapshotScope::all());
}

std::vector<TopologyEdge> TopologyEngine::all_edges() const {
    std::shared_lock lock(impl_->state_mutex);
    return impl_->graph.sorted_edges(SnapshotScope::all());
}

std::vector<TopologyNodeId> TopologyEngine::entity_members(const std::string& anchor_entity_id,
                                                           RelationClass relation) const {
    std::shared_lock lock(impl_->state_mutex);
    std::vector<TopologyNodeId> result;
    const TopologyNodeId* anchor = impl_->graph.find_node_by_entity(anchor_entity_id);
    if (anchor == nullptr) {
        return result;
    }
    const auto it = impl_->graph.incident.find(*anchor);
    if (it == impl_->graph.incident.end()) {
        return result;
    }
    for (const TopologyEdgeId& edge_id : it->second) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found == nullptr || found->relation != relation) {
            continue;
        }
        if (found->from == *anchor) {
            result.push_back(found->to);
        } else if (found->to == *anchor) {
            result.push_back(found->from);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

ValidationReport TopologyEngine::validate(const SnapshotScope& scope) const {
    std::shared_lock lock(impl_->state_mutex);
    return internal::validate_graph(impl_->graph, impl_->directory.get(), scope);
}

std::string ValidationReport::render() const {
    std::string out;
    out.reserve(256);
    out += "valid=";
    out += valid ? "true" : "false";
    out += " generation=";
    out += generation.to_string();
    out += " nodes=";
    out += std::to_string(nodes);
    out += " edges=";
    out += std::to_string(edges);
    out += " current_edges=";
    out += std::to_string(current_edges);
    out += " non_current_edges=";
    out += std::to_string(non_current_edges);
    out += " issues=";
    out += std::to_string(issues.size());
    for (const ValidationIssue& issue : issues) {
        out += "\n  ";
        out += issue.code;
        out += " outcome=";
        out += to_string(issue.outcome);
        if (!issue.node.empty()) {
            out += " node=";
            out += issue.node.to_string();
        }
        if (!issue.edge.empty()) {
            out += " edge=";
            out += issue.edge.to_string();
        }
        if (!issue.detail.empty()) {
            out += " detail=";
            out += issue.detail;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Explanations
// ---------------------------------------------------------------------------

Status TopologyEngine::explain_relationship(const TopologyEdgeId& id, Explanation& out) const {
    std::shared_lock lock(impl_->state_mutex);
    const TopologyEdge* found = impl_->graph.find_edge(id);
    if (found == nullptr) {
        out = make_status(Outcome::NotFound, "explain.relationship_not_found");
        out.with("edge_id", id.to_string());
        return out;
    }
    const TopologyEdge& edge = *found;
    const TopologyNode* from_node = impl_->graph.find_node(edge.from);
    const TopologyNode* to_node = impl_->graph.find_node(edge.to);

    out = make_status(Outcome::Ok, "explain.relationship");
    out.with("edge_id", edge.id.to_string());
    out.with("relationship_id", edge.relationship_id.to_string());
    out.with("relation", to_string(edge.relation));
    out.with("direction", to_string(relation_rules(edge.relation).direction));
    out.with("layer", to_string(edge.layer));
    out.with("domain", edge.domain.to_string());
    out.with("cross_domain", edge.cross_domain ? "true" : "false");
    if (edge.cross_domain) {
        out.with("secondary_domain", edge.secondary_domain.to_string());
    }
    out.with("edge_generation", edge.generation.to_string());
    out.with("created_generation", edge.created_generation.to_string());
    out.with("last_validated_generation", edge.last_validated_generation.to_string());
    out.with("lifecycle", to_string(edge.lifecycle));
    out.with("evidence_generation", edge.evidence_generation.to_string());
    out.with("from_node", edge.from.to_string());
    out.with("to_node", edge.to.to_string());
    if (from_node != nullptr) {
        out.with("from_entity", from_node->entity_id);
        out.with("from_class", to_string(from_node->node_class));
        out.with("from_entity_generation_bound", edge.from_entity_generation.to_string());
        out.with("from_entity_generation_current", from_node->entity_generation.to_string());
    }
    if (to_node != nullptr) {
        out.with("to_entity", to_node->entity_id);
        out.with("to_class", to_string(to_node->node_class));
        out.with("to_entity_generation_bound", edge.to_entity_generation.to_string());
        out.with("to_entity_generation_current", to_node->entity_generation.to_string());
    }
    if (edge.attachment.has_value()) {
        out.with("attachment", edge.attachment->value());
    }
    if (edge.superseded_by.has_value()) {
        out.with("superseded_by", edge.superseded_by->to_string());
    }
    if (edge.supported_by.has_value()) {
        out.with("supported_by", edge.supported_by->to_string());
    }
    out.with("publisher", edge.provenance.publisher.to_string());
    out.with("worker_boot", edge.provenance.worker_boot.to_string());
    out.with("coordinator_epoch", edge.provenance.coordinator_epoch.to_string());
    out.with("discovery_source", to_string(edge.provenance.source));
    out.with("evidence_type", to_string(edge.provenance.evidence_type));
    out.with("cycle_rule", relation_rules(edge.relation).cycle_rule == CycleRule::Acyclic ? "ACYCLIC"
                                                                                          : "CYCLES_ALLOWED");
    out.with("currentness_reason", [&edge, from_node, to_node]() -> std::string {
        if (edge.lifecycle == LifecycleState::Current) {
            if (from_node == nullptr || to_node == nullptr) {
                return "endpoint_missing";
            }
            if (edge.from_entity_generation != from_node->entity_generation ||
                edge.to_entity_generation != to_node->entity_generation) {
                return "endpoint_entity_generation_advanced";
            }
            return "validated_under_current_generation";
        }
        if (edge.lifecycle == LifecycleState::RevalidationRequired) {
            return "evidence_not_revalidated_under_current_authority";
        }
        if (edge.lifecycle == LifecycleState::Superseded) {
            return "replaced_by_newer_relationship";
        }
        if (edge.lifecycle == LifecycleState::Retired) {
            return "explicitly_retired";
        }
        if (edge.lifecycle == LifecycleState::Conflicted) {
            return "conflicting_authoritative_observations";
        }
        return "no_authoritative_evidence";
    }());
    return out;
}

Status TopologyEngine::explain_node(const TopologyNodeId& id, Explanation& out) const {
    std::shared_lock lock(impl_->state_mutex);
    const TopologyNode* found = impl_->graph.find_node(id);
    if (found == nullptr) {
        out = make_status(Outcome::NotFound, "explain.node_not_found");
        out.with("node_id", id.to_string());
        return out;
    }
    const TopologyNode& node = *found;
    out = make_status(Outcome::Ok, "explain.node");
    out.with("node_id", node.id.to_string());
    out.with("entity_id", node.entity_id);
    out.with("entity_class", to_string(node.entity_class));
    out.with("node_class", to_string(node.node_class));
    out.with("tier", to_string(node.tier));
    out.with("domain", node.domain.to_string());
    out.with("node_generation", node.generation.to_string());
    out.with("entity_generation", node.entity_generation.to_string());
    out.with("created_generation", node.created_generation.to_string());
    out.with("last_validated_generation", node.last_validated_generation.to_string());
    out.with("lifecycle", to_string(node.lifecycle));
    out.with("publisher", node.provenance.publisher.to_string());
    out.with("worker_boot", node.provenance.worker_boot.to_string());
    out.with("participates_in_current_topology", node.lifecycle == LifecycleState::Current ? "true"
                                                                                          : "false");
    if (impl_->directory != nullptr) {
        const auto record = impl_->directory->lookup(node.entity_id);
        out.with("entity_exists", record.has_value() ? "true" : "false");
        if (record.has_value()) {
            out.with("registry_generation", record->generation.to_string());
            out.with("registry_retired", record->retired ? "true" : "false");
            out.with("registry_superseded", record->superseded ? "true" : "false");
        }
    } else {
        out.with("entity_exists", "unknown");
    }
    const auto incident_it = impl_->graph.incident.find(id);
    out.with("incident_relationships",
             std::to_string(incident_it == impl_->graph.incident.end() ? 0 : incident_it->second.size()));
    return out;
}

Status TopologyEngine::explain_generation(TopologyGeneration gen, Explanation& out) const {
    std::shared_lock lock(impl_->state_mutex);
    out = make_status(Outcome::Ok, "explain.generation");
    out.with("requested_generation", gen.to_string());
    out.with("current_generation", impl_->graph.generation.to_string());
    out.with("coordinator_epoch", impl_->coordinator_epoch.to_string());
    out.with("matches_current", gen == impl_->graph.generation ? "true" : "false");
    out.with("generation_advances", std::to_string(impl_->generation_advances));
    out.with("committed_mutations", std::to_string(impl_->committed_mutations));
    out.with("idempotent_mutations", std::to_string(impl_->idempotent_mutations));
    out.with("rejected_mutations", std::to_string(impl_->rejected_mutations));
    out.with("publications_committed", std::to_string(impl_->publications_committed));
    out.with("publications_rejected", std::to_string(impl_->publications_rejected));
    return out;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

std::string TopologyEngine::render_canonical(const SnapshotScope& scope) const {
    std::shared_lock lock(impl_->state_mutex);
    return impl_->graph.render_canonical(scope);
}

std::string TopologyEngine::statistics() const {
    std::shared_lock lock(impl_->state_mutex);
    std::string out;
    out.reserve(512);
    out += "topology_generation=";
    out += impl_->graph.generation.to_string();
    out += "\ncoordinator_epoch=";
    out += impl_->coordinator_epoch.to_string();
    out += "\nnodes=";
    out += std::to_string(impl_->graph.nodes.size());
    out += "\nedges=";
    out += std::to_string(impl_->graph.edges.size());
    out += "\ndomains=";
    out += std::to_string(impl_->graph.domain_definitions.size());
    out += "\npublishers=";
    out += std::to_string(impl_->publishers.size());
    out += "\nfenced_worker_boots=";
    out += std::to_string(impl_->fenced_boots.size());
    out += "\nsnapshots=";
    out += std::to_string(impl_->snapshots.size());
    out += "\ncommitted_mutations=";
    out += std::to_string(impl_->committed_mutations);
    out += "\nidempotent_mutations=";
    out += std::to_string(impl_->idempotent_mutations);
    out += "\nrejected_mutations=";
    out += std::to_string(impl_->rejected_mutations);
    out += "\ngeneration_advances=";
    out += std::to_string(impl_->generation_advances);
    out += "\npublications_committed=";
    out += std::to_string(impl_->publications_committed);
    out += "\npublications_rejected=";
    out += std::to_string(impl_->publications_rejected);
    out += "\ndigest=";
    out += impl_->graph.topology_digest(SnapshotScope::all());
    for (std::uint8_t raw = 1; raw < kRelationClassCount; ++raw) {
        const auto it = impl_->graph.edge_by_relation.find(raw);
        const std::size_t count = it == impl_->graph.edge_by_relation.end() ? 0 : it->second.size();
        out += "\nrelation.";
        out += to_string(static_cast<RelationClass>(raw));
        out += "=";
        out += std::to_string(count);
    }
    for (std::uint8_t raw = 0; raw < kLifecycleStateCount; ++raw) {
        const auto it = impl_->graph.edge_by_lifecycle.find(raw);
        const std::size_t count = it == impl_->graph.edge_by_lifecycle.end() ? 0 : it->second.size();
        out += "\nlifecycle.";
        out += to_string(static_cast<LifecycleState>(raw));
        if (count != 0 || raw <= static_cast<std::uint8_t>(LifecycleState::Conflicted)) {
            out += "=";
            out += std::to_string(count);
        }
    }
    return out;
}

Status TopologyEngine::verify_integrity() const {
    std::shared_lock lock(impl_->state_mutex);
    const Status indexes = impl_->graph.verify_indexes();
    if (indexes.outcome() != Outcome::Ok) {
        return indexes;
    }
    return make_status(Outcome::Ok, "integrity.ok");
}

}  // namespace fabric_topology
