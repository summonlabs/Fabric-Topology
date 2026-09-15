// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "graph_state.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "fabric_topology/digest.hpp"
#include "fabric_topology/types.hpp"

namespace fabric_topology {

EdgeKey make_edge_key(RelationClass relation, const TopologyNodeId& from, const TopologyNodeId& to,
                      const TopologyDomainId& domain) {
    EdgeKey key;
    key.relation = relation;
    key.domain = domain;
    if (relation_rules(relation).direction == Directionality::Undirected && to < from) {
        key.first = to;
        key.second = from;
    } else {
        key.first = from;
        key.second = to;
    }
    return key;
}

std::string render_edge_key(const EdgeKey& key) {
    std::string out;
    out.reserve(96);
    out += to_string(key.relation);
    out += '|';
    out += key.first.to_string();
    out += '|';
    out += key.second.to_string();
    out += '|';
    out += key.domain.to_string();
    return out;
}

std::string render_edge(const TopologyEdge& edge) {
    std::string out;
    out.reserve(192);
    out += "edge ";
    out += edge.id.to_string();
    out += ' ';
    out += to_string(edge.relation);
    out += ' ';
    out += edge.from.to_string();
    out += " -> ";
    out += edge.to.to_string();
    out += " layer=";
    out += to_string(edge.layer);
    out += " domain=";
    out += edge.domain.to_string();
    out += " gen=";
    out += edge.generation.to_string();
    out += " lifecycle=";
    out += to_string(edge.lifecycle);
    return out;
}

std::string render_node(const TopologyNode& node) {
    std::string out;
    out.reserve(192);
    out += "node ";
    out += node.id.to_string();
    out += " entity=";
    out += node.entity_id;
    out += " class=";
    out += to_string(node.node_class);
    out += " tier=";
    out += to_string(node.tier);
    out += " domain=";
    out += node.domain.to_string();
    out += " gen=";
    out += node.generation.to_string();
    out += " lifecycle=";
    out += to_string(node.lifecycle);
    return out;
}

std::string render_provenance(const Provenance& provenance) {
    std::string out;
    out.reserve(224);
    out += "publisher=";
    out += provenance.publisher.to_string();
    out += " boot=";
    out += provenance.worker_boot.to_string();
    out += " epoch=";
    out += provenance.coordinator_epoch.to_string();
    out += " source=";
    out += to_string(provenance.source);
    out += " evidence=";
    out += to_string(provenance.evidence_type);
    out += " evidence_gen=";
    out += provenance.evidence_generation.to_string();
    out += " created_gen=";
    out += provenance.created_generation.to_string();
    out += " validated_gen=";
    out += provenance.last_validated_generation.to_string();
    if (!provenance.source_relationship_id.empty()) {
        out += " source_relationship_id=";
        out += provenance.source_relationship_id;
    }
    if (!provenance.publication.empty()) {
        out += " publication=";
        out += provenance.publication.to_string();
    }
    return out;
}

}  // namespace fabric_topology

