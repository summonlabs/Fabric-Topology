// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Relationship mutations: add, evidence update, revalidate, supersede, retire, attachment
// move, endpoint generation replacement and attachment convenience operations.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine_impl.hpp"
#include "fabric_topology/digest.hpp"
#include "fabric_topology/topology.hpp"
#include "graph_state.hpp"

namespace fabric_topology {

namespace {

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] Status finalize_mutation(TopologyEngine::Impl& impl, internal::StateTxn& txn) {
    txn.note_generation(impl.graph.generation);
    if (!impl.graph.generation.can_advance()) {
        txn.rollback();
        return make_status(Outcome::ResourceLimit, "generation.exhausted");
    }
    impl.graph.generation = impl.graph.generation.next();
    ++impl.generation_advances;
    const Status persisted = impl.persist_locked();
    if (persisted.outcome() != Outcome::Ok) {
        txn.rollback();
        return persisted;
    }
    txn.commit();
    return make_status(Outcome::Ok, "mutation.ok");
}

[[nodiscard]] DiffEntry make_diff(DiffKind kind, const std::string& key, const TopologyEdge& edge,
                                  const std::string& before, const std::string& after) {
    DiffEntry entry;
    entry.kind = kind;
    entry.key = key;
    entry.edge = edge.id;
    entry.before = before;
    entry.after = after;
    return entry;
}

[[nodiscard]] bool endpoints_match(const TopologyEdge& edge, const TopologyNode& from,
                                   const TopologyNode& to) {
    return edge.from_entity_generation == from.entity_generation &&
           edge.to_entity_generation == to.entity_generation;
}

void finish_diff(MutationResult& result, std::vector<DiffEntry> entries) {
    std::sort(entries.begin(), entries.end(), [](const DiffEntry& a, const DiffEntry& b) {
        const auto left = static_cast<std::uint8_t>(a.kind);
        const auto right = static_cast<std::uint8_t>(b.kind);
        if (left != right) {
            return left < right;
        }
        return a.key < b.key;
    });
    result.diff.entries = std::move(entries);
    result.diff.from_generation = TopologyGeneration{result.generation.value() - 1};
    result.diff.to_generation = result.generation;
    result.diff.digest = sha256_hex(result.diff.render());
}

}  // namespace

// ---------------------------------------------------------------------------
// MutationResult
// ---------------------------------------------------------------------------

bool MutationResult::accepted() const noexcept { return outcome_is_success(status.outcome()); }

bool MutationResult::changed() const noexcept { return status.outcome() == Outcome::Committed; }

std::string MutationResult::render() const {
    std::string out = status.render();
    out += "\ngeneration=";
    out += generation.to_string();
    if (node.has_value()) {
        out += " node=";
        out += node->to_string();
    }
    if (edge.has_value()) {
        out += " edge=";
        out += edge->to_string();
    }
    out += " diff_entries=";
    out += std::to_string(diff.entries.size());
    return out;
}

// ---------------------------------------------------------------------------
// add_relationship
// ---------------------------------------------------------------------------

