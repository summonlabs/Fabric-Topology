// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Relationship validation shared by every mutation path, and the deterministic reconciler
// that turns an untrusted publication into authoritative topology.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "engine_impl.hpp"
#include "fabric_topology/digest.hpp"
#include "fabric_topology/topology.hpp"
#include "graph_state.hpp"
#include "validation.hpp"

namespace fabric_topology::internal {

namespace {

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] bool topology_edge_less(const TopologyEdge& a, const TopologyEdge& b) {
    return a.id < b.id;
}

}  // namespace

// ---------------------------------------------------------------------------
// Domain resolution
// ---------------------------------------------------------------------------

DomainResolution resolve_relationship_domain(const GraphState& state, const TopologyDomainId& from_domain,
                                             const TopologyDomainId& to_domain,
                                             const TopologyDomainId& requested, RelationClass relation,
                                             bool allow_cross_domain) {
    DomainResolution result;
    if (from_domain.empty() || to_domain.empty()) {
        result.status = make_status(Outcome::DomainViolation, "domain.endpoint_scope_missing");
        return result;
    }
    if (from_domain == to_domain) {
        if (!requested.empty() && requested != from_domain) {
            result.status = make_status(Outcome::DomainViolation, "domain.requested_scope_mismatch");
            result.status.with("requested", requested.to_string());
            result.status.with("endpoint_scope", from_domain.to_string());
            return result;
        }
        result.ok = true;
        result.domain = from_domain;
        result.status = make_status(Outcome::Ok, "domain.resolved");
        return result;
    }

    // Cross-domain relationship: requires an explicit engine-configured rule, an explicit
    // request flag and an unambiguous primary scope.
    if (!allow_cross_domain) {
        result.status = make_status(Outcome::DomainViolation, "domain.cross_domain_not_permitted");
        result.status.with("from_domain", from_domain.to_string());
        result.status.with("to_domain", to_domain.to_string());
        return result;
    }
    if (requested.empty() || (requested != from_domain && requested != to_domain)) {
        result.status = make_status(Outcome::DomainViolation, "domain.cross_domain_scope_ambiguous");
        result.status.with("from_domain", from_domain.to_string());
        result.status.with("to_domain", to_domain.to_string());
        return result;
    }

    bool permitted = false;
    for (const CrossDomainRule& rule : state.cross_domain_rules) {
        if ((rule.from_domain == from_domain && rule.to_domain == to_domain) ||
            (rule.symmetric && rule.from_domain == to_domain && rule.to_domain == from_domain)) {
            if (rule.relation == relation) {
                permitted = true;
                break;
            }
        }
    }
    if (!permitted) {
        result.status = make_status(Outcome::DomainViolation, "domain.cross_domain_rule_missing");
        result.status.with("from_domain", from_domain.to_string());
        result.status.with("to_domain", to_domain.to_string());
        result.status.with("relation", to_string(relation));
        return result;
    }

    result.ok = true;
    result.domain = requested;
    result.cross_domain = true;
    result.secondary_domain = requested == from_domain ? to_domain : from_domain;
    result.status = make_status(Outcome::Ok, "domain.cross_domain_resolved");
    return result;
}

// ---------------------------------------------------------------------------
// Relationship validation
// ---------------------------------------------------------------------------