namespace fabric_topology::internal {

namespace {

[[nodiscard]] std::vector<TopologyEdgeId> sorted_ids(const std::unordered_set<TopologyEdgeId>& bucket) {
    std::vector<TopologyEdgeId> result(bucket.begin(), bucket.end());
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] std::vector<TopologyEdgeId> sorted_edges_of(
    const std::unordered_map<TopologyNodeId, std::unordered_set<TopologyEdgeId>>& index,
    const TopologyNodeId& id) {
    const auto it = index.find(id);
    return it == index.end() ? std::vector<TopologyEdgeId>{} : sorted_ids(it->second);
}

template <class Map>
[[nodiscard]] bool maps_equal(const Map& a, const Map& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (const auto& entry : a) {
        const auto it = b.find(entry.first);
        if (it == b.end() || !(it->second == entry.second)) {
            return false;
        }
    }
    return true;
}

template <class Key>
[[nodiscard]] bool bucket_maps_equal(
    const std::unordered_map<Key, std::unordered_set<TopologyEdgeId>>& a,
    const std::unordered_map<Key, std::unordered_set<TopologyEdgeId>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (const auto& entry : a) {
        const auto it = b.find(entry.first);
        if (it == b.end() || it->second != entry.second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string hex_prefix_of_digest(std::string_view seed) {
    return sha256_hex(seed).substr(0, 32);
}

void encode_metadata_canonical(const Metadata& metadata, RecordWriter& writer) {
    writer.u32(static_cast<std::uint32_t>(metadata.size()));
    for (const Metadata::Item& item : metadata.items()) {
        writer.text(item.first);
        writer.text(item.second);
    }
}

[[nodiscard]] Status index_failure(const char* code) {
    Status status;
    status.set_outcome(Outcome::StructuralInvariantViolation).set_code(code);
    return status;
}

}  // namespace

void canonical_encode_node(const TopologyNode& node, RecordWriter& writer) {
    writer.u8(1);  // record tag: node
    writer.text(node.id.value());
    writer.text(node.entity_id);
    writer.u8(static_cast<std::uint8_t>(node.entity_class));
    writer.u64(node.entity_generation.value());
    writer.u8(static_cast<std::uint8_t>(node.node_class));
    writer.u8(static_cast<std::uint8_t>(node.tier));
    writer.text(node.domain.value());
    writer.u64(node.generation.value());
    writer.u8(static_cast<std::uint8_t>(node.lifecycle));
    // Evidence classification is semantic; the publisher incarnation that carried it is not.
    writer.u8(static_cast<std::uint8_t>(node.provenance.source));
    writer.u8(static_cast<std::uint8_t>(node.provenance.evidence_type));
    encode_metadata_canonical(node.metadata, writer);
}

void canonical_encode_edge(const TopologyEdge& edge, RecordWriter& writer) {
    writer.u8(2);  // record tag: edge
    writer.text(edge.id.value());
    writer.text(edge.relationship_id.value());
    writer.u8(static_cast<std::uint8_t>(edge.relation));
    writer.u8(static_cast<std::uint8_t>(relation_rules(edge.relation).direction));
    writer.text(edge.from.value());
    writer.text(edge.to.value());
    writer.u8(static_cast<std::uint8_t>(edge.layer));
    writer.text(edge.domain.value());
    writer.boolean(edge.cross_domain);
    writer.text(edge.secondary_domain.value());
    writer.u64(edge.generation.value());
    writer.u64(edge.from_entity_generation.value());
    writer.u64(edge.to_entity_generation.value());
    writer.u8(static_cast<std::uint8_t>(edge.lifecycle));
    writer.boolean(edge.supported_by.has_value());
    if (edge.supported_by.has_value()) {
        writer.text(edge.supported_by->value());
    }
    writer.boolean(edge.superseded_by.has_value());
    if (edge.superseded_by.has_value()) {
        writer.text(edge.superseded_by->value());
    }
    writer.boolean(edge.attachment.has_value());
    if (edge.attachment.has_value()) {
        writer.text(edge.attachment->value());
    }
    writer.u8(static_cast<std::uint8_t>(edge.provenance.source));
    writer.u8(static_cast<std::uint8_t>(edge.provenance.evidence_type));
    encode_metadata_canonical(edge.metadata, writer);
}

// ---------------------------------------------------------------------------
// GraphState
// ---------------------------------------------------------------------------

GraphState::GraphState(const Limits& limits_in) : limits(limits_in) {}

void GraphState::index_node(const TopologyNode& node) {
    node_by_entity[node.entity_id] = node.id;
    domains[node.domain].nodes.insert(node.id);
}

void GraphState::unindex_node(const TopologyNode& node) {
    const auto entity_it = node_by_entity.find(node.entity_id);
    if (entity_it != node_by_entity.end() && entity_it->second == node.id) {
        node_by_entity.erase(entity_it);
    }
    const auto domain_it = domains.find(node.domain);
    if (domain_it != domains.end()) {
        domain_it->second.nodes.erase(node.id);
        if (domain_it->second.nodes.empty() && domain_it->second.edges.empty() &&
            domain_definitions.find(domain_it->first) == domain_definitions.end()) {
            domains.erase(domain_it);
        }
    }
}

void GraphState::index_edge(const TopologyEdge& edge) {
    edge_by_key[make_edge_key(edge.relation, edge.from, edge.to, edge.domain)] = edge.id;
    edge_by_relationship[edge.relationship_id] = edge.id;

    add_to(incident[edge.from], edge.id);
    add_to(incident[edge.to], edge.id);
    add_to(outgoing[edge.from], edge.id);
    add_to(incoming[edge.to], edge.id);

    domains[edge.domain].edges.insert(edge.id);
    if (edge.cross_domain && !edge.secondary_domain.empty()) {
        domains[edge.secondary_domain].edges.insert(edge.id);
    }

    index_into(edge_by_relation, static_cast<std::uint8_t>(edge.relation), edge.id);
    index_into(edge_by_layer, static_cast<std::uint8_t>(edge.layer), edge.id);
    index_into(edge_by_lifecycle, static_cast<std::uint8_t>(edge.lifecycle), edge.id);
    if (!edge.provenance.publisher.empty()) {
        index_into(edge_by_publisher, edge.provenance.publisher.value(), edge.id);
    }
    if (edge.attachment.has_value()) {
        index_into(edge_by_attachment, edge.attachment->value(), edge.id);
    }
}

void GraphState::unindex_edge(const TopologyEdge& edge) {
    const auto key_it = edge_by_key.find(make_edge_key(edge.relation, edge.from, edge.to, edge.domain));
    if (key_it != edge_by_key.end() && key_it->second == edge.id) {
        edge_by_key.erase(key_it);
    }
    const auto rel_it = edge_by_relationship.find(edge.relationship_id);
    if (rel_it != edge_by_relationship.end() && rel_it->second == edge.id) {
        edge_by_relationship.erase(rel_it);
    }

    // Buckets are erased once empty so the maintained index is byte-for-byte equivalent to a
    // freshly rebuilt one: no stale empty entries survive a mutation.
    auto erase_adjacency = [&edge](std::unordered_map<TopologyNodeId, EdgeBuckets>& index,
                                   const TopologyNodeId& key) {
        const auto it = index.find(key);
        if (it != index.end()) {
            remove_from(it->second, edge.id);
            if (it->second.empty()) {
                index.erase(it);
            }
        }
    };
    erase_adjacency(incident, edge.from);
    if (edge.to != edge.from) {
        erase_adjacency(incident, edge.to);
    }
    erase_adjacency(outgoing, edge.from);
    erase_adjacency(incoming, edge.to);

    const auto domain_it = domains.find(edge.domain);
    if (domain_it != domains.end()) {
        domain_it->second.edges.erase(edge.id);
        if (domain_it->second.edges.empty() && domain_it->second.nodes.empty() &&
            domain_definitions.find(domain_it->first) == domain_definitions.end()) {
            domains.erase(domain_it);
        }
    }
    if (edge.cross_domain && !edge.secondary_domain.empty()) {
        const auto secondary_it = domains.find(edge.secondary_domain);
        if (secondary_it != domains.end()) {
            secondary_it->second.edges.erase(edge.id);
        }
    }

    unindex_from(edge_by_relation, static_cast<std::uint8_t>(edge.relation), edge.id);
    unindex_from(edge_by_layer, static_cast<std::uint8_t>(edge.layer), edge.id);
    unindex_from(edge_by_lifecycle, static_cast<std::uint8_t>(edge.lifecycle), edge.id);
    if (!edge.provenance.publisher.empty()) {
        unindex_from(edge_by_publisher, edge.provenance.publisher.value(), edge.id);
    }
    if (edge.attachment.has_value()) {
        unindex_from(edge_by_attachment, edge.attachment->value(), edge.id);
    }
}

bool GraphState::insert_node(const TopologyNode& node) {
    if (nodes.find(node.id) != nodes.end()) {
        return false;
    }
    index_node(node);
    nodes.emplace(node.id, node);
    return true;
}

bool GraphState::erase_node(const TopologyNodeId& id) {
    const auto it = nodes.find(id);
    if (it == nodes.end()) {
        return false;
    }
    const TopologyNode node = it->second;
    nodes.erase(it);
    unindex_node(node);
    return true;
}

bool GraphState::update_node(const TopologyNode& node) {
    const auto it = nodes.find(node.id);
    if (it == nodes.end()) {
        return false;
    }
    unindex_node(it->second);
    it->second = node;
    index_node(node);
    return true;
}

bool GraphState::insert_edge(const TopologyEdge& edge) {
    if (edges.find(edge.id) != edges.end()) {
        return false;
    }
    index_edge(edge);
    edges.emplace(edge.id, edge);
    return true;
}

bool GraphState::erase_edge(const TopologyEdgeId& id) {
    const auto it = edges.find(id);
    if (it == edges.end()) {
        return false;
    }
    const TopologyEdge edge = it->second;
    edges.erase(it);
    unindex_edge(edge);
    return true;
}

bool GraphState::update_edge(const TopologyEdge& edge) {
    const auto it = edges.find(edge.id);
    if (it == edges.end()) {
        return false;
    }
    unindex_edge(it->second);
    it->second = edge;
    index_edge(edge);
    return true;
}

void GraphState::rebuild_indexes() {
    node_by_entity.clear();
    edge_by_key.clear();
    edge_by_relationship.clear();
    incident.clear();
    outgoing.clear();
    incoming.clear();
    domains.clear();
    edge_by_relation.clear();
    edge_by_layer.clear();
    edge_by_lifecycle.clear();
    edge_by_publisher.clear();
    edge_by_attachment.clear();

    for (const auto& entry : domain_definitions) {
        domains[entry.first];
    }
    for (const auto& entry : nodes) {
        index_node(entry.second);
    }
    for (const auto& entry : edges) {
        index_edge(entry.second);
    }
}

Status GraphState::verify_indexes() const {
    GraphState reference(limits);
    reference.nodes = nodes;
    reference.edges = edges;
    // Declared domains exist in the index even before they hold members, so the reference
    // must be seeded with the same declarations.
    reference.domain_definitions = domain_definitions;
    reference.rebuild_indexes();

    if (!maps_equal(node_by_entity, reference.node_by_entity)) {
        return index_failure("index.node_by_entity");
    }
    if (!maps_equal(edge_by_key, reference.edge_by_key)) {
        return index_failure("index.edge_by_key");
    }
    if (!maps_equal(edge_by_relationship, reference.edge_by_relationship)) {
        return index_failure("index.edge_by_relationship");
    }
    if (!bucket_maps_equal(edge_by_relation, reference.edge_by_relation)) {
        return index_failure("index.edge_by_relation");
    }
    if (!bucket_maps_equal(edge_by_layer, reference.edge_by_layer)) {
        return index_failure("index.edge_by_layer");
    }
    if (!bucket_maps_equal(edge_by_lifecycle, reference.edge_by_lifecycle)) {
        return index_failure("index.edge_by_lifecycle");
    }
    if (!bucket_maps_equal(edge_by_publisher, reference.edge_by_publisher)) {
        return index_failure("index.edge_by_publisher");
    }
    if (!bucket_maps_equal(edge_by_attachment, reference.edge_by_attachment)) {
        return index_failure("index.edge_by_attachment");
    }
    if (incident.size() != reference.incident.size() || outgoing.size() != reference.outgoing.size() ||
        incoming.size() != reference.incoming.size()) {
        return index_failure("index.adjacency_size");
    }
    for (const auto& entry : reference.incident) {
        if (sorted_edges_of(incident, entry.first) != sorted_edges_of(reference.incident, entry.first)) {
            return index_failure("index.incident");
        }
    }
    for (const auto& entry : reference.outgoing) {
        if (sorted_edges_of(outgoing, entry.first) != sorted_edges_of(reference.outgoing, entry.first)) {
            return index_failure("index.outgoing");
        }
    }
    for (const auto& entry : reference.incoming) {
        if (sorted_edges_of(incoming, entry.first) != sorted_edges_of(reference.incoming, entry.first)) {
            return index_failure("index.incoming");
        }
    }
    if (domains.size() != reference.domains.size()) {
        return index_failure("index.domains_size");
    }
    for (const auto& entry : reference.domains) {
        const auto it = domains.find(entry.first);
        if (it == domains.end()) {
            return index_failure("index.domain_missing");
        }
        if (it->second.nodes != entry.second.nodes || it->second.edges != entry.second.edges) {
            return index_failure("index.domain_membership");
        }
    }
    Status status;
    status.set_outcome(Outcome::Ok).set_code("index.ok");
    return status;
}

const TopologyNode* GraphState::find_node(const TopologyNodeId& id) const {
    const auto it = nodes.find(id);
    return it == nodes.end() ? nullptr : &it->second;
}

const TopologyEdge* GraphState::find_edge(const TopologyEdgeId& id) const {
    const auto it = edges.find(id);
    return it == edges.end() ? nullptr : &it->second;
}

const TopologyNodeId* GraphState::find_node_by_entity(std::string_view entity_id) const {
    const auto it = node_by_entity.find(std::string(entity_id));
    return it == node_by_entity.end() ? nullptr : &it->second;
}

const TopologyEdgeId* GraphState::find_edge_by_key(const EdgeKey& key) const {
    const auto it = edge_by_key.find(key);
    return it == edge_by_key.end() ? nullptr : &it->second;
}

const TopologyEdgeId* GraphState::find_edge_by_relationship(const RelationshipId& id) const {
    const auto it = edge_by_relationship.find(id);
    return it == edge_by_relationship.end() ? nullptr : &it->second;
}

bool GraphState::node_in_scope(const TopologyNode& node, const SnapshotScope& scope) const {
    if (!scope.whole_topology && node.domain != scope.domain) {
        return false;
    }
    return true;
}

bool GraphState::edge_in_scope(const TopologyEdge& edge, const SnapshotScope& scope) const {
    if (!scope.whole_topology && edge.domain != scope.domain &&
        !(edge.cross_domain && edge.secondary_domain == scope.domain)) {
        return false;
    }
    if (scope.filter_relation && edge.relation != scope.relation) {
        return false;
    }
    if (scope.filter_layer && edge.layer != scope.layer) {
        return false;
    }
    return true;
}

std::vector<TopologyNode> GraphState::sorted_nodes(const SnapshotScope& scope) const {
    std::vector<TopologyNode> result;
    result.reserve(nodes.size());
    for (const auto& entry : nodes) {
        if (node_in_scope(entry.second, scope)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TopologyNode& a, const TopologyNode& b) { return a.id < b.id; });
    return result;
}

std::vector<TopologyEdge> GraphState::sorted_edges(const SnapshotScope& scope) const {
    std::vector<TopologyEdge> result;
    result.reserve(edges.size());
    for (const auto& entry : edges) {
        if (edge_in_scope(entry.second, scope)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TopologyEdge& a, const TopologyEdge& b) { return a.id < b.id; });
    return result;
}

std::vector<TopologyEdgeId> GraphState::sorted_incident(const TopologyNodeId& id) const {
    const auto it = incident.find(id);
    return it == incident.end() ? std::vector<TopologyEdgeId>{} : sorted_ids(it->second);
}

bool GraphState::would_create_cycle(RelationClass relation, const TopologyNodeId& from,
                                    const TopologyNodeId& to) const {
    if (relation_rules(relation).cycle_rule != CycleRule::Acyclic) {
        return false;
    }
    if (from == to) {
        return true;
    }
    // Iterative DFS from \`to\` following edges of the same relation class in their stored
    // direction. Reaching \`from\` means from -> to would close a cycle.
    std::vector<TopologyNodeId> stack;
    std::unordered_set<TopologyNodeId> visited;
    stack.push_back(to);
    while (!stack.empty()) {
        const TopologyNodeId current = stack.back();
        stack.pop_back();
        if (current == from) {
            return true;
        }
        if (!visited.insert(current).second) {
            continue;
        }
        const auto it = outgoing.find(current);
        if (it == outgoing.end()) {
            continue;
        }
        for (const TopologyEdgeId& edge_id : it->second) {
            const TopologyEdge* edge = find_edge(edge_id);
            if (edge == nullptr || edge->relation != relation ||
                edge->lifecycle == LifecycleState::Retired || edge->lifecycle == LifecycleState::Superseded) {
                continue;
            }
            stack.push_back(edge->to);
        }
    }
    return false;
}

std::string GraphState::topology_digest(const SnapshotScope& scope) const {
    RecordWriter writer(4096);
    writer.text("FABRIC-TOPOLOGY-DIGEST");
    writer.u32(1);
    writer.boolean(scope.whole_topology);
    writer.text(scope.domain.value());
    writer.boolean(scope.filter_relation);
    writer.u8(static_cast<std::uint8_t>(scope.relation));
    writer.boolean(scope.filter_layer);
    writer.u8(static_cast<std::uint8_t>(scope.layer));

    const std::vector<TopologyNode> node_list = sorted_nodes(scope);
    const std::vector<TopologyEdge> edge_list = sorted_edges(scope);
    writer.u64(node_list.size());
    writer.u64(edge_list.size());
    for (const TopologyNode& node : node_list) {
        canonical_encode_node(node, writer);
    }
    for (const TopologyEdge& edge : edge_list) {
        canonical_encode_edge(edge, writer);
    }
    return sha256_hex(writer.data());
}

std::string GraphState::render_canonical(const SnapshotScope& scope) const {
    std::string out;
    out.reserve(1024);
    out += "generation=";
    out += generation.to_string();
    out += "\n";
    const std::vector<TopologyNode> node_list = sorted_nodes(scope);
    const std::vector<TopologyEdge> edge_list = sorted_edges(scope);
    out += "nodes=";
    out += std::to_string(node_list.size());
    out += "\n";
    for (const TopologyNode& node : node_list) {
        out += render_node(node);
        out += "\n";
    }
    out += "edges=";
    out += std::to_string(edge_list.size());
    out += "\n";
    for (const TopologyEdge& edge : edge_list) {
        out += render_edge(edge);
        out += "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// StateTxn
// ---------------------------------------------------------------------------

void StateTxn::note_node_inserted(const TopologyNodeId& id) {
    Entry entry;
    entry.kind = Kind::NodeInserted;
    entry.node_id = id;
    entries_.push_back(std::move(entry));
}

void StateTxn::note_node_erased(const TopologyNode& previous) {
    Entry entry;
    entry.kind = Kind::NodeErased;
    entry.node_id = previous.id;
    entry.node = previous;
    entries_.push_back(std::move(entry));
}

void StateTxn::note_node_updated(const TopologyNode& previous) {
    Entry entry;
    entry.kind = Kind::NodeUpdated;
    entry.node_id = previous.id;
    entry.node = previous;
    entries_.push_back(std::move(entry));
}

void StateTxn::note_edge_inserted(const TopologyEdgeId& id) {
    Entry entry;
    entry.kind = Kind::EdgeInserted;
    entry.edge_id = id;
    entries_.push_back(std::move(entry));
}

void StateTxn::note_edge_erased(const TopologyEdge& previous) {
    Entry entry;
    entry.kind = Kind::EdgeErased;
    entry.edge_id = previous.id;
    entry.edge = previous;
    entries_.push_back(std::move(entry));
}

void StateTxn::note_edge_updated(const TopologyEdge& previous) {
    Entry entry;
    entry.kind = Kind::EdgeUpdated;
    entry.edge_id = previous.id;
    entry.edge = previous;
    entries_.push_back(std::move(entry));
}

void StateTxn::note_generation(TopologyGeneration previous) {
    Entry entry;
    entry.kind = Kind::GenerationChanged;
    entry.generation = previous;
    entries_.push_back(std::move(entry));
}

void StateTxn::note_domain(const TopologyDomainId& id, bool existed) {
    if (existed) {
        return;
    }
    Entry entry;
    entry.kind = Kind::DomainCreated;
    entry.domain = id;
    entries_.push_back(std::move(entry));
}

void StateTxn::rollback() {
    if (!active_) {
        return;
    }
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        const Entry& entry = *it;
        switch (entry.kind) {
            case Kind::NodeInserted:
                state_.erase_node(entry.node_id);
                break;
            case Kind::NodeErased:
                state_.insert_node(entry.node);
                break;
            case Kind::NodeUpdated:
                state_.update_node(entry.node);
                break;
            case Kind::EdgeInserted:
                state_.erase_edge(entry.edge_id);
                break;
            case Kind::EdgeErased:
                state_.insert_edge(entry.edge);
                break;
            case Kind::EdgeUpdated:
                state_.update_edge(entry.edge);
                break;
            case Kind::GenerationChanged:
                state_.generation = entry.generation;
                break;
            case Kind::DomainCreated:
                state_.domains.erase(entry.domain);
                break;
        }
    }
    entries_.clear();
    active_ = false;
}

// ---------------------------------------------------------------------------
// Derived identifiers
// ---------------------------------------------------------------------------

TopologyNodeId derive_node_id(const TopologyDomainId& /*domain*/, std::string_view entity_id) {
    // A canonical entity participates in the runtime as exactly one topology node, so the
    // identifier is derived from the entity identity alone, using a bounded and deterministic
    // encoding that can never exceed the identifier length limit.
    std::string seed = "node|";
    seed.append(entity_id.data(), entity_id.size());
    std::string value = "n_";
    value += hex_prefix_of_digest(seed);
    return TopologyNodeId::from_trusted(std::move(value));
}

TopologyEdgeId derive_edge_id(const EdgeKey& key) {
    std::string seed = "edge|";
    seed += render_edge_key(key);
    std::string value = "e_";
    value += hex_prefix_of_digest(seed);
    return TopologyEdgeId::from_trusted(std::move(value));
}

std::string derive_relationship_id(const EdgeKey& key) {
    std::string seed = "rel|";
    seed += render_edge_key(key);
    std::string value = "r_";
    value += hex_prefix_of_digest(seed);
    return value;
}

std::string render_generation(TopologyGeneration generation) {
    return generation.to_string();
}

void normalize_undirected_endpoints(RelationClass relation, TopologyNodeId& from, TopologyNodeId& to) {
    if (relation_rules(relation).direction == Directionality::Undirected && to < from) {
        const TopologyNodeId swap = from;
        from = to;
        to = swap;
    }
}

std::optional<TopologyLayer> resolve_layer(RelationClass relation, std::optional<TopologyLayer> requested) {
    switch (relation_rules(relation).layer_policy) {
        case LayerPolicy::PhysicalOnly:
            if (requested.has_value() && *requested != TopologyLayer::Physical) {
                return std::nullopt;
            }
            return TopologyLayer::Physical;
        case LayerPolicy::LogicalOnly:
            if (requested.has_value() && *requested != TopologyLayer::Logical) {
                return std::nullopt;
            }
            return TopologyLayer::Logical;
        case LayerPolicy::Either:
            return requested;
    }
    return std::nullopt;
}

}  // namespace fabric_topology::internal