MutationResult TopologyEngine::add_relationship(const AuthorityContext& authority,
                                                const AddRelationshipRequest& request) {
    MutationResult result;
    if (request.relation == RelationClass::Unknown) {
        result.status = make_status(Outcome::MalformedRequest, "relationship.relation_unknown");
        return result;
    }
    if (request.from.empty() || request.to.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "relationship.endpoint_missing");
        return result;
    }
    const Status metadata_status = Metadata::validate(request.metadata.items(), impl_->limits);
    if (metadata_status.outcome() != Outcome::Ok) {
        result.status = metadata_status;
        return result;
    }

    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    if (impl_->graph.find_node(request.from) == nullptr ||
        impl_->graph.find_node(request.to) == nullptr) {
        impl_->note_rejected();
        Status status = make_status(Outcome::UnknownEntity, "relationship.endpoint_unknown");
        status.with("from", request.from.to_string());
        status.with("to", request.to.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    const std::optional<TopologyLayer> layer = internal::resolve_layer(request.relation, request.layer);
    if (!layer.has_value()) {
        impl_->note_rejected();
        Status status = make_status(Outcome::MalformedRequest, "relationship.layer_required");
        status.with("relation", to_string(request.relation));
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    // Canonical direction: an undirected relationship has exactly one stored orientation.
    TopologyNodeId from = request.from;
    TopologyNodeId to = request.to;
    internal::normalize_undirected_endpoints(request.relation, from, to);

    const TopologyNode* from_node = impl_->graph.find_node(from);
    const TopologyNode* to_node = impl_->graph.find_node(to);
    if (from_node == nullptr || to_node == nullptr) {
        impl_->note_rejected();
        result.status = make_status(Outcome::UnknownEntity, "relationship.endpoint_unknown");
        result.generation = impl_->graph.generation;
        return result;
    }

    const internal::DomainResolution resolution = internal::resolve_relationship_domain(
        impl_->graph, from_node->domain, to_node->domain, request.domain, request.relation,
        request.allow_cross_domain);
    if (!resolution.ok) {
        impl_->note_rejected();
        result.status = resolution.status;
        result.generation = impl_->graph.generation;
        return result;
    }

    PublisherState* publisher = nullptr;
    Status authority_status = impl_->check_authority_locked(authority, resolution.domain,
                                                            GrantMode::IncrementalWrite, &publisher);
    if (authority_status.outcome() == Outcome::Ok && resolution.cross_domain) {
        authority_status = impl_->check_authority_locked(authority, resolution.secondary_domain,
                                                         GrantMode::IncrementalWrite, &publisher);
    }
    if (authority_status.outcome() != Outcome::Ok) {
        impl_->note_rejected();
        if (publisher != nullptr) {
            ++publisher->rejected_mutations;
        }
        result.status = authority_status;
        result.generation = impl_->graph.generation;
        return result;
    }

    if (!request.expected_generation.is_zero() && request.expected_generation != impl_->graph.generation) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "relationship.stale_generation");
        status.with("expected", request.expected_generation.to_string());
        status.with("current", impl_->graph.generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    const EdgeKey key = make_edge_key(request.relation, from, to, resolution.domain);
    internal::RelationshipCandidate candidate;
    candidate.relationship_id = request.relationship_id.empty()
                                    ? RelationshipId::from_trusted(internal::derive_relationship_id(key))
                                    : request.relationship_id;
    candidate.relation = request.relation;
    candidate.from = from;
    candidate.to = to;
    candidate.layer = *layer;
    candidate.domain = resolution.domain;
    candidate.cross_domain = resolution.cross_domain;
    candidate.secondary_domain = resolution.secondary_domain;
    candidate.evidence_generation = request.evidence_generation;
    candidate.supported_by = request.supported_by;
    candidate.attachment = request.attachment;
    candidate.exclusive_attachment = request.exclusive_attachment;
    candidate.metadata = request.metadata;
    candidate.evidence_type = PublicationType::Real;
    candidate.source = DiscoverySource::OperatorDeclaration;
    candidate.id = request.edge_id.has_value() ? *request.edge_id : internal::derive_edge_id(key);

    if (candidate.id.empty() || candidate.relationship_id.empty()) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = make_status(Outcome::MalformedRequest, "relationship.id_invalid");
        result.generation = impl_->graph.generation;
        return result;
    }

    const TopologyEdgeId* by_key = impl_->graph.find_edge_by_key(key);
    const TopologyEdge* by_id = impl_->graph.find_edge(candidate.id);

    // Two distinct edge identifiers may never describe the same semantic relationship.
    if (by_key != nullptr && *by_key != candidate.id) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::DuplicateEdge, "relationship.duplicate_edge");
        status.with("existing", by_key->to_string());
        status.with("requested", candidate.id.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    // An existing edge identifier may never be reused for a different relationship.
    if (by_id != nullptr &&
        make_edge_key(by_id->relation, by_id->from, by_id->to, by_id->domain) != key) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::DuplicateIdentity, "relationship.edge_id_reused");
        status.with("edge_id", candidate.id.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (const TopologyEdgeId* by_relationship =
            impl_->graph.find_edge_by_relationship(candidate.relationship_id);
        by_relationship != nullptr && *by_relationship != candidate.id) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::DuplicateIdentity, "relationship.id_reused");
        status.with("relationship_id", candidate.relationship_id.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    const bool is_new = by_key == nullptr;
    if (!is_new) {
        const TopologyEdge& existing = *impl_->graph.find_edge(*by_key);
        if (existing.lifecycle == LifecycleState::Retired ||
            existing.lifecycle == LifecycleState::Superseded) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::Retired, "relationship.retired_cannot_revive");
            status.with("edge_id", existing.id.to_string());
            status.with("lifecycle", to_string(existing.lifecycle));
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
        const bool unchanged = existing.layer == candidate.layer &&
                               existing.evidence_generation == candidate.evidence_generation &&
                               existing.lifecycle == LifecycleState::Current &&
                               existing.metadata.items() == candidate.metadata.items() &&
                               existing.supported_by == candidate.supported_by &&
                               existing.attachment == candidate.attachment &&
                               endpoints_match(existing, *from_node, *to_node);
        if (unchanged) {
            impl_->note_idempotent();
            ++publisher->committed_mutations;
            result.status = make_status(Outcome::Idempotent, "relationship.unchanged");
            result.edge = existing.id;
            result.generation = impl_->graph.generation;
            return result;
        }
    }

    const internal::RelationshipValidation validation =
        internal::validate_relationship_candidate(impl_->graph, candidate, is_new);
    if (!validation.ok) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = validation.status;
        result.generation = impl_->graph.generation;
        return result;
    }

    internal::StateTxn txn(impl_->graph);
    std::vector<DiffEntry> diff;

    TopologyEdge edge;
    if (!is_new) {
        edge = *impl_->graph.find_edge(*by_key);
        txn.note_edge_updated(edge);
        const std::string before = render_edge(edge);
        edge.relationship_id = candidate.relationship_id;
        edge.layer = candidate.layer;
        edge.evidence_generation = candidate.evidence_generation;
        edge.metadata = candidate.metadata;
        edge.supported_by = candidate.supported_by;
        edge.attachment = candidate.attachment;
        edge.lifecycle = LifecycleState::Current;
        edge.from_entity_generation = from_node->entity_generation;
        edge.to_entity_generation = to_node->entity_generation;
        edge.last_validated_generation = impl_->graph.generation;
        edge.provenance.evidence_generation = candidate.evidence_generation;
        edge.provenance.last_validated_generation = impl_->graph.generation;
        if (edge.generation.can_advance()) {
            edge.generation = edge.generation.next();
        }
        impl_->graph.update_edge(edge);
        diff.push_back(make_diff(DiffKind::RelationshipChanged, edge.id.to_string(), edge, before,
                                 render_edge(edge)));
    } else {
        edge.id = candidate.id;
        edge.relationship_id = candidate.relationship_id;
        edge.relation = candidate.relation;
        edge.from = candidate.from;
        edge.to = candidate.to;
        edge.layer = candidate.layer;
        edge.domain = candidate.domain;
        edge.cross_domain = candidate.cross_domain;
        edge.secondary_domain = candidate.cross_domain ? candidate.secondary_domain
                                                       : candidate.domain;
        edge.generation = EdgeGeneration{1};
        edge.created_generation = impl_->graph.generation;
        edge.last_validated_generation = impl_->graph.generation;
        edge.evidence_generation = candidate.evidence_generation;
        edge.from_entity_generation = from_node->entity_generation;
        edge.to_entity_generation = to_node->entity_generation;
        edge.lifecycle = LifecycleState::Current;
        edge.supported_by = candidate.supported_by;
        edge.attachment = candidate.attachment;
        edge.provenance.publisher = authority.publisher;
        edge.provenance.worker_boot = authority.worker_boot;
        edge.provenance.coordinator_epoch = authority.coordinator_epoch;
        edge.provenance.source = candidate.source;
        edge.provenance.evidence_type = candidate.evidence_type;
        edge.provenance.evidence_generation = candidate.evidence_generation;
        edge.provenance.source_relationship_id = candidate.source_relationship_id;
        edge.provenance.publication = authority.publication;
        edge.provenance.created_generation = impl_->graph.generation;
        edge.provenance.last_validated_generation = impl_->graph.generation;
        edge.metadata = candidate.metadata;

        txn.note_edge_inserted(edge.id);
        if (!impl_->graph.insert_edge(edge)) {
            txn.rollback();
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            result.status = make_status(Outcome::InternalError, "relationship.insert_failed");
            result.generation = impl_->graph.generation;
            return result;
        }
        diff.push_back(make_diff(DiffKind::EdgeAdded, edge.id.to_string(), edge, std::string(),
                                 render_edge(edge)));
    }

    const Status finalized = finalize_mutation(*impl_, txn);
    if (finalized.outcome() != Outcome::Ok) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = finalized;
        result.generation = impl_->graph.generation;
        return result;
    }

    if (impl_->options.verify_indexes_on_mutation) {
        const Status indexes = impl_->graph.verify_indexes();
        if (indexes.outcome() != Outcome::Ok) {
            result.status = indexes;
            return result;
        }
    }

    ++publisher->committed_mutations;
    impl_->note_committed();
    result.status = make_status(Outcome::Committed, is_new ? "relationship.added" : "relationship.updated");
    result.edge = edge.id;
    result.generation = impl_->graph.generation;
    finish_diff(result, std::move(diff));
    return result;
}