RelationshipValidation validate_relationship_candidate(const GraphState& state,
                                                       const RelationshipCandidate& candidate,
                                                       bool is_new) {
    RelationshipValidation result;

    if (candidate.relation == RelationClass::Unknown) {
        result.status = make_status(Outcome::MalformedRequest, "relationship.relation_unknown");
        return result;
    }
    if (candidate.id.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "relationship.id_missing");
        return result;
    }
    if (candidate.relationship_id.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "relationship.relationship_id_missing");
        return result;
    }

    const TopologyNode* from_node = state.find_node(candidate.from);
    if (from_node == nullptr) {
        result.status = make_status(Outcome::UnknownEntity, "relationship.from_unknown");
        result.status.with("from", candidate.from.to_string());
        return result;
    }
    const TopologyNode* to_node = state.find_node(candidate.to);
    if (to_node == nullptr) {
        result.status = make_status(Outcome::UnknownEntity, "relationship.to_unknown");
        result.status.with("to", candidate.to.to_string());
        return result;
    }

    if (from_node->lifecycle == LifecycleState::Retired) {
        result.status = make_status(Outcome::Retired, "relationship.from_retired");
        result.status.with("from", candidate.from.to_string());
        return result;
    }
    if (from_node->lifecycle == LifecycleState::Superseded) {
        result.status = make_status(Outcome::Superseded, "relationship.from_superseded");
        result.status.with("from", candidate.from.to_string());
        return result;
    }
    if (to_node->lifecycle == LifecycleState::Retired) {
        result.status = make_status(Outcome::Retired, "relationship.to_retired");
        result.status.with("to", candidate.to.to_string());
        return result;
    }
    if (to_node->lifecycle == LifecycleState::Superseded) {
        result.status = make_status(Outcome::Superseded, "relationship.to_superseded");
        result.status.with("to", candidate.to.to_string());
        return result;
    }

    const RelationRules& rules = relation_rules(candidate.relation);

    if (!relation_endpoints_permitted(candidate.relation, from_node->node_class, to_node->node_class)) {
        result.status = make_status(Outcome::IncompatibleEntityClass, "relationship.endpoint_class_pairing");
        result.status.with("relation", to_string(candidate.relation));
        result.status.with("from_class", to_string(from_node->node_class));
        result.status.with("to_class", to_string(to_node->node_class));
        return result;
    }
    if (!relation_layer_permitted(candidate.relation, candidate.layer)) {
        result.status = make_status(Outcome::MalformedRequest, "relationship.layer_policy");
        result.status.with("relation", to_string(candidate.relation));
        result.status.with("layer", to_string(candidate.layer));
        return result;
    }
    if (candidate.from == candidate.to && !rules.self_edge_allowed) {
        result.status = make_status(Outcome::InvalidEndpoint, "relationship.self_link");
        result.status.with("node", candidate.from.to_string());
        result.status.with("relation", to_string(candidate.relation));
        return result;
    }

    if (candidate.supported_by.has_value()) {
        const TopologyEdge* support = state.find_edge(*candidate.supported_by);
        if (support == nullptr) {
            result.status = make_status(Outcome::UnknownEntity, "relationship.support_unknown");
            result.status.with("supported_by", candidate.supported_by->to_string());
            return result;
        }
        if (*candidate.supported_by == candidate.id) {
            result.status = make_status(Outcome::CycleRejected, "relationship.self_support");
            return result;
        }
        if (candidate.layer == TopologyLayer::Logical &&
            support->layer != TopologyLayer::Physical) {
            result.status = make_status(Outcome::RelationshipConflict, "relationship.support_not_physical");
            result.status.with("supported_by", support->id.to_string());
            result.status.with("support_layer", to_string(support->layer));
            return result;
        }
    }

    if (candidate.exclusive_attachment && candidate.attachment.has_value()) {
        const auto it = state.edge_by_attachment.find(candidate.attachment->value());
        if (it != state.edge_by_attachment.end()) {
            for (const TopologyEdgeId& edge_id : it->second) {
                if (edge_id == candidate.id) {
                    continue;
                }
                const TopologyEdge* other = state.find_edge(edge_id);
                if (other == nullptr || other->lifecycle != LifecycleState::Current) {
                    continue;
                }
                if (other->to == candidate.to && other->from == candidate.from) {
                    continue;
                }
                result.status = make_status(Outcome::RelationshipConflict, "relationship.attachment_conflict");
                result.status.with("attachment", candidate.attachment->value());
                result.status.with("conflicting_edge", other->id.to_string());
                return result;
            }
        }
        // One current exclusive attachment per endpoint.
        const auto incident_it = state.incident.find(candidate.from);
        if (incident_it != state.incident.end()) {
            for (const TopologyEdgeId& edge_id : incident_it->second) {
                if (edge_id == candidate.id) {
                    continue;
                }
                const TopologyEdge* other = state.find_edge(edge_id);
                if (other == nullptr || other->relation != RelationClass::AttachedTo ||
                    other->lifecycle != LifecycleState::Current || !other->attachment.has_value()) {
                    continue;
                }
                if (other->from != candidate.from) {
                    continue;
                }
                result.status = make_status(Outcome::RelationshipConflict, "relationship.exclusive_attachment_exists");
                result.status.with("endpoint", candidate.from.to_string());
                result.status.with("existing_edge", other->id.to_string());
                result.status.with("existing_attachment", other->attachment->value());
                return result;
            }
        }
    }

    if (is_new && state.would_create_cycle(candidate.relation, candidate.from, candidate.to)) {
        result.status = make_status(Outcome::CycleRejected, "relationship.cycle");
        result.status.with("relation", to_string(candidate.relation));
        result.status.with("from", candidate.from.to_string());
        result.status.with("to", candidate.to.to_string());
        result.status.with("cycle_rule", "ACYCLIC");
        return result;
    }

    result.ok = true;
    result.status = make_status(Outcome::Ok, "relationship.valid");
    return result;
}

}  // namespace fabric_topology::internal