// ---------------------------------------------------------------------------
// Evidence update / revalidate
// ---------------------------------------------------------------------------

namespace {

MutationResult apply_evidence_change(TopologyEngine::Impl& impl, const AuthorityContext& authority,
                                     const TopologyEdgeId& edge_id, EvidenceGeneration evidence,
                                     LifecycleState lifecycle, TopologyGeneration expected_generation,
                                     EdgeGeneration expected_edge_generation, bool replace_metadata,
                                     const Metadata& metadata, std::string_view reason,
                                     bool require_endpoint_binding) {
    MutationResult result;
    std::unique_lock authority_lock(impl.authority_mutex);
    std::unique_lock state_lock(impl.state_mutex);

    const TopologyEdge* found = impl.graph.find_edge(edge_id);
    if (found == nullptr) {
        impl.note_rejected();
        Status status = make_status(Outcome::NotFound, "relationship.not_found");
        status.with("edge_id", edge_id.to_string());
        result.status = status;
        result.generation = impl.graph.generation;
        return result;
    }

    PublisherState* publisher = nullptr;
    Status authority_status =
        impl.check_authority_locked(authority, found->domain, GrantMode::IncrementalWrite, &publisher);
    if (authority_status.outcome() != Outcome::Ok) {
        impl.note_rejected();
        result.status = authority_status;
        result.generation = impl.graph.generation;
        return result;
    }
    if (!expected_generation.is_zero() && expected_generation != impl.graph.generation) {
        ++publisher->rejected_mutations;
        impl.note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "relationship.stale_generation");
        status.with("expected", expected_generation.to_string());
        status.with("current", impl.graph.generation.to_string());
        result.status = status;
        result.generation = impl.graph.generation;
        return result;
    }
    if (!expected_edge_generation.is_zero() && expected_edge_generation != found->generation) {
        ++publisher->rejected_mutations;
        impl.note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "relationship.stale_edge_generation");
        status.with("expected_edge_generation", expected_edge_generation.to_string());
        status.with("current_edge_generation", found->generation.to_string());
        result.status = status;
        result.generation = impl.graph.generation;
        return result;
    }
    if (found->lifecycle == LifecycleState::Retired) {
        ++publisher->rejected_mutations;
        impl.note_rejected();
        Status status = make_status(Outcome::Retired, "relationship.retired_cannot_revive");
        status.with("edge_id", edge_id.to_string());
        result.status = status;
        result.generation = impl.graph.generation;
        return result;
    }
    if (found->lifecycle == LifecycleState::Superseded) {
        ++publisher->rejected_mutations;
        impl.note_rejected();
        Status status = make_status(Outcome::Superseded, "relationship.superseded_cannot_revive");
        status.with("edge_id", edge_id.to_string());
        result.status = status;
        result.generation = impl.graph.generation;
        return result;
    }

    if (require_endpoint_binding && lifecycle == LifecycleState::Current) {
        const TopologyNode* from_node = impl.graph.find_node(found->from);
        const TopologyNode* to_node = impl.graph.find_node(found->to);
        // The registry may have superseded an endpoint identity while the node still carries
        // the older binding. Such a relationship can never be made current again from stale
        // endpoint generations.
        if (impl.directory != nullptr && from_node != nullptr && to_node != nullptr) {
            const auto from_record = impl.directory->lookup(from_node->entity_id);
            const auto to_record = impl.directory->lookup(to_node->entity_id);
            const bool from_stale = !from_record.has_value() || from_record->retired ||
                                    from_record->superseded ||
                                    from_record->generation != from_node->entity_generation;
            const bool to_stale = !to_record.has_value() || to_record->retired ||
                                  to_record->superseded ||
                                  to_record->generation != to_node->entity_generation;
            if (from_stale || to_stale) {
                ++publisher->rejected_mutations;
                impl.note_rejected();
                Status status = make_status(Outcome::StaleEntityGeneration,
                                            "relationship.registry_generation_advanced");
                status.with("edge_id", edge_id.to_string());
                if (from_stale) {
                    status.with("from_entity", from_node->entity_id);
                }
                if (to_stale) {
                    status.with("to_entity", to_node->entity_id);
                }
                result.status = status;
                result.generation = impl.graph.generation;
                return result;
            }
        }
        if (from_node == nullptr || to_node == nullptr) {
            ++publisher->rejected_mutations;
            impl.note_rejected();
            Status status = make_status(Outcome::StructuralInvariantViolation, "relationship.endpoint_missing");
            status.with("edge_id", edge_id.to_string());
            result.status = status;
            result.generation = impl.graph.generation;
            return result;
        }
        if (!endpoints_match(*found, *from_node, *to_node)) {
            ++publisher->rejected_mutations;
            impl.note_rejected();
            Status status = make_status(Outcome::StaleEntityGeneration, "relationship.endpoint_generation_changed");
            status.with("edge_id", edge_id.to_string());
            status.with("edge_from_entity_generation", found->from_entity_generation.to_string());
            status.with("node_entity_generation", from_node->entity_generation.to_string());
            result.status = status;
            result.generation = impl.graph.generation;
            return result;
        }
    }

    const bool unchanged = found->evidence_generation == evidence && found->lifecycle == lifecycle &&
                           (!replace_metadata || found->metadata.items() == metadata.items());
    if (unchanged) {
        impl.note_idempotent();
        ++publisher->committed_mutations;
        result.status = make_status(Outcome::Idempotent, "relationship.evidence_unchanged");
        result.edge = edge_id;
        result.generation = impl.graph.generation;
        return result;
    }

    internal::StateTxn txn(impl.graph);
    TopologyEdge edge = *found;
    const LifecycleState previous_lifecycle = edge.lifecycle;
    txn.note_edge_updated(edge);
    edge.evidence_generation = evidence;
    edge.lifecycle = lifecycle;
    if (replace_metadata) {
        edge.metadata = metadata;
    }
    edge.provenance.evidence_generation = evidence;
    edge.provenance.last_validated_generation = impl.graph.generation;
    edge.last_validated_generation = impl.graph.generation;
    if (lifecycle == LifecycleState::Current) {
        edge.provenance.publisher = authority.publisher;
        edge.provenance.worker_boot = authority.worker_boot;
        edge.provenance.coordinator_epoch = authority.coordinator_epoch;
    }
    if (edge.generation.can_advance()) {
        edge.generation = edge.generation.next();
    }
    impl.graph.update_edge(edge);

    const Status finalized = finalize_mutation(impl, txn);
    if (finalized.outcome() != Outcome::Ok) {
        ++publisher->rejected_mutations;
        impl.note_rejected();
        result.status = finalized;
        result.generation = impl.graph.generation;
        return result;
    }

    ++publisher->committed_mutations;
    impl.note_committed();
    result.status = make_status(Outcome::Committed, "relationship.evidence_updated");
    result.edge = edge_id;
    result.generation = impl.graph.generation;

    std::vector<DiffEntry> diff;
    if (previous_lifecycle != lifecycle) {
        DiffEntry entry;
        entry.kind = DiffKind::CurrentnessChanged;
        entry.key = edge_id.to_string();
        entry.edge = edge_id;
        entry.before = to_string(previous_lifecycle);
        entry.after = to_string(lifecycle);
        diff.push_back(std::move(entry));
    } else {
        diff.push_back(make_diff(DiffKind::RelationshipChanged, edge_id.to_string(), edge,
                                 std::string(), render_edge(edge)));
    }
    if (!reason.empty()) {
        result.status.with("reason", std::string(reason));
    }
    finish_diff(result, std::move(diff));
    return result;
}

}  // namespace

MutationResult TopologyEngine::update_relationship_evidence(const AuthorityContext& authority,
                                                            const UpdateEvidenceRequest& request) {
    const Status metadata_status = Metadata::validate(request.metadata.items(), impl_->limits);
    if (metadata_status.outcome() != Outcome::Ok) {
        MutationResult result;
        result.status = metadata_status;
        result.generation = generation();
        return result;
    }
    if (request.lifecycle == LifecycleState::Retired || request.lifecycle == LifecycleState::Superseded) {
        MutationResult result;
        result.status = make_status(Outcome::MalformedRequest, "relationship.use_dedicated_operation");
        result.generation = generation();
        return result;
    }
    MutationResult result = apply_evidence_change(
        *impl_, authority, request.edge, request.evidence_generation, request.lifecycle,
        request.expected_generation, request.expected_edge_generation, request.replace_metadata,
        request.metadata, request.reason, request.lifecycle == LifecycleState::Current);
    result.status.with("operation", "update_relationship_evidence");
    return result;
}

MutationResult TopologyEngine::revalidate_relationship(const AuthorityContext& authority,
                                                       const RevalidateRequest& request) {
    MutationResult result = apply_evidence_change(*impl_, authority, request.edge,
                                                  request.evidence_generation,
                                                  LifecycleState::Current, request.expected_generation,
                                                  EdgeGeneration{}, false, Metadata{}, request.reason, true);
    result.status.with("operation", "revalidate_relationship");
    if (result.accepted()) {
        result.status.with("revalidated", "true");
    }
    return result;
}