namespace fabric_topology {

namespace {

using internal::GraphState;
using internal::RelationshipCandidate;

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

struct MaterializedCounts {
    std::size_t nodes_added = 0;
    std::size_t nodes_updated = 0;
    std::size_t nodes_removed = 0;
    std::size_t nodes_unchanged = 0;
    std::size_t edges_added = 0;
    std::size_t edges_updated = 0;
    std::size_t edges_removed = 0;
    std::size_t edges_superseded = 0;
    std::size_t edges_unchanged = 0;
    std::size_t edges_conflicted = 0;
    std::size_t edges_rejected = 0;
};

struct RemovalPlan {
    std::vector<TopologyEdgeId> edges;
    std::vector<TopologyNodeId> nodes;
};

[[nodiscard]] bool diff_entry_less(const DiffEntry& a, const DiffEntry& b) {
    const auto left = static_cast<std::uint8_t>(a.kind);
    const auto right = static_cast<std::uint8_t>(b.kind);
    if (left != right) {
        return left < right;
    }
    return a.key < b.key;
}

}  // namespace

PublicationResult TopologyEngine::publish(const AuthorityContext& authority,
                                          const Publication& publication) {
    PublicationResult result;
    result.generation = generation();

    if (publication.id.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "publication.id_missing");
        return result;
    }
    if (publication.domain.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "publication.domain_missing");
        return result;
    }
    if (publication.note.size() > impl_->limits.max_string_bytes) {
        result.status = make_status(Outcome::ResourceLimit, "publication.note_too_long");
        return result;
    }
    if (publication.nodes.size() > impl_->limits.max_nodes_per_publication) {
        Status status = make_status(Outcome::ResourceLimit, "publication.too_many_nodes");
        status.with("declared", std::to_string(publication.nodes.size()));
        status.with("limit", std::to_string(impl_->limits.max_nodes_per_publication));
        result.status = status;
        return result;
    }
    if (publication.edges.size() > impl_->limits.max_edges_per_publication) {
        Status status = make_status(Outcome::ResourceLimit, "publication.too_many_edges");
        status.with("declared", std::to_string(publication.edges.size()));
        status.with("limit", std::to_string(impl_->limits.max_edges_per_publication));
        result.status = status;
        return result;
    }
    if (publication.declared_absent_relationships.size() > impl_->limits.max_edges_per_publication) {
        result.status = make_status(Outcome::ResourceLimit, "publication.too_many_absences");
        return result;
    }
    for (const ObservedNode& observed : publication.nodes) {
        const Status metadata_status = Metadata::validate(observed.metadata.items(), impl_->limits);
        if (metadata_status.outcome() != Outcome::Ok) {
            result.status = metadata_status;
            return result;
        }
        if (observed.entity_id.empty() ||
            observed.entity_id.size() > impl_->limits.max_identifier_bytes) {
            result.status = make_status(Outcome::MalformedRequest, "publication.node_entity_invalid");
            return result;
        }
    }
    for (const ObservedEdge& observed : publication.edges) {
        const Status metadata_status = Metadata::validate(observed.metadata.items(), impl_->limits);
        if (metadata_status.outcome() != Outcome::Ok) {
            result.status = metadata_status;
            return result;
        }
    }

    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    // An undeclared scope is a malformed request, not an authority problem.
    if (impl_->graph.domain_definitions.find(publication.domain) ==
        impl_->graph.domain_definitions.end()) {
        impl_->note_rejected();
        ++impl_->publications_rejected;
        Status status = make_status(Outcome::DomainViolation, "publication.domain_unknown");
        status.with("domain", publication.domain.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    const GrantMode required = publication.mode == PublicationMode::AuthoritativeSnapshot
                                   ? GrantMode::AuthoritativeWrite
                                   : GrantMode::IncrementalWrite;
    PublisherState* publisher = nullptr;
    Status authority_status =
        impl_->check_authority_locked(authority, publication.domain, required, &publisher);
    if (authority_status.outcome() != Outcome::Ok) {
        impl_->note_rejected();
        ++impl_->publications_rejected;
        result.status = authority_status;
        result.generation = impl_->graph.generation;
        return result;
    }
    result.status.with("publisher", authority.publisher.to_string());
    result.status.with("publication", publication.id.to_string());
    result.status.with("mode", to_string(publication.mode));
    result.status.with("domain", publication.domain.to_string());

    const bool expects_generation = !publication.expected_generation.is_zero();
    if (expects_generation && publication.expected_generation != impl_->graph.generation) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        ++impl_->publications_rejected;
        Status status = make_status(Outcome::StaleGeneration, "publication.stale_generation");
        status.with("expected", publication.expected_generation.to_string());
        status.with("current", impl_->graph.generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    if (impl_->attempts.seen(publication.id.value())) {
        impl_->note_idempotent();
        ++publisher->committed_mutations;
        result.status = make_status(Outcome::Idempotent, "publication.replayed");
        result.generation = impl_->graph.generation;
        result.nodes_unchanged = publication.nodes.size();
        result.edges_unchanged = publication.edges.size();
        return result;
    }

    // ---- build the candidate state -------------------------------------------------
    GraphState candidate = impl_->graph;
    std::vector<DiffEntry> diff;

    std::unordered_set<std::string> seen_node_ids;
    std::unordered_set<std::string> seen_entity_ids;
    std::unordered_set<std::string> seen_edge_keys;
    std::unordered_set<std::string> seen_relationship_ids;
    std::unordered_set<std::string> mentioned_edge_keys;
    std::unordered_set<std::string> mentioned_node_ids;
    MaterializedCounts counts;
    std::size_t stale_entries = 0;
    std::string stale_detail;

    auto reject = [&](Status status) -> PublicationResult {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        ++impl_->publications_rejected;
        result.status = std::move(status);
        result.generation = impl_->graph.generation;
        return result;
    };

    // ---- nodes ---------------------------------------------------------------------
    std::vector<const ObservedNode*> observed_nodes;
    observed_nodes.reserve(publication.nodes.size());
    for (const ObservedNode& observed : publication.nodes) {
        observed_nodes.push_back(&observed);
    }
    std::sort(observed_nodes.begin(), observed_nodes.end(),
              [](const ObservedNode* a, const ObservedNode* b) { return a->entity_id < b->entity_id; });

    for (const ObservedNode* observed : observed_nodes) {
        const TopologyDomainId domain =
            observed->domain.empty() ? publication.domain : observed->domain;
        if (domain != publication.domain) {
            Status status = make_status(Outcome::DomainViolation, "publication.node_out_of_scope");
            status.with("entity_id", observed->entity_id);
            status.with("entry_domain", domain.to_string());
            status.with("publication_domain", publication.domain.to_string());
            return reject(std::move(status));
        }
        if (observed->node_class == NodeClass::Unknown) {
            Status status = make_status(Outcome::MalformedRequest, "publication.node_class_unknown");
            status.with("entity_id", observed->entity_id);
            return reject(std::move(status));
        }

        const TopologyNodeId node_id = !observed->node_id.empty()
                                           ? observed->node_id
                                           : internal::derive_node_id(domain, observed->entity_id);
        if (!seen_node_ids.insert(node_id.value()).second) {
            Status status = make_status(Outcome::DuplicateIdentity, "publication.duplicate_node");
            status.with("node_id", node_id.to_string());
            return reject(std::move(status));
        }

        if (!observed->present) {
            if (publication.mode == PublicationMode::PartialObservation) {
                Status status = make_status(Outcome::MalformedRequest,
                                            "publication.partial_cannot_assert_node_absence");
                status.with("entity_id", observed->entity_id);
                return reject(std::move(status));
            }
            mentioned_node_ids.insert(node_id.value());
            continue;
        }

        if (!seen_entity_ids.insert(observed->entity_id).second) {
            Status status = make_status(Outcome::DuplicateIdentity, "publication.duplicate_entity");
            status.with("entity_id", observed->entity_id);
            return reject(std::move(status));
        }

        std::optional<EntityRecord> record;
        if (impl_->directory != nullptr) {
            record = impl_->directory->lookup(observed->entity_id);
        }
        if (!record.has_value()) {
            Status status = make_status(Outcome::UnknownEntity, "publication.node_entity_unknown");
            status.with("entity_id", observed->entity_id);
            return reject(std::move(status));
        }
        if (record->retired || record->superseded) {
            Status status = make_status(record->retired ? Outcome::Retired : Outcome::Superseded,
                                        "publication.node_entity_not_live");
            status.with("entity_id", observed->entity_id);
            if (!record->superseded_by.empty()) {
                status.with("superseded_by", record->superseded_by);
            }
            return reject(std::move(status));
        }
        const EntityClass expected_class = node_class_entity_class(observed->node_class);
        if (expected_class == EntityClass::Unknown || record->entity_class != expected_class) {
            Status status = make_status(Outcome::IncompatibleEntityClass,
                                        "publication.node_entity_class_mismatch");
            status.with("entity_id", observed->entity_id);
            status.with("registry_class", to_string(record->entity_class));
            status.with("node_class", to_string(observed->node_class));
            return reject(std::move(status));
        }

        mentioned_node_ids.insert(node_id.value());
        const TopologyNode* existing = candidate.find_node(node_id);
        if (existing == nullptr) {
            if (const TopologyNodeId* bound = candidate.find_node_by_entity(observed->entity_id);
                bound != nullptr) {
                Status status = make_status(Outcome::DuplicateIdentity,
                                            "publication.entity_already_bound");
                status.with("entity_id", observed->entity_id);
                status.with("bound_node", bound->to_string());
                return reject(std::move(status));
            }
            if (candidate.nodes.size() >= impl_->limits.max_nodes) {
                return reject(make_status(Outcome::ResourceLimit, "publication.node_limit"));
            }
            TopologyNode node;
            node.id = node_id;
            node.entity_id = observed->entity_id;
            node.entity_class = record->entity_class;
            node.entity_generation = record->generation;
            node.node_class = observed->node_class;
            node.tier = observed->tier;
            node.domain = domain;
            node.generation = NodeGeneration{1};
            node.created_generation = candidate.generation;
            node.last_validated_generation = candidate.generation;
            node.lifecycle = LifecycleState::Current;
            node.provenance.publisher = authority.publisher;
            node.provenance.worker_boot = authority.worker_boot;
            node.provenance.coordinator_epoch = authority.coordinator_epoch;
            node.provenance.source = publication.source;
            node.provenance.evidence_type = publication.type;
            node.provenance.evidence_generation = publication.evidence_generation;
            node.provenance.publication = publication.id;
            node.provenance.created_generation = candidate.generation;
            node.provenance.last_validated_generation = candidate.generation;
            node.metadata = observed->metadata;
            candidate.insert_node(node);

            DiffEntry entry;
            entry.kind = DiffKind::NodeAdded;
            entry.key = node_id.to_string();
            entry.node = node_id;
            entry.after = render_node(node);
            diff.push_back(std::move(entry));
            ++counts.nodes_added;
        } else {
            if (existing->entity_id != observed->entity_id) {
                Status status = make_status(Outcome::DuplicateIdentity, "publication.node_id_reused");
                status.with("node_id", node_id.to_string());
                return reject(std::move(status));
            }
            TopologyNode node = *existing;
            const bool unchanged = node.entity_generation == record->generation &&
                                   node.tier == observed->tier &&
                                   node.node_class == observed->node_class &&
                                   node.metadata.items() == observed->metadata.items() &&
                                   node.lifecycle == LifecycleState::Current;
            if (unchanged) {
                ++counts.nodes_unchanged;
                continue;
            }
            const EntityGeneration previous_generation = node.entity_generation;
            node.entity_generation = record->generation;
            node.tier = observed->tier;
            node.metadata = observed->metadata;
            node.lifecycle = LifecycleState::Current;
            node.last_validated_generation = candidate.generation;
            if (node.generation.can_advance()) {
                node.generation = node.generation.next();
            }
            candidate.update_node(node);

            DiffEntry entry;
            entry.kind = DiffKind::NodeGenerationChanged;
            entry.key = node_id.to_string();
            entry.node = node_id;
            entry.before = previous_generation.to_string();
            entry.after = node.entity_generation.to_string();
            diff.push_back(std::move(entry));
            ++counts.nodes_updated;

            if (previous_generation != node.entity_generation) {
                const auto incident_it = candidate.incident.find(node_id);
                std::vector<TopologyEdgeId> affected =
                    incident_it == candidate.incident.end()
                        ? std::vector<TopologyEdgeId>{}
                        : std::vector<TopologyEdgeId>(incident_it->second.begin(),
                                                      incident_it->second.end());
                std::sort(affected.begin(), affected.end());
                for (const TopologyEdgeId& edge_id : affected) {
                    const TopologyEdge* edge_found = candidate.find_edge(edge_id);
                    if (edge_found == nullptr) {
                        continue;
                    }
                    TopologyEdge edge = *edge_found;
                    if (edge.lifecycle == LifecycleState::Retired ||
                        edge.lifecycle == LifecycleState::Superseded) {
                        continue;
                    }
                    const LifecycleState previous_lifecycle = edge.lifecycle;
                    if (edge.from == node_id) {
                        edge.from_entity_generation = node.entity_generation;
                    }
                    if (edge.to == node_id) {
                        edge.to_entity_generation = node.entity_generation;
                    }
                    edge.lifecycle = LifecycleState::RevalidationRequired;
                    if (edge.generation.can_advance()) {
                        edge.generation = edge.generation.next();
                    }
                    candidate.update_edge(edge);
                    DiffEntry currentness;
                    currentness.kind = DiffKind::CurrentnessChanged;
                    currentness.key = edge.id.to_string();
                    currentness.edge = edge.id;
                    currentness.before = to_string(previous_lifecycle);
                    currentness.after = to_string(LifecycleState::RevalidationRequired);
                    diff.push_back(std::move(currentness));
                }
            }
        }
    }

    // ---- explicit relationship absences ---------------------------------------------
    const std::vector<RelationshipId>& absences = publication.declared_absent_relationships;
    std::vector<const ObservedEdge*> observed_edges;
    observed_edges.reserve(publication.edges.size());
    for (const ObservedEdge& observed : publication.edges) {
        observed_edges.push_back(&observed);
    }

    // ---- edges ---------------------------------------------------------------------
    std::sort(observed_edges.begin(), observed_edges.end(), [](const ObservedEdge* a, const ObservedEdge* b) {
        return render_edge_key(make_edge_key(a->relation, a->from, a->to, a->domain)) <
               render_edge_key(make_edge_key(b->relation, b->from, b->to, b->domain));
    });

    for (const ObservedEdge* observed : observed_edges) {
        if (observed->relation == RelationClass::Unknown) {
            Status status = make_status(Outcome::MalformedRequest, "publication.edge_relation_unknown");
            status.with("relationship_id", observed->relationship_id.to_string());
            return reject(std::move(status));
        }
        if (observed->from.empty() || observed->to.empty()) {
            return reject(make_status(Outcome::MalformedRequest, "publication.edge_endpoint_missing"));
        }
        const TopologyDomainId raw_domain = observed->domain.empty() ? publication.domain : observed->domain;
        if (raw_domain != publication.domain) {
            Status status = make_status(Outcome::DomainViolation, "publication.edge_out_of_scope");
            status.with("entry_domain", raw_domain.to_string());
            return reject(std::move(status));
        }

        const EdgeKey key =
            make_edge_key(observed->relation, observed->from, observed->to, raw_domain);
        const std::string key_text = render_edge_key(key);
        if (!seen_edge_keys.insert(key_text).second) {
            Status status = make_status(Outcome::DuplicateIdentity, "publication.duplicate_relationship");
            status.with("key", key_text);
            return reject(std::move(status));
        }
        mentioned_edge_keys.insert(key_text);

        if (!observed->present) {
            if (publication.mode == PublicationMode::PartialObservation) {
                Status status = make_status(Outcome::MalformedRequest,
                                            "publication.partial_cannot_assert_absence");
                status.with("key", key_text);
                return reject(std::move(status));
            }
            const TopologyEdgeId* absent_id = candidate.find_edge_by_key(key);
            if (absent_id == nullptr) {
                ++counts.edges_unchanged;
                continue;
            }
            const TopologyEdge* absent = candidate.find_edge(*absent_id);
            if (absent == nullptr || absent->lifecycle == LifecycleState::Retired ||
                absent->lifecycle == LifecycleState::Superseded) {
                ++counts.edges_unchanged;
                continue;
            }
            if (publication.mode == PublicationMode::AuthoritativeSnapshot) {
                const TopologyEdge removed = *absent;
                candidate.erase_edge(removed.id);
                DiffEntry entry;
                entry.kind = DiffKind::EdgeRemoved;
                entry.key = removed.id.to_string();
                entry.edge = removed.id;
                entry.before = render_edge(removed);
                diff.push_back(std::move(entry));
            } else {
                TopologyEdge retired_edge = *absent;
                const LifecycleState previous = retired_edge.lifecycle;
                retired_edge.lifecycle = LifecycleState::Retired;
                if (retired_edge.generation.can_advance()) {
                    retired_edge.generation = retired_edge.generation.next();
                }
                candidate.update_edge(retired_edge);
                DiffEntry entry;
                entry.kind = DiffKind::CurrentnessChanged;
                entry.key = retired_edge.id.to_string();
                entry.edge = retired_edge.id;
                entry.before = to_string(previous);
                entry.after = to_string(LifecycleState::Retired);
                diff.push_back(std::move(entry));
            }
            ++counts.edges_removed;
            continue;
        }

        const std::optional<TopologyLayer> layer =
            internal::resolve_layer(observed->relation, observed->layer);
        if (!layer.has_value()) {
            Status status = make_status(Outcome::MalformedRequest, "publication.edge_layer_required");
            status.with("relation", to_string(observed->relation));
            return reject(std::move(status));
        }

        TopologyNodeId candidate_from = observed->from;
        TopologyNodeId candidate_to = observed->to;
        internal::normalize_undirected_endpoints(observed->relation, candidate_from, candidate_to);
        const EdgeKey normalized_key =
            make_edge_key(observed->relation, candidate_from, candidate_to, raw_domain);
        RelationshipCandidate candidate_edge;
        candidate_edge.id = observed->edge_id.has_value() ? *observed->edge_id
                                                          : internal::derive_edge_id(normalized_key);
        candidate_edge.relationship_id =
            observed->relationship_id.empty()
                ? RelationshipId::from_trusted(internal::derive_relationship_id(normalized_key))
                : observed->relationship_id;
        candidate_edge.relation = observed->relation;
        candidate_edge.from = candidate_from;
        candidate_edge.to = candidate_to;
        candidate_edge.layer = *layer;
        candidate_edge.domain = raw_domain;
        candidate_edge.evidence_generation = observed->evidence_generation;
        candidate_edge.supported_by = observed->supported_by;
        candidate_edge.attachment = observed->attachment;
        candidate_edge.exclusive_attachment = observed->exclusive_attachment;
        candidate_edge.source_relationship_id = observed->source_relationship_id;
        candidate_edge.metadata = observed->metadata;
        candidate_edge.evidence_type = publication.type;
        candidate_edge.source = publication.source;

        if (!seen_relationship_ids.insert(candidate_edge.relationship_id.value()).second) {
            Status status = make_status(Outcome::DuplicateIdentity,
                                        "publication.duplicate_relationship_id");
            status.with("relationship_id", candidate_edge.relationship_id.to_string());
            return reject(std::move(status));
        }

        const TopologyEdgeId* existing_by_key = candidate.find_edge_by_key(normalized_key);
        const TopologyEdge* existing_by_id = candidate.find_edge(candidate_edge.id);
        if (existing_by_key != nullptr && *existing_by_key != candidate_edge.id) {
            Status status = make_status(Outcome::DuplicateEdge, "publication.duplicate_edge");
            status.with("existing", existing_by_key->to_string());
            status.with("requested", candidate_edge.id.to_string());
            return reject(std::move(status));
        }
        if (existing_by_id != nullptr &&
            make_edge_key(existing_by_id->relation, existing_by_id->from, existing_by_id->to,
                          existing_by_id->domain) != normalized_key) {
            Status status = make_status(Outcome::DuplicateIdentity, "publication.edge_id_reused");
            status.with("edge_id", candidate_edge.id.to_string());
            return reject(std::move(status));
        }
        const bool is_new = existing_by_key == nullptr;

        const internal::RelationshipValidation validation =
            internal::validate_relationship_candidate(candidate, candidate_edge, is_new);
        if (!validation.ok) {
            return reject(validation.status);
        }

        if (is_new) {
            if (candidate.edges.size() >= impl_->limits.max_edges) {
                return reject(make_status(Outcome::ResourceLimit, "publication.edge_limit"));
            }
            const TopologyNode* from_node = candidate.find_node(candidate_edge.from);
            const TopologyNode* to_node = candidate.find_node(candidate_edge.to);
            if (from_node == nullptr || to_node == nullptr) {
                return reject(make_status(Outcome::UnknownEntity, "publication.edge_endpoint_unknown"));
            }
            TopologyEdge edge;
            edge.id = candidate_edge.id;
            edge.relationship_id = candidate_edge.relationship_id;
            edge.relation = candidate_edge.relation;
            edge.from = candidate_edge.from;
            edge.to = candidate_edge.to;
            edge.layer = candidate_edge.layer;
            edge.domain = candidate_edge.domain;
            edge.secondary_domain = candidate_edge.domain;
            edge.generation = EdgeGeneration{1};
            edge.created_generation = candidate.generation;
            edge.last_validated_generation = candidate.generation;
            edge.evidence_generation = candidate_edge.evidence_generation;
            edge.from_entity_generation = from_node->entity_generation;
            edge.to_entity_generation = to_node->entity_generation;
            edge.lifecycle = LifecycleState::Current;
            edge.supported_by = candidate_edge.supported_by;
            edge.attachment = candidate_edge.attachment;
            edge.provenance.publisher = authority.publisher;
            edge.provenance.worker_boot = authority.worker_boot;
            edge.provenance.coordinator_epoch = authority.coordinator_epoch;
            edge.provenance.source = publication.source;
            edge.provenance.evidence_type = publication.type;
            edge.provenance.evidence_generation = publication.evidence_generation;
            edge.provenance.source_relationship_id = candidate_edge.source_relationship_id;
            edge.provenance.publication = publication.id;
            edge.provenance.created_generation = candidate.generation;
            edge.provenance.last_validated_generation = candidate.generation;
            edge.metadata = candidate_edge.metadata;
            candidate.insert_edge(edge);

            DiffEntry entry;
            entry.kind = DiffKind::EdgeAdded;
            entry.key = edge.id.to_string();
            entry.edge = edge.id;
            entry.after = render_edge(edge);
            diff.push_back(std::move(entry));
            ++counts.edges_added;
            continue;
        }

        const TopologyEdge& existing = *candidate.find_edge(*existing_by_key);
        if (existing.lifecycle == LifecycleState::Retired ||
            existing.lifecycle == LifecycleState::Superseded) {
            Status status = make_status(Outcome::Retired, "publication.relationship_retired");
            status.with("edge_id", existing.id.to_string());
            status.with("lifecycle", to_string(existing.lifecycle));
            return reject(std::move(status));
        }
        if (existing.evidence_generation > candidate_edge.evidence_generation) {
            if (publication.mode == PublicationMode::AuthoritativeSnapshot) {
                Status status = make_status(Outcome::StaleGeneration, "publication.stale_evidence");
                status.with("edge_id", existing.id.to_string());
                status.with("stored_evidence_generation", existing.evidence_generation.to_string());
                status.with("published_evidence_generation",
                            candidate_edge.evidence_generation.to_string());
                return reject(std::move(status));
            }
            ++stale_entries;
            stale_detail = existing.id.to_string();
            ++counts.edges_unchanged;
            continue;
        }

        const bool unchanged = existing.layer == candidate_edge.layer &&
                               existing.evidence_generation == candidate_edge.evidence_generation &&
                               existing.lifecycle == LifecycleState::Current &&
                               existing.metadata.items() == candidate_edge.metadata.items() &&
                               existing.supported_by == candidate_edge.supported_by &&
                               existing.attachment == candidate_edge.attachment;
        if (unchanged) {
            ++counts.edges_unchanged;
            continue;
        }

        TopologyEdge edge = existing;
        const LifecycleState previous_lifecycle = edge.lifecycle;
        const EvidenceGeneration previous_evidence = edge.evidence_generation;
        edge.layer = candidate_edge.layer;
        edge.evidence_generation = candidate_edge.evidence_generation;
        edge.metadata = candidate_edge.metadata;
        edge.supported_by = candidate_edge.supported_by;
        edge.attachment = candidate_edge.attachment;
        edge.lifecycle = LifecycleState::Current;
        edge.last_validated_generation = candidate.generation;
        edge.provenance.evidence_generation = candidate_edge.evidence_generation;
        edge.provenance.source = publication.source;
        edge.provenance.evidence_type = publication.type;
        edge.provenance.publication = publication.id;
        edge.provenance.publisher = authority.publisher;
        edge.provenance.worker_boot = authority.worker_boot;
        edge.provenance.coordinator_epoch = authority.coordinator_epoch;
        edge.provenance.last_validated_generation = candidate.generation;
        if (!candidate_edge.source_relationship_id.empty()) {
            edge.provenance.source_relationship_id = candidate_edge.source_relationship_id;
        }
        if (edge.generation.can_advance()) {
            edge.generation = edge.generation.next();
        }
        candidate.update_edge(edge);

        DiffEntry entry;
        if (previous_lifecycle != edge.lifecycle) {
            entry.kind = DiffKind::CurrentnessChanged;
            entry.before = to_string(previous_lifecycle);
            entry.after = to_string(edge.lifecycle);
        } else {
            entry.kind = DiffKind::RelationshipChanged;
            entry.before = previous_evidence.to_string();
            entry.after = edge.evidence_generation.to_string();
        }
        entry.key = edge.id.to_string();
        entry.edge = edge.id;
        diff.push_back(std::move(entry));
        ++counts.edges_updated;
    }

    // ---- explicit relationship absences ---------------------------------------------
    for (const RelationshipId& relationship_id : absences) {
        if (relationship_id.empty()) {
            continue;
        }
        const TopologyEdgeId* edge_id = candidate.find_edge_by_relationship(relationship_id);
        if (edge_id == nullptr) {
            continue;
        }
        const TopologyEdge* found = candidate.find_edge(*edge_id);
        if (found == nullptr || found->domain != publication.domain) {
            continue;
        }
        if (found->lifecycle == LifecycleState::Retired ||
            found->lifecycle == LifecycleState::Superseded) {
            continue;
        }
        mentioned_edge_keys.insert(
            render_edge_key(make_edge_key(found->relation, found->from, found->to, found->domain)));
        TopologyEdge edge = *found;
        if (publication.mode == PublicationMode::AuthoritativeSnapshot) {
            candidate.erase_edge(edge.id);
            DiffEntry entry;
            entry.kind = DiffKind::EdgeRemoved;
            entry.key = edge.id.to_string();
            entry.edge = edge.id;
            entry.before = render_edge(edge);
            diff.push_back(std::move(entry));
            ++counts.edges_removed;
        } else {
            const LifecycleState previous = edge.lifecycle;
            edge.lifecycle = LifecycleState::Retired;
            if (edge.generation.can_advance()) {
                edge.generation = edge.generation.next();
            }
            candidate.update_edge(edge);
            DiffEntry entry;
            entry.kind = DiffKind::CurrentnessChanged;
            entry.key = edge.id.to_string();
            entry.edge = edge.id;
            entry.before = to_string(previous);
            entry.after = to_string(LifecycleState::Retired);
            diff.push_back(std::move(entry));
            ++counts.edges_removed;
        }
    }

    // ---- transactional replacement semantics for an authoritative snapshot ----------
    if (publication.mode == PublicationMode::AuthoritativeSnapshot) {
        RemovalPlan plan;
        for (const auto& entry : candidate.edges) {
            const TopologyEdge& edge = entry.second;
            if (edge.domain != publication.domain) {
                continue;
            }
            if (edge.lifecycle == LifecycleState::Retired ||
                edge.lifecycle == LifecycleState::Superseded) {
                continue;
            }
            const std::string key_text =
                render_edge_key(make_edge_key(edge.relation, edge.from, edge.to, edge.domain));
            if (mentioned_edge_keys.find(key_text) != mentioned_edge_keys.end()) {
                continue;
            }
            plan.edges.push_back(edge.id);
        }
        std::sort(plan.edges.begin(), plan.edges.end());
        for (const TopologyEdgeId& edge_id : plan.edges) {
            const TopologyEdge* found = candidate.find_edge(edge_id);
            if (found == nullptr) {
                continue;
            }
            const TopologyEdge removed = *found;
            candidate.erase_edge(edge_id);
            DiffEntry entry;
            entry.kind = DiffKind::EdgeRemoved;
            entry.key = edge_id.to_string();
            entry.edge = edge_id;
            entry.before = render_edge(removed);
            diff.push_back(std::move(entry));
            ++counts.edges_removed;
        }

        for (const auto& entry : candidate.nodes) {
            const TopologyNode& node = entry.second;
            if (node.domain != publication.domain) {
                continue;
            }
            if (mentioned_node_ids.find(node.id.value()) != mentioned_node_ids.end()) {
                continue;
            }
            plan.nodes.push_back(node.id);
        }
        std::sort(plan.nodes.begin(), plan.nodes.end());
        for (const TopologyNodeId& node_id : plan.nodes) {
            const TopologyNode* found = candidate.find_node(node_id);
            if (found == nullptr) {
                continue;
            }
            const auto incident_it = candidate.incident.find(node_id);
            const bool has_edges =
                incident_it != candidate.incident.end() && !incident_it->second.empty();
            if (has_edges) {
                // The node still carries relationships that belong to another scope. It is
                // retired rather than erased, so no authoritative edge is ever orphaned.
                TopologyNode node = *found;
                const LifecycleState previous = node.lifecycle;
                node.lifecycle = LifecycleState::Retired;
                if (node.generation.can_advance()) {
                    node.generation = node.generation.next();
                }
                candidate.update_node(node);
                DiffEntry entry;
                entry.kind = DiffKind::CurrentnessChanged;
                entry.key = node_id.to_string();
                entry.node = node_id;
                entry.before = to_string(previous);
                entry.after = to_string(LifecycleState::Retired);
                diff.push_back(std::move(entry));
            } else {
                const TopologyNode removed = *found;
                candidate.erase_node(node_id);
                DiffEntry entry;
                entry.kind = DiffKind::NodeRemoved;
                entry.key = node_id.to_string();
                entry.node = node_id;
                entry.before = render_node(removed);
                diff.push_back(std::move(entry));
            }
            ++counts.nodes_removed;
        }
    }

    const bool changed = !diff.empty();
    if (!changed) {
        impl_->attempts.record(publication.id.value());
        impl_->note_idempotent();
        ++publisher->committed_mutations;
        result.status = make_status(Outcome::Idempotent, "publication.no_change");
        result.generation = impl_->graph.generation;
        result.nodes_unchanged = counts.nodes_unchanged + publication.nodes.size();
        result.edges_unchanged = counts.edges_unchanged + publication.edges.size();
        result.nodes_added = counts.nodes_added;
        result.edges_added = counts.edges_added;
        return result;
    }

    if (!candidate.generation.can_advance()) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        ++impl_->publications_rejected;
        result.status = make_status(Outcome::ResourceLimit, "generation.exhausted");
        result.generation = impl_->graph.generation;
        return result;
    }
    candidate.generation = candidate.generation.next();

    // ---- complete graph invariant validation before anything commits ----------------
    const ValidationReport candidate_report =
        internal::validate_graph(candidate, impl_->directory.get(), SnapshotScope::all());
    if (!candidate_report.valid) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        ++impl_->publications_rejected;
        Status status = make_status(Outcome::StructuralInvariantViolation,
                                    "publication.invariant_violation");
        status.with("issues", std::to_string(candidate_report.issues.size()));
        if (!candidate_report.issues.empty()) {
            const ValidationIssue& first = candidate_report.issues.front();
            status.with("first_issue", first.code);
            if (!first.detail.empty()) {
                status.with("first_detail", first.detail);
            }
        }
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    GraphState previous = std::move(impl_->graph);
    impl_->graph = std::move(candidate);
    ++impl_->generation_advances;

    const Status persisted = impl_->persist_locked();
    if (persisted.outcome() != Outcome::Ok) {
        impl_->graph = std::move(previous);
        --impl_->generation_advances;
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        ++impl_->publications_rejected;
        result.status = persisted;
        result.generation = impl_->graph.generation;
        return result;
    }

    impl_->attempts.record(publication.id.value());
    ++publisher->committed_mutations;
    impl_->note_committed();
    ++impl_->publications_committed;

    std::sort(diff.begin(), diff.end(), diff_entry_less);
    result.status.set_outcome(Outcome::Committed).set_code("publication.committed");
    result.generation = impl_->graph.generation;
    result.generation_advanced = true;
    result.nodes_added = counts.nodes_added;
    result.nodes_updated = counts.nodes_updated;
    result.nodes_removed = counts.nodes_removed;
    result.nodes_unchanged = counts.nodes_unchanged;
    result.edges_added = counts.edges_added;
    result.edges_updated = counts.edges_updated;
    result.edges_removed = counts.edges_removed;
    result.edges_superseded = counts.edges_superseded;
    result.edges_unchanged = counts.edges_unchanged;
    result.edges_conflicted = counts.edges_conflicted;
    result.edges_rejected = counts.edges_rejected;
    result.diff.entries = std::move(diff);
    result.diff.to_generation = result.generation;
    result.diff.from_generation = TopologyGeneration{result.generation.value() - 1};
    result.diff.digest = sha256_hex(result.diff.render());
    if (stale_entries != 0) {
        result.status.with("stale_entries", std::to_string(stale_entries));
        if (!stale_detail.empty()) {
            result.status.with("stale_example", stale_detail);
        }
    }
    if (impl_->options.verify_indexes_on_mutation) {
        const Status indexes = impl_->graph.verify_indexes();
        if (indexes.outcome() != Outcome::Ok) {
            result.status = indexes;
        }
    }
    return result;
}

bool PublicationResult::committed() const noexcept { return outcome_is_success(status.outcome()); }

std::string PublicationResult::render() const {
    std::string out = status.render();
    out += "\ngeneration=";
    out += generation.to_string();
    out += " nodes_added=";
    out += std::to_string(nodes_added);
    out += " nodes_updated=";
    out += std::to_string(nodes_updated);
    out += " nodes_removed=";
    out += std::to_string(nodes_removed);
    out += " nodes_unchanged=";
    out += std::to_string(nodes_unchanged);
    out += " edges_added=";
    out += std::to_string(edges_added);
    out += " edges_updated=";
    out += std::to_string(edges_updated);
    out += " edges_removed=";
    out += std::to_string(edges_removed);
    out += " edges_superseded=";
    out += std::to_string(edges_superseded);
    out += " edges_unchanged=";
    out += std::to_string(edges_unchanged);
    out += " edges_conflicted=";
    out += std::to_string(edges_conflicted);
    out += " edges_rejected=";
    out += std::to_string(edges_rejected);
    out += " diff_entries=";
    out += std::to_string(diff.entries.size());
    out += " generation_advanced=";
    out += generation_advanced ? "true" : "false";
    return out;
}

}  // namespace fabric_topology