// ---------------------------------------------------------------------------
// Supersede / retire
// ---------------------------------------------------------------------------

MutationResult TopologyEngine::supersede_relationship(const AuthorityContext& authority,
                                                      const SupersedeRequest& request) {
    MutationResult result;
    if (request.edge.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "relationship.edge_missing");
        return result;
    }
    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    const TopologyEdge* found = impl_->graph.find_edge(request.edge);
    if (found == nullptr) {
        impl_->note_rejected();
        Status status = make_status(Outcome::NotFound, "relationship.not_found");
        status.with("edge_id", request.edge.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    PublisherState* publisher = nullptr;
    Status authority_status = impl_->check_authority_locked(authority, found->domain,
                                                            GrantMode::IncrementalWrite, &publisher);
    if (authority_status.outcome() != Outcome::Ok) {
        impl_->note_rejected();
        result.status = authority_status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (!request.expected_generation.is_zero() && request.expected_generation != impl_->graph.generation) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "relationship.stale_generation");
        status.with("expected", request.expected_generation.to_string());
        status.with("current", impl_->graph.generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (found->lifecycle == LifecycleState::Superseded && found->superseded_by == request.replacement) {
        impl_->note_idempotent();
        ++publisher->committed_mutations;
        result.status = make_status(Outcome::Idempotent, "relationship.already_superseded");
        result.edge = request.edge;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (found->lifecycle == LifecycleState::Retired) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = make_status(Outcome::Retired, "relationship.retired");
        result.generation = impl_->graph.generation;
        return result;
    }
    if (request.replacement.has_value()) {
        const TopologyEdge* replacement = impl_->graph.find_edge(*request.replacement);
        if (replacement == nullptr) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::NotFound, "relationship.replacement_not_found");
            status.with("replacement", request.replacement->to_string());
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
        if (*request.replacement == request.edge) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            result.status = make_status(Outcome::CycleRejected, "relationship.self_supersede");
            result.generation = impl_->graph.generation;
            return result;
        }
        if (replacement->lifecycle != LifecycleState::Current) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::RelationshipConflict, "relationship.replacement_not_current");
            status.with("replacement_lifecycle", to_string(replacement->lifecycle));
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
    }

    internal::StateTxn txn(impl_->graph);
    TopologyEdge edge = *found;
    txn.note_edge_updated(edge);
    edge.lifecycle = LifecycleState::Superseded;
    edge.superseded_by = request.replacement;
    if (edge.generation.can_advance()) {
        edge.generation = edge.generation.next();
    }
    impl_->graph.update_edge(edge);

    const Status finalized = finalize_mutation(*impl_, txn);
    if (finalized.outcome() != Outcome::Ok) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = finalized;
        result.generation = impl_->graph.generation;
        return result;
    }
    ++publisher->committed_mutations;
    impl_->note_committed();
    result.status = make_status(Outcome::Committed, "relationship.superseded");
    if (!request.reason.empty()) {
        result.status.with("reason", request.reason);
    }
    result.edge = request.edge;
    result.generation = impl_->graph.generation;
    std::vector<DiffEntry> diff;
    diff.push_back(make_diff(DiffKind::EdgeSuperseded, request.edge.to_string(), edge,
                             to_string(found->lifecycle), to_string(edge.lifecycle)));
    finish_diff(result, std::move(diff));
    return result;
}

MutationResult TopologyEngine::retire_relationship(const AuthorityContext& authority,
                                                   const RetireRequest& request) {
    MutationResult result;
    if (request.edge.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "relationship.edge_missing");
        return result;
    }
    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    const TopologyEdge* found = impl_->graph.find_edge(request.edge);
    if (found == nullptr) {
        impl_->note_rejected();
        Status status = make_status(Outcome::NotFound, "relationship.not_found");
        status.with("edge_id", request.edge.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    PublisherState* publisher = nullptr;
    Status authority_status = impl_->check_authority_locked(authority, found->domain,
                                                            GrantMode::IncrementalWrite, &publisher);
    if (authority_status.outcome() != Outcome::Ok) {
        impl_->note_rejected();
        result.status = authority_status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (!request.expected_generation.is_zero() && request.expected_generation != impl_->graph.generation) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "relationship.stale_generation");
        status.with("expected", request.expected_generation.to_string());
        status.with("current", impl_->graph.generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (found->lifecycle == LifecycleState::Retired) {
        impl_->note_idempotent();
        ++publisher->committed_mutations;
        result.status = make_status(Outcome::Idempotent, "relationship.already_retired");
        result.edge = request.edge;
        result.generation = impl_->graph.generation;
        return result;
    }

    internal::StateTxn txn(impl_->graph);
    TopologyEdge edge = *found;
    const LifecycleState previous = edge.lifecycle;
    txn.note_edge_updated(edge);
    edge.lifecycle = LifecycleState::Retired;
    if (edge.generation.can_advance()) {
        edge.generation = edge.generation.next();
    }
    impl_->graph.update_edge(edge);

    const Status finalized = finalize_mutation(*impl_, txn);
    if (finalized.outcome() != Outcome::Ok) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = finalized;
        result.generation = impl_->graph.generation;
        return result;
    }
    ++publisher->committed_mutations;
    impl_->note_committed();
    result.status = make_status(Outcome::Committed, "relationship.retired");
    if (!request.reason.empty()) {
        result.status.with("reason", request.reason);
    }
    result.edge = request.edge;
    result.generation = impl_->graph.generation;
    std::vector<DiffEntry> diff;
    diff.push_back(make_diff(DiffKind::CurrentnessChanged, request.edge.to_string(), edge,
                             to_string(previous), to_string(LifecycleState::Retired)));
    finish_diff(result, std::move(diff));
    return result;
}

// ---------------------------------------------------------------------------
// Attachment move
// ---------------------------------------------------------------------------

MutationResult TopologyEngine::move_attachment(const AuthorityContext& authority,
                                               const MoveAttachmentRequest& request) {
    MutationResult result;
    if (request.attachment_edge.empty() || request.new_target.empty() || request.attachment.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "attachment.move_incomplete");
        return result;
    }
    const Status metadata_status = Metadata::validate(request.metadata.items(), impl_->limits);
    if (metadata_status.outcome() != Outcome::Ok) {
        result.status = metadata_status;
        return result;
    }

    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    const TopologyEdge* found = impl_->graph.find_edge(request.attachment_edge);
    if (found == nullptr) {
        impl_->note_rejected();
        Status status = make_status(Outcome::NotFound, "attachment.edge_not_found");
        status.with("edge_id", request.attachment_edge.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (found->relation != RelationClass::AttachedTo) {
        impl_->note_rejected();
        Status status = make_status(Outcome::MalformedRequest, "attachment.not_an_attachment");
        status.with("relation", to_string(found->relation));
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    PublisherState* publisher = nullptr;
    Status authority_status = impl_->check_authority_locked(authority, found->domain,
                                                            GrantMode::IncrementalWrite, &publisher);
    if (authority_status.outcome() != Outcome::Ok) {
        impl_->note_rejected();
        result.status = authority_status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (!request.expected_generation.is_zero() && request.expected_generation != impl_->graph.generation) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "attachment.stale_generation");
        status.with("expected", request.expected_generation.to_string());
        status.with("current", impl_->graph.generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (!request.expected_edge_generation.is_zero() &&
        request.expected_edge_generation != found->generation) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "attachment.stale_edge_generation");
        status.with("expected_edge_generation", request.expected_edge_generation.to_string());
        status.with("current_edge_generation", found->generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (found->lifecycle != LifecycleState::Current) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::RevalidationRequired, "attachment.source_not_current");
        status.with("lifecycle", to_string(found->lifecycle));
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    const TopologyNode* target = impl_->graph.find_node(request.new_target);
    if (target == nullptr) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::UnknownEntity, "attachment.target_unknown");
        status.with("target", request.new_target.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    const TopologyDomainId domain = request.domain.empty() ? found->domain : request.domain;
    if (domain != found->domain) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::DomainViolation, "attachment.domain_change_not_permitted");
        status.with("current_domain", found->domain.to_string());
        status.with("requested_domain", domain.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    const TopologyNode* endpoint = impl_->graph.find_node(found->from);
    if (endpoint == nullptr) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = make_status(Outcome::StructuralInvariantViolation, "attachment.endpoint_missing");
        result.generation = impl_->graph.generation;
        return result;
    }

    const EdgeKey new_key = make_edge_key(RelationClass::AttachedTo, found->from, request.new_target, domain);
    if (const TopologyEdgeId* clash = impl_->graph.find_edge_by_key(new_key); clash != nullptr) {
        if (*clash != found->id) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::DuplicateEdge, "attachment.move_target_exists");
            status.with("existing", clash->to_string());
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
    }

    internal::RelationshipCandidate candidate;
    candidate.id = request.new_edge_id.has_value()
                       ? *request.new_edge_id
                       : internal::derive_edge_id(new_key);
    candidate.relationship_id = RelationshipId::from_trusted(internal::derive_relationship_id(new_key));
    candidate.relation = RelationClass::AttachedTo;
    candidate.from = found->from;
    candidate.to = request.new_target;
    candidate.layer = TopologyLayer::Physical;
    candidate.domain = domain;
    candidate.evidence_generation = request.evidence_generation;
    candidate.attachment = request.attachment;
    candidate.exclusive_attachment = true;
    candidate.metadata = request.metadata;
    candidate.evidence_type = PublicationType::Real;
    candidate.source = DiscoverySource::OperatorDeclaration;

    internal::RelationshipValidation validation =
        internal::validate_relationship_candidate(impl_->graph, candidate, true);
    if (!validation.ok) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = validation.status;
        result.generation = impl_->graph.generation;
        return result;
    }

    internal::StateTxn txn(impl_->graph);
    std::vector<DiffEntry> diff;

    TopologyEdge retired = *found;
    txn.note_edge_updated(retired);

    TopologyEdge created;
    created.id = candidate.id;
    created.relationship_id = candidate.relationship_id;
    created.relation = candidate.relation;
    created.from = candidate.from;
    created.to = candidate.to;
    created.layer = candidate.layer;
    created.domain = candidate.domain;
    created.secondary_domain = candidate.domain;
    created.generation = EdgeGeneration{1};
    created.created_generation = impl_->graph.generation;
    created.last_validated_generation = impl_->graph.generation;
    created.evidence_generation = candidate.evidence_generation;
    created.from_entity_generation = endpoint->entity_generation;
    created.to_entity_generation = target->entity_generation;
    created.lifecycle = LifecycleState::Current;
    created.attachment = candidate.attachment;
    created.provenance.publisher = authority.publisher;
    created.provenance.worker_boot = authority.worker_boot;
    created.provenance.coordinator_epoch = authority.coordinator_epoch;
    created.provenance.source = candidate.source;
    created.provenance.evidence_type = candidate.evidence_type;
    created.provenance.evidence_generation = candidate.evidence_generation;
    created.provenance.publication = authority.publication;
    created.provenance.created_generation = impl_->graph.generation;
    created.provenance.last_validated_generation = impl_->graph.generation;
    created.metadata = candidate.metadata;

    retired.lifecycle = LifecycleState::Superseded;
    retired.superseded_by = created.id;
    if (retired.generation.can_advance()) {
        retired.generation = retired.generation.next();
    }
    impl_->graph.update_edge(retired);

    txn.note_edge_inserted(created.id);
    if (!impl_->graph.insert_edge(created)) {
        txn.rollback();
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = make_status(Outcome::InternalError, "attachment.insert_failed");
        result.generation = impl_->graph.generation;
        return result;
    }

    diff.push_back(make_diff(DiffKind::EdgeSuperseded, retired.id.to_string(), retired,
                             render_edge(*found), render_edge(retired)));
    diff.push_back(make_diff(DiffKind::AttachmentMoved, created.id.to_string(), created,
                             found->to.to_string(), created.to.to_string()));

    const Status finalized = finalize_mutation(*impl_, txn);
    if (finalized.outcome() != Outcome::Ok) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = finalized;
        result.generation = impl_->graph.generation;
        return result;
    }

    ++publisher->committed_mutations;
    impl_->note_committed();
    result.status = make_status(Outcome::Committed, "attachment.moved");
    if (!request.reason.empty()) {
        result.status.with("reason", request.reason);
    }
    result.edge = created.id;
    result.generation = impl_->graph.generation;
    finish_diff(result, std::move(diff));
    return result;
}

// ---------------------------------------------------------------------------
// Endpoint generation replacement
// ---------------------------------------------------------------------------

MutationResult TopologyEngine::replace_endpoint_generation(
    const AuthorityContext& authority, const ReplaceEndpointGenerationRequest& request) {
    MutationResult result;
    if (request.node.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "endpoint.node_missing");
        return result;
    }
    if (request.rebind_identity && request.successor_entity_id.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "endpoint.successor_missing");
        return result;
    }

    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    const TopologyNode* found = impl_->graph.find_node(request.node);
    if (found == nullptr) {
        impl_->note_rejected();
        Status status = make_status(Outcome::NotFound, "endpoint.not_found");
        status.with("node_id", request.node.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    PublisherState* publisher = nullptr;
    Status authority_status = impl_->check_authority_locked(authority, found->domain,
                                                            GrantMode::IncrementalWrite, &publisher);
    if (authority_status.outcome() != Outcome::Ok) {
        impl_->note_rejected();
        result.status = authority_status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (!request.expected_generation.is_zero() && request.expected_generation != impl_->graph.generation) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "endpoint.stale_generation");
        status.with("expected", request.expected_generation.to_string());
        status.with("current", impl_->graph.generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (!request.expected_node_generation.is_zero() &&
        request.expected_node_generation != found->generation) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "endpoint.stale_node_generation");
        status.with("expected_node_generation", request.expected_node_generation.to_string());
        status.with("current_node_generation", found->generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    TopologyNode updated = *found;
    const EntityGeneration previous_entity_generation = found->entity_generation;
    std::string successor_class_note;

    if (request.rebind_identity) {
        std::optional<EntityRecord> record;
        if (impl_->directory != nullptr) {
            record = impl_->directory->lookup(request.successor_entity_id);
        }
        if (!record.has_value()) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::UnknownEntity, "endpoint.successor_unknown");
            status.with("successor", request.successor_entity_id);
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
        if (record->retired || record->superseded) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::Retired, "endpoint.successor_not_live");
            status.with("successor", request.successor_entity_id);
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
        if (record->entity_class != node_class_entity_class(updated.node_class)) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::IncompatibleEntityClass, "endpoint.successor_class_mismatch");
            status.with("registry_class", to_string(record->entity_class));
            status.with("node_class", to_string(updated.node_class));
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
        if (const TopologyNodeId* clash = impl_->graph.find_node_by_entity(request.successor_entity_id);
            clash != nullptr && *clash != updated.id) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::DuplicateIdentity, "endpoint.successor_already_bound");
            status.with("successor", request.successor_entity_id);
            status.with("bound_node", clash->to_string());
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
        updated.entity_id = request.successor_entity_id;
        updated.entity_class = record->entity_class;
        updated.entity_generation = record->generation;
        successor_class_note = request.successor_entity_id;
    } else {
        if (!request.new_entity_generation.is_zero() &&
            request.new_entity_generation <= updated.entity_generation) {
            impl_->note_idempotent();
            ++publisher->committed_mutations;
            Status status = make_status(Outcome::Idempotent, "endpoint.generation_unchanged");
            status.with("entity_generation", updated.entity_generation.to_string());
            result.status = status;
            result.node = request.node;
            result.generation = impl_->graph.generation;
            return result;
        }
        updated.entity_generation = request.new_entity_generation;
    }

    internal::StateTxn txn(impl_->graph);
    std::vector<DiffEntry> diff;

    txn.note_node_updated(updated);
    if (updated.generation.can_advance()) {
        updated.generation = updated.generation.next();
    }
    updated.last_validated_generation = impl_->graph.generation;
    impl_->graph.update_node(updated);

    // Old relationships bound to the previous entity generation never silently inherit the
    // new one: they become revalidation-required and carry the new binding explicitly.
    const auto incident_it = impl_->graph.incident.find(request.node);
    if (incident_it != impl_->graph.incident.end()) {
        std::vector<TopologyEdgeId> affected(incident_it->second.begin(), incident_it->second.end());
        std::sort(affected.begin(), affected.end());
        for (const TopologyEdgeId& edge_id : affected) {
            const TopologyEdge* edge_found = impl_->graph.find_edge(edge_id);
            if (edge_found == nullptr) {
                continue;
            }
            TopologyEdge edge = *edge_found;
            if (edge.lifecycle == LifecycleState::Retired ||
                edge.lifecycle == LifecycleState::Superseded) {
                continue;
            }
            txn.note_edge_updated(edge);
            if (edge.from == request.node) {
                edge.from_entity_generation = updated.entity_generation;
            }
            if (edge.to == request.node) {
                edge.to_entity_generation = updated.entity_generation;
            }
            const LifecycleState previous_lifecycle = edge.lifecycle;
            edge.lifecycle = LifecycleState::RevalidationRequired;
            if (edge.generation.can_advance()) {
                edge.generation = edge.generation.next();
            }
            impl_->graph.update_edge(edge);
            diff.push_back(make_diff(DiffKind::CurrentnessChanged, edge.id.to_string(), edge,
                                     to_string(previous_lifecycle),
                                     to_string(LifecycleState::RevalidationRequired)));
        }
    }

    DiffEntry node_entry;
    node_entry.kind = DiffKind::NodeGenerationChanged;
    node_entry.key = request.node.to_string();
    node_entry.node = request.node;
    node_entry.before = previous_entity_generation.to_string();
    node_entry.after = updated.entity_generation.to_string();
    diff.push_back(std::move(node_entry));

    const Status finalized = finalize_mutation(*impl_, txn);
    if (finalized.outcome() != Outcome::Ok) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = finalized;
        result.generation = impl_->graph.generation;
        return result;
    }
    ++publisher->committed_mutations;
    impl_->note_committed();
    result.status = make_status(Outcome::Committed, "endpoint.generation_replaced");
    result.status.with("entity_generation", updated.entity_generation.to_string());
    if (!successor_class_note.empty()) {
        result.status.with("successor_entity_id", successor_class_note);
    }
    if (!request.reason.empty()) {
        result.status.with("reason", request.reason);
    }
    result.node = request.node;
    result.generation = impl_->graph.generation;
    finish_diff(result, std::move(diff));
    return result;
}

// ---------------------------------------------------------------------------
// Attachment convenience operations
// ---------------------------------------------------------------------------

MutationResult TopologyEngine::attach_endpoint(const AuthorityContext& authority,
                                               const AttachEndpointRequest& request) {
    AddRelationshipRequest add;
    add.edge_id = request.edge_id;
    add.relation = RelationClass::AttachedTo;
    add.from = request.endpoint;
    add.to = request.target;
    add.layer = TopologyLayer::Physical;
    add.domain = request.domain;
    add.expected_generation = request.expected_generation;
    add.evidence_generation = request.evidence_generation;
    add.attachment = request.attachment;
    add.exclusive_attachment = request.exclusive;
    add.metadata = request.metadata;
    MutationResult result = add_relationship(authority, add);
    result.status.with("operation", "attach_endpoint");
    return result;
}

MutationResult TopologyEngine::detach_endpoint(const AuthorityContext& authority,
                                               const DetachEndpointRequest& request) {
    MutationResult result;
    RetireRequest retire;
    retire.expected_generation = request.expected_generation;
    retire.reason = request.reason;

    TopologyEdgeId target;
    {
        std::shared_lock lock(impl_->state_mutex);
        const auto it = impl_->graph.edge_by_attachment.find(request.attachment.value());
        if (it != impl_->graph.edge_by_attachment.end()) {
            for (const TopologyEdgeId& edge_id : it->second) {
                const TopologyEdge* edge = impl_->graph.find_edge(edge_id);
                if (edge != nullptr && edge->from == request.endpoint &&
                    edge->lifecycle == LifecycleState::Current) {
                    target = edge_id;
                    break;
                }
            }
        }
    }
    if (target.empty()) {
        impl_->note_rejected();
        Status status = make_status(Outcome::NotFound, "attachment.not_found");
        status.with("endpoint", request.endpoint.to_string());
        status.with("attachment", request.attachment.to_string());
        result.status = status;
        result.generation = generation();
        return result;
    }
    retire.edge = target;
    result = retire_relationship(authority, retire);
    result.status.with("operation", "detach_endpoint");
    return result;
}

}  // namespace fabric_topology
