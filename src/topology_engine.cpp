// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/topology.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "engine_impl.hpp"
#include "graph_state.hpp"

namespace fabric_topology {

namespace {

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] bool metadata_equal(const Metadata& a, const Metadata& b) {
    return a.items() == b.items();
}

[[nodiscard]] GrantMode publisher_grant(const PublisherState& state, const TopologyDomainId& domain) {
    return state.grant_for(domain);
}

}  // namespace

// ---------------------------------------------------------------------------
// PublisherState
// ---------------------------------------------------------------------------

GrantMode PublisherState::grant_for(const TopologyDomainId& domain) const noexcept {
    GrantMode best = GrantMode::None;
    for (const ScopeGrant& grant : grants) {
        if (grant.domain == domain && grant.mode > best) {
            best = grant.mode;
        }
    }
    return best;
}

std::string PublisherState::render() const {
    std::string out;
    out.reserve(192);
    out += "publisher=";
    out += publisher.to_string();
    out += " boot=";
    out += worker_boot.to_string();
    out += " epoch=";
    out += coordinator_epoch.to_string();
    out += " fenced=";
    out += fenced ? "true" : "false";
    out += " committed=";
    out += std::to_string(committed_mutations);
    out += " rejected=";
    out += std::to_string(rejected_mutations);
    out += " grants=";
    out += std::to_string(grants.size());
    if (!reason.empty()) {
        out += " reason=";
        out += reason;
    }
    if (!fence_reason.empty()) {
        out += " fence_reason=";
        out += fence_reason;
    }
    return out;
}

const char* to_string(GrantMode value) noexcept {
    switch (value) {
        case GrantMode::None: return "NONE";
        case GrantMode::Read: return "READ";
        case GrantMode::IncrementalWrite: return "INCREMENTAL_WRITE";
        case GrantMode::AuthoritativeWrite: return "AUTHORITATIVE_WRITE";
    }
    return "NONE";
}

std::optional<GrantMode> grant_mode_from_string(std::string_view text) noexcept {
    if (text == "NONE") return GrantMode::None;
    if (text == "READ") return GrantMode::Read;
    if (text == "INCREMENTAL_WRITE") return GrantMode::IncrementalWrite;
    if (text == "AUTHORITATIVE_WRITE") return GrantMode::AuthoritativeWrite;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// AttemptLedger
// ---------------------------------------------------------------------------

namespace internal {

void AttemptLedger::record(const std::string& key) {
    if (keys_.insert(key).second) {
        order_.push_back(key);
        while (order_.size() > kCapacity) {
            keys_.erase(order_.front());
            order_.pop_front();
        }
    }
}

bool AttemptLedger::seen(const std::string& key) const { return keys_.find(key) != keys_.end(); }

}  // namespace internal

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

TopologyEngine::Impl::Impl(TopologyEngineOptions options_in)
    : options(std::move(options_in)),
      limits(options.limits),
      directory(options.directory),
      graph(options.limits),
      coordinator_epoch(options.initial_coordinator_epoch) {}

bool TopologyEngine::Impl::boot_is_fenced_locked(std::string_view boot) const {
    return fenced_boots.find(std::string(boot)) != fenced_boots.end();
}

Status TopologyEngine::Impl::check_authority_locked(const AuthorityContext& authority,
                                                    const TopologyDomainId& domain, GrantMode required,
                                                    PublisherState** out_state) {
    if (authority.publisher.empty()) {
        return make_status(Outcome::MalformedRequest, "authority.publisher_missing");
    }
    if (authority.worker_boot.empty()) {
        return make_status(Outcome::MalformedRequest, "authority.worker_boot_missing");
    }
    if (authority.coordinator_epoch != coordinator_epoch) {
        Status status = make_status(Outcome::StaleCoordinatorEpoch, "authority.epoch_stale");
        status.with("request_epoch", authority.coordinator_epoch.to_string());
        status.with("current_epoch", coordinator_epoch.to_string());
        return status;
    }
    if (boot_is_fenced_locked(authority.worker_boot.value())) {
        Status status = make_status(Outcome::StaleWorkerBoot, "authority.worker_boot_fenced");
        status.with("worker_boot", authority.worker_boot.to_string());
        return status;
    }
    const auto it = publishers.find(authority.publisher.value());
    if (it == publishers.end()) {
        Status status = make_status(Outcome::StaleAuthority, "authority.publisher_unregistered");
        status.with("publisher", authority.publisher.to_string());
        return status;
    }
    PublisherState& state = it->second;
    if (state.worker_boot != authority.worker_boot) {
        Status status = make_status(Outcome::StaleWorkerBoot, "authority.worker_boot_mismatch");
        status.with("registered_boot", state.worker_boot.to_string());
        status.with("request_boot", authority.worker_boot.to_string());
        return status;
    }
    if (state.coordinator_epoch != coordinator_epoch) {
        Status status = make_status(Outcome::StaleAuthority, "authority.registration_stale");
        status.with("registration_epoch", state.coordinator_epoch.to_string());
        status.with("current_epoch", coordinator_epoch.to_string());
        return status;
    }
    if (state.fenced) {
        Status status = make_status(Outcome::StaleAuthority, "authority.publisher_fenced");
        status.with("publisher", state.publisher.to_string());
        if (!state.fence_reason.empty()) {
            status.with("fence_reason", state.fence_reason);
        }
        return status;
    }
    const GrantMode granted = publisher_grant(state, domain);
    if (granted < required) {
        Status status = make_status(Outcome::UnauthorizedScope, "authority.scope_not_granted");
        status.with("publisher", state.publisher.to_string());
        status.with("domain", domain.to_string());
        status.with("granted", to_string(granted));
        status.with("required", to_string(required));
        return status;
    }
    if (out_state != nullptr) {
        *out_state = &state;
    }
    Status status;
    status.set_outcome(Outcome::Ok).set_code("authority.ok");
    return status;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TopologyEngine::TopologyEngine(TopologyEngineOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

TopologyEngine::~TopologyEngine() = default;

std::unique_ptr<TopologyEngine> TopologyEngine::open(TopologyEngineOptions options, LoadReport& report) {
    auto engine = std::make_unique<TopologyEngine>(std::move(options));
    const std::string& path = engine->impl_->options.persistence_path;
    if (!path.empty()) {
        report = engine->load_from(path);
    } else {
        report.status.set_outcome(Outcome::Ok).set_code("persistence.not_configured");
        report.loaded = false;
    }
    return engine;
}

// ---------------------------------------------------------------------------
// Domain administration
// ---------------------------------------------------------------------------

Status TopologyEngine::define_domain(const DomainDefinition& definition) {
    if (definition.id.empty()) {
        return make_status(Outcome::MalformedRequest, "domain.id_missing");
    }
    if (!definition.anchor_entity_id.empty() && impl_->directory != nullptr) {
        const auto record = impl_->directory->lookup(definition.anchor_entity_id);
        if (!record.has_value()) {
            Status status = make_status(Outcome::UnknownEntity, "domain.anchor_unknown");
            status.with("anchor", definition.anchor_entity_id);
            return status;
        }
        if (record->retired || record->superseded) {
            Status status = make_status(Outcome::Retired, "domain.anchor_not_live");
            status.with("anchor", definition.anchor_entity_id);
            return status;
        }
    }

    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    const auto existing = impl_->graph.domain_definitions.find(definition.id);
    if (existing != impl_->graph.domain_definitions.end()) {
        if (existing->second == definition) {
            return make_status(Outcome::Idempotent, "domain.unchanged");
        }
        Status status = make_status(Outcome::DuplicateIdentity, "domain.redefinition");
        status.with("domain", definition.id.to_string());
        return status;
    }

    if (!definition.parent.empty()) {
        if (impl_->graph.domain_definitions.find(definition.parent) == impl_->graph.domain_definitions.end()) {
            Status status = make_status(Outcome::UnknownEntity, "domain.parent_unknown");
            status.with("parent", definition.parent.to_string());
            return status;
        }
        // Domain nesting must stay acyclic.
        TopologyDomainId cursor = definition.parent;
        std::size_t guard = 0;
        while (!cursor.empty()) {
            if (cursor == definition.id) {
                return make_status(Outcome::CycleRejected, "domain.parent_cycle");
            }
            const auto it = impl_->graph.domain_definitions.find(cursor);
            if (it == impl_->graph.domain_definitions.end()) {
                break;
            }
            cursor = it->second.parent;
            if (++guard > impl_->limits.max_nodes) {
                return make_status(Outcome::ResourceLimit, "domain.parent_depth");
            }
        }
    }

    impl_->graph.domain_definitions.emplace(definition.id, definition);
    impl_->graph.domains[definition.id];
    return make_status(Outcome::Committed, "domain.defined");
}

Status TopologyEngine::define_cross_domain_rule(const CrossDomainRule& rule) {
    if (rule.from_domain.empty() || rule.to_domain.empty() || rule.relation == RelationClass::Unknown) {
        return make_status(Outcome::MalformedRequest, "cross_domain.rule_incomplete");
    }
    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    if (impl_->graph.domain_definitions.find(rule.from_domain) == impl_->graph.domain_definitions.end() ||
        impl_->graph.domain_definitions.find(rule.to_domain) == impl_->graph.domain_definitions.end()) {
        return make_status(Outcome::UnknownEntity, "cross_domain.domain_unknown");
    }
    for (const CrossDomainRule& existing : impl_->graph.cross_domain_rules) {
        if (existing == rule) {
            return make_status(Outcome::Idempotent, "cross_domain.unchanged");
        }
    }
    impl_->graph.cross_domain_rules.push_back(rule);
    return make_status(Outcome::Committed, "cross_domain.defined");
}

std::optional<DomainDefinition> TopologyEngine::domain(const TopologyDomainId& id) const {
    std::shared_lock lock(impl_->state_mutex);
    const auto it = impl_->graph.domain_definitions.find(id);
    if (it == impl_->graph.domain_definitions.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<DomainDefinition> TopologyEngine::domains() const {
    std::shared_lock lock(impl_->state_mutex);
    std::vector<DomainDefinition> result;
    result.reserve(impl_->graph.domain_definitions.size());
    for (const auto& entry : impl_->graph.domain_definitions) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(),
              [](const DomainDefinition& a, const DomainDefinition& b) { return a.id < b.id; });
    return result;
}

// ---------------------------------------------------------------------------
// Authority administration
// ---------------------------------------------------------------------------

Status TopologyEngine::register_publisher(const PublisherRegistration& registration) {
    if (registration.publisher.empty() || registration.worker_boot.empty()) {
        return make_status(Outcome::MalformedRequest, "registration.incomplete");
    }
    if (registration.reason.size() > impl_->limits.max_string_bytes) {
        return make_status(Outcome::ResourceLimit, "registration.reason_too_long");
    }

    std::unique_lock authority_lock(impl_->authority_mutex);
    if (impl_->boot_is_fenced_locked(registration.worker_boot.value())) {
        Status status = make_status(Outcome::StaleWorkerBoot, "registration.boot_fenced");
        status.with("worker_boot", registration.worker_boot.to_string());
        return status;
    }
    if (registration.coordinator_epoch != impl_->coordinator_epoch) {
        Status status = make_status(Outcome::StaleCoordinatorEpoch, "registration.epoch_stale");
        status.with("registration_epoch", registration.coordinator_epoch.to_string());
        status.with("current_epoch", impl_->coordinator_epoch.to_string());
        return status;
    }

    PublisherState state;
    state.publisher = registration.publisher;
    state.worker_boot = registration.worker_boot;
    state.coordinator_epoch = registration.coordinator_epoch;
    state.grants = registration.grants;
    state.reason = registration.reason;
    state.committed_mutations = 0;
    state.rejected_mutations = 0;

    const auto it = impl_->publishers.find(registration.publisher.value());
    if (it != impl_->publishers.end()) {
        if (it->second.worker_boot == registration.worker_boot &&
            it->second.coordinator_epoch == registration.coordinator_epoch &&
            it->second.grants == registration.grants && !it->second.fenced) {
            return make_status(Outcome::Idempotent, "registration.unchanged");
        }
        // A new incarnation replaces the previous registration for the same publisher id.
        const std::string previous_boot = it->second.worker_boot.value();
        if (previous_boot != registration.worker_boot.value()) {
            impl_->fenced_boots.insert(previous_boot);
        }
        it->second = state;
        return make_status(Outcome::Committed, "registration.replaced");
    }

    impl_->publishers.emplace(registration.publisher.value(), std::move(state));
    return make_status(Outcome::Committed, "registration.registered");
}

Status TopologyEngine::fence_publisher(const PublisherId& publisher, const WorkerBootId& boot,
                                       std::string_view reason) {
    if (publisher.empty() || boot.empty()) {
        return make_status(Outcome::MalformedRequest, "fence.incomplete");
    }
    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);
    impl_->fenced_boots.insert(boot.value());

    const auto it = impl_->publishers.find(publisher.value());
    if (it == impl_->publishers.end()) {
        Status status = make_status(Outcome::StaleAuthority, "fence.publisher_unregistered");
        status.with("publisher", publisher.to_string());
        status.with("worker_boot", boot.to_string());
        return status;
    }
    if (it->second.worker_boot != boot) {
        Status status = make_status(Outcome::StaleWorkerBoot, "fence.boot_mismatch");
        status.with("registered_boot", it->second.worker_boot.to_string());
        status.with("request_boot", boot.to_string());
        return status;
    }

    const bool already_fenced = it->second.fenced;
    it->second.fenced = true;
    if (it->second.fence_reason.empty()) {
        it->second.fence_reason.assign(reason.data(), reason.size());
    }

    // Relationships asserted by the fenced incarnation are no longer current: the runtime has
    // lost the evidence stream that kept them live. Durable structure is preserved.
    internal::StateTxn txn(impl_->graph);
    std::size_t demoted = 0;
    std::vector<TopologyEdgeId> affected;
    const auto publisher_it = impl_->graph.edge_by_publisher.find(publisher.value());
    if (publisher_it != impl_->graph.edge_by_publisher.end()) {
        affected.assign(publisher_it->second.begin(), publisher_it->second.end());
        std::sort(affected.begin(), affected.end());
    }
    for (const TopologyEdgeId& edge_id : affected) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found == nullptr || found->lifecycle != LifecycleState::Current) {
            continue;
        }
        TopologyEdge edge = *found;
        txn.note_edge_updated(edge);
        edge.lifecycle = LifecycleState::RevalidationRequired;
        if (edge.generation.can_advance()) {
            edge.generation = edge.generation.next();
        }
        impl_->graph.update_edge(edge);
        ++demoted;
    }
    if (demoted != 0) {
        if (!impl_->graph.generation.can_advance()) {
            txn.rollback();
            return make_status(Outcome::ResourceLimit, "generation.exhausted");
        }
        txn.note_generation(impl_->graph.generation);
        impl_->graph.generation = impl_->graph.generation.next();
        ++impl_->generation_advances;
        const Status persisted = impl_->persist_locked();
        if (persisted.outcome() != Outcome::Ok) {
            txn.rollback();
            return persisted;
        }
    }
    txn.commit();

    if (already_fenced) {
        Status status = make_status(Outcome::Idempotent, "fence.already_fenced");
        status.with("publisher", publisher.to_string());
        status.with("worker_boot", boot.to_string());
        status.with("demoted_relationships", std::to_string(demoted));
        return status;
    }
    Status status = make_status(Outcome::Committed, "fence.fenced");
    status.with("publisher", publisher.to_string());
    status.with("worker_boot", boot.to_string());
    status.with("demoted_relationships", std::to_string(demoted));
    if (!reason.empty()) {
        status.with("reason", std::string(reason));
    }
    return status;
}

std::size_t TopologyEngine::fence_boot(const WorkerBootId& boot, std::string_view reason,
                                       bool include_unregistered) {
    (void)include_unregistered;
    if (boot.empty()) {
        return 0;
    }
    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);
    impl_->fenced_boots.insert(boot.value());
    std::size_t fenced = 0;
    for (auto& entry : impl_->publishers) {
        if (entry.second.worker_boot == boot && !entry.second.fenced) {
            entry.second.fenced = true;
            entry.second.fence_reason.assign(reason.data(), reason.size());
            ++fenced;
        }
    }

    internal::StateTxn txn(impl_->graph);
    std::size_t demoted = 0;
    std::vector<TopologyEdgeId> affected;
    for (const auto& entry : impl_->graph.edges) {
        if (entry.second.provenance.worker_boot == boot &&
            entry.second.lifecycle == LifecycleState::Current) {
            affected.push_back(entry.first);
        }
    }
    std::sort(affected.begin(), affected.end());
    for (const TopologyEdgeId& edge_id : affected) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found == nullptr) {
            continue;
        }
        TopologyEdge edge = *found;
        txn.note_edge_updated(edge);
        edge.lifecycle = LifecycleState::RevalidationRequired;
        if (edge.generation.can_advance()) {
            edge.generation = edge.generation.next();
        }
        impl_->graph.update_edge(edge);
        ++demoted;
    }
    if (demoted != 0) {
        if (!impl_->graph.generation.can_advance()) {
            txn.rollback();
            return fenced;
        }
        txn.note_generation(impl_->graph.generation);
        impl_->graph.generation = impl_->graph.generation.next();
        ++impl_->generation_advances;
        const Status persisted = impl_->persist_locked();
        if (persisted.outcome() != Outcome::Ok) {
            txn.rollback();
            return fenced;
        }
    }
    txn.commit();
    return fenced;
}

std::optional<PublisherState> TopologyEngine::publisher_state(const PublisherId& publisher) const {
    std::unique_lock authority_lock(impl_->authority_mutex);
    const auto it = impl_->publishers.find(publisher.value());
    if (it == impl_->publishers.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<PublisherState> TopologyEngine::publishers() const {
    std::unique_lock authority_lock(impl_->authority_mutex);
    std::vector<PublisherState> result;
    result.reserve(impl_->publishers.size());
    for (const auto& entry : impl_->publishers) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(),
              [](const PublisherState& a, const PublisherState& b) { return a.publisher < b.publisher; });
    return result;
}

CoordinatorEpoch TopologyEngine::coordinator_epoch() const noexcept {
    std::unique_lock authority_lock(impl_->authority_mutex);
    return impl_->coordinator_epoch;
}

CoordinatorEpoch TopologyEngine::advance_coordinator_epoch(std::string_view reason) {
    (void)reason;
    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    if (!impl_->coordinator_epoch.can_advance()) {
        return impl_->coordinator_epoch;
    }
    impl_->coordinator_epoch = impl_->coordinator_epoch.next();

    // Process-local publisher authority never survives a coordinator restart.
    impl_->publishers.clear();

    // Durable structure is preserved; live currentness is not. Every relationship that was
    // asserted by a distributed publisher becomes revalidation-required, because the runtime
    // cannot claim it is physically current after losing its evidence stream.
    internal::StateTxn txn(impl_->graph);
    bool changed = false;
    std::vector<TopologyEdgeId> epoch_affected;
    epoch_affected.reserve(impl_->graph.edges.size());
    for (const auto& entry : impl_->graph.edges) {
        epoch_affected.push_back(entry.first);
    }
    std::sort(epoch_affected.begin(), epoch_affected.end());
    for (const TopologyEdgeId& edge_id : epoch_affected) {
        const TopologyEdge* stored = impl_->graph.find_edge(edge_id);
        if (stored == nullptr || stored->provenance.publisher.empty()) {
            continue;
        }
        if (stored->lifecycle != LifecycleState::Current) {
            continue;
        }
        txn.note_edge_updated(*stored);
        static_cast<void>(impl_->graph.mutate_edge(edge_id, [](TopologyEdge& edge) {
            edge.lifecycle = LifecycleState::RevalidationRequired;
            if (edge.generation.can_advance()) {
                edge.generation = edge.generation.next();
            }
        }));
        changed = true;
    }
    if (changed) {
        if (impl_->graph.generation.can_advance()) {
            txn.note_generation(impl_->graph.generation);
            impl_->graph.generation = impl_->graph.generation.next();
            ++impl_->generation_advances;
        }
        const Status persisted = impl_->persist_locked();
        if (!persisted.has("persistence.ok") && persisted.outcome() != Outcome::Ok &&
            persisted.outcome() != Outcome::Committed) {
            txn.rollback();
        }
    } else {
        txn.commit();
    }
    return impl_->coordinator_epoch;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

namespace {

void fence_edges_for_node_change(internal::GraphState& graph, internal::StateTxn& txn,
                                 const TopologyNodeId& node_id, EntityGeneration new_generation,
                                 std::vector<DiffEntry>& diff) {
    const auto it = graph.incident.find(node_id);
    if (it == graph.incident.end()) {
        return;
    }
    std::vector<TopologyEdgeId> affected(it->second.begin(), it->second.end());
    std::sort(affected.begin(), affected.end());
    for (const TopologyEdgeId& edge_id : affected) {
        const TopologyEdge* found = graph.find_edge(edge_id);
        if (found == nullptr) {
            continue;
        }
        TopologyEdge edge = *found;
        if (edge.lifecycle == LifecycleState::Retired || edge.lifecycle == LifecycleState::Superseded) {
            continue;
        }
        const EntityGeneration bound_before = edge.from == node_id ? edge.from_entity_generation
                                                                   : edge.to_entity_generation;
        if (bound_before == new_generation && edge.lifecycle == LifecycleState::RevalidationRequired) {
            continue;
        }
        const LifecycleState previous_lifecycle = edge.lifecycle;
        txn.note_edge_updated(edge);
        if (edge.from == node_id) {
            edge.from_entity_generation = new_generation;
        } else {
            edge.to_entity_generation = new_generation;
        }
        edge.lifecycle = LifecycleState::RevalidationRequired;
        if (edge.generation.can_advance()) {
            edge.generation = edge.generation.next();
        }
        graph.update_edge(edge);

        DiffEntry entry;
        entry.kind = DiffKind::CurrentnessChanged;
        entry.key = edge.id.to_string();
        entry.edge = edge.id;
        entry.before = to_string(previous_lifecycle);
        entry.after = to_string(LifecycleState::RevalidationRequired);
        diff.push_back(std::move(entry));
    }
}

}  // namespace

MutationResult TopologyEngine::add_node(const AuthorityContext& authority, const AddNodeRequest& request) {
    MutationResult result;
    if (request.entity_id.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "node.entity_id_missing");
        return result;
    }
    if (request.entity_id.size() > impl_->limits.max_identifier_bytes) {
        result.status = make_status(Outcome::ResourceLimit, "node.entity_id_too_long");
        return result;
    }
    if (request.node_class == NodeClass::Unknown) {
        result.status = make_status(Outcome::MalformedRequest, "node.node_class_unknown");
        return result;
    }
    if (request.domain.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "node.domain_missing");
        return result;
    }
    const Status metadata_status = Metadata::validate(request.metadata.items(), impl_->limits);
    if (metadata_status.outcome() != Outcome::Ok) {
        result.status = metadata_status;
        return result;
    }

    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    // An undeclared scope is a malformed request, not an authority problem: report it before
    // authority is considered so the caller gets the precise cause.
    if (impl_->graph.domain_definitions.find(request.domain) == impl_->graph.domain_definitions.end()) {
        impl_->note_rejected();
        Status status = make_status(Outcome::DomainViolation, "node.domain_unknown");
        status.with("domain", request.domain.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    PublisherState* publisher = nullptr;
    const Status authority_status = impl_->check_authority_locked(
        authority, request.domain, GrantMode::IncrementalWrite, &publisher);
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
        Status status = make_status(Outcome::StaleGeneration, "node.stale_generation");
        status.with("expected", request.expected_generation.to_string());
        status.with("current", impl_->graph.generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    std::optional<EntityRecord> record;
    if (impl_->directory != nullptr) {
        record = impl_->directory->lookup(request.entity_id);
    }
    if (!record.has_value()) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::UnknownEntity, "node.entity_unknown");
        status.with("entity_id", request.entity_id);
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (record->retired) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::Retired, "node.entity_retired");
        status.with("entity_id", request.entity_id);
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (record->superseded) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::Superseded, "node.entity_superseded");
        status.with("entity_id", request.entity_id);
        if (!record->superseded_by.empty()) {
            status.with("superseded_by", record->superseded_by);
        }
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }
    const EntityClass expected_class = node_class_entity_class(request.node_class);
    if (expected_class == EntityClass::Unknown || record->entity_class != expected_class) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::IncompatibleEntityClass, "node.entity_class_mismatch");
        status.with("entity_id", request.entity_id);
        status.with("registry_class", to_string(record->entity_class));
        status.with("node_class", to_string(request.node_class));
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    const TopologyNodeId node_id = request.node_id.has_value()
                                       ? *request.node_id
                                       : internal::derive_node_id(request.domain, request.entity_id);
    if (node_id.empty()) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        result.status = make_status(Outcome::MalformedRequest, "node.id_missing");
        result.generation = impl_->graph.generation;
        return result;
    }

    internal::StateTxn txn(impl_->graph);
    std::vector<DiffEntry> diff;

    const TopologyNode* existing = impl_->graph.find_node(node_id);
    if (existing != nullptr) {
        if (existing->entity_id != request.entity_id) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::DuplicateIdentity, "node.id_reused");
            status.with("node_id", node_id.to_string());
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
        TopologyNode updated = *existing;
        const bool unchanged = updated.entity_generation == record->generation &&
                               updated.tier == request.tier &&
                               metadata_equal(updated.metadata, request.metadata) &&
                               updated.lifecycle == LifecycleState::Current;
        if (unchanged) {
            txn.commit();
            impl_->note_idempotent();
            ++publisher->committed_mutations;
            result.status = make_status(Outcome::Idempotent, "node.unchanged");
            result.node = node_id;
            result.generation = impl_->graph.generation;
            return result;
        }
        txn.note_node_updated(updated);
        const EntityGeneration previous_generation = updated.entity_generation;
        updated.entity_generation = record->generation;
        updated.tier = request.tier;
        updated.metadata = request.metadata;
        updated.lifecycle = LifecycleState::Current;
        if (updated.generation.can_advance()) {
            updated.generation = updated.generation.next();
        }
        updated.last_validated_generation = impl_->graph.generation;
        impl_->graph.update_node(updated);

        if (previous_generation != updated.entity_generation) {
            fence_edges_for_node_change(impl_->graph, txn, node_id, updated.entity_generation, diff);
        }

        DiffEntry entry;
        entry.kind = DiffKind::NodeGenerationChanged;
        entry.key = node_id.to_string();
        entry.node = node_id;
        entry.before = previous_generation.to_string();
        entry.after = updated.entity_generation.to_string();
        diff.push_back(std::move(entry));
    } else {
        if (impl_->graph.find_node_by_entity(request.entity_id) != nullptr) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            Status status = make_status(Outcome::DuplicateIdentity, "node.entity_already_bound");
            status.with("entity_id", request.entity_id);
            result.status = status;
            result.generation = impl_->graph.generation;
            return result;
        }
        if (impl_->graph.nodes.size() >= impl_->limits.max_nodes) {
            ++publisher->rejected_mutations;
            impl_->note_rejected();
            result.status = make_status(Outcome::ResourceLimit, "node.limit_reached");
            result.generation = impl_->graph.generation;
            return result;
        }
        TopologyNode node;
        node.id = node_id;
        node.entity_id = request.entity_id;
        node.entity_class = record->entity_class;
        node.entity_generation = record->generation;
        node.node_class = request.node_class;
        node.tier = request.tier;
        node.domain = request.domain;
        node.generation = NodeGeneration{1};
        node.created_generation = impl_->graph.generation;
        node.last_validated_generation = impl_->graph.generation;
        node.lifecycle = LifecycleState::Current;
        node.provenance.publisher = authority.publisher;
        node.provenance.worker_boot = authority.worker_boot;
        node.provenance.coordinator_epoch = authority.coordinator_epoch;
        node.provenance.source = DiscoverySource::OperatorDeclaration;
        node.provenance.evidence_type = PublicationType::Real;
        node.provenance.evidence_generation = authority.evidence_generation;
        node.provenance.publication = authority.publication;
        node.provenance.created_generation = impl_->graph.generation;
        node.provenance.last_validated_generation = impl_->graph.generation;
        node.metadata = request.metadata;

        txn.note_node_inserted(node_id);
        if (!impl_->graph.insert_node(node)) {
            txn.rollback();
            impl_->note_rejected();
            ++publisher->rejected_mutations;
            result.status = make_status(Outcome::InternalError, "node.insert_failed");
            result.generation = impl_->graph.generation;
            return result;
        }
        DiffEntry entry;
        entry.kind = DiffKind::NodeAdded;
        entry.key = node_id.to_string();
        entry.node = node_id;
        entry.after = render_node(node);
        diff.push_back(std::move(entry));
    }

    txn.note_generation(impl_->graph.generation);
    if (!impl_->graph.generation.can_advance()) {
        txn.rollback();
        impl_->note_rejected();
        ++publisher->rejected_mutations;
        result.status = make_status(Outcome::ResourceLimit, "generation.exhausted");
        result.generation = impl_->graph.generation;
        return result;
    }
    impl_->graph.generation = impl_->graph.generation.next();
    ++impl_->generation_advances;

    const Status persisted = impl_->persist_locked();
    if (persisted.outcome() != Outcome::Ok) {
        txn.rollback();
        impl_->note_rejected();
        ++publisher->rejected_mutations;
        result.status = persisted;
        result.generation = impl_->graph.generation;
        return result;
    }
    txn.commit();

    if (impl_->options.verify_indexes_on_mutation) {
        const Status indexes = impl_->graph.verify_indexes();
        if (indexes.outcome() != Outcome::Ok) {
            result.status = indexes;
            return result;
        }
    }

    ++publisher->committed_mutations;
    impl_->note_committed();
    result.status = make_status(Outcome::Committed, "node.committed");
    result.node = node_id;
    result.generation = impl_->graph.generation;
    result.diff.entries = std::move(diff);
    result.diff.from_generation = TopologyGeneration{result.generation.value() - 1};
    result.diff.to_generation = result.generation;
    return result;
}

MutationResult TopologyEngine::remove_node(const AuthorityContext& authority, const RemoveNodeRequest& request) {
    MutationResult result;
    if (request.node.empty()) {
        result.status = make_status(Outcome::MalformedRequest, "node.id_missing");
        return result;
    }
    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    const TopologyNode* node = impl_->graph.find_node(request.node);
    if (node == nullptr) {
        impl_->note_rejected();
        Status status = make_status(Outcome::NotFound, "node.not_found");
        status.with("node_id", request.node.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    PublisherState* publisher = nullptr;
    Status authority_status = impl_->check_authority_locked(authority, node->domain,
                                                            GrantMode::AuthoritativeWrite, &publisher);
    if (authority_status.outcome() != Outcome::Ok) {
        impl_->note_rejected();
        result.status = authority_status;
        result.generation = impl_->graph.generation;
        return result;
    }
    if (!request.expected_generation.is_zero() && request.expected_generation != impl_->graph.generation) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::StaleGeneration, "node.stale_generation");
        status.with("expected", request.expected_generation.to_string());
        status.with("current", impl_->graph.generation.to_string());
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    const auto incident_it = impl_->graph.incident.find(request.node);
    const bool has_edges = incident_it != impl_->graph.incident.end() && !incident_it->second.empty();
    if (has_edges && !request.cascade_relationships) {
        ++publisher->rejected_mutations;
        impl_->note_rejected();
        Status status = make_status(Outcome::StructuralInvariantViolation, "node.has_relationships");
        status.with("node_id", request.node.to_string());
        status.with("incident", std::to_string(incident_it->second.size()));
        result.status = status;
        result.generation = impl_->graph.generation;
        return result;
    }

    // Capture everything that the diff and the undo log need before anything is erased: a
    // pointer into the graph must never outlive the erase that invalidates it.
    const TopologyNode removed_node = *node;

    internal::StateTxn txn(impl_->graph);
    std::vector<DiffEntry> diff;
    std::vector<TopologyEdgeId> removed_edges =
        has_edges ? std::vector<TopologyEdgeId>(incident_it->second.begin(), incident_it->second.end())
                  : std::vector<TopologyEdgeId>{};
    std::sort(removed_edges.begin(), removed_edges.end());
    for (const TopologyEdgeId& edge_id : removed_edges) {
        const TopologyEdge* found = impl_->graph.find_edge(edge_id);
        if (found == nullptr) {
            continue;
        }
        const TopologyEdge edge = *found;
        txn.note_edge_erased(edge);
        impl_->graph.erase_edge(edge_id);
        DiffEntry entry;
        entry.kind = DiffKind::EdgeRemoved;
        entry.key = edge_id.to_string();
        entry.edge = edge_id;
        entry.before = render_edge(edge);
        diff.push_back(std::move(entry));
    }

    txn.note_node_erased(removed_node);
    impl_->graph.erase_node(request.node);

    DiffEntry node_entry;
    node_entry.kind = DiffKind::NodeRemoved;
    node_entry.key = request.node.to_string();
    node_entry.node = request.node;
    node_entry.before = render_node(removed_node);
    diff.push_back(std::move(node_entry));

    txn.note_generation(impl_->graph.generation);
    if (!impl_->graph.generation.can_advance()) {
        txn.rollback();
        impl_->note_rejected();
        ++publisher->rejected_mutations;
        result.status = make_status(Outcome::ResourceLimit, "generation.exhausted");
        result.generation = impl_->graph.generation;
        return result;
    }
    impl_->graph.generation = impl_->graph.generation.next();
    ++impl_->generation_advances;

    const Status persisted = impl_->persist_locked();
    if (persisted.outcome() != Outcome::Ok) {
        txn.rollback();
        impl_->note_rejected();
        ++publisher->rejected_mutations;
        result.status = persisted;
        result.generation = impl_->graph.generation;
        return result;
    }
    txn.commit();

    ++publisher->committed_mutations;
    impl_->note_committed();
    std::sort(diff.begin(), diff.end(), [](const DiffEntry& a, const DiffEntry& b) {
        if (a.kind != b.kind) {
            return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
        }
        return a.key < b.key;
    });
    result.status = make_status(Outcome::Committed, "node.removed");
    result.node = request.node;
    result.generation = impl_->graph.generation;
    result.diff.entries = std::move(diff);
    result.diff.from_generation = TopologyGeneration{result.generation.value() - 1};
    result.diff.to_generation = result.generation;
    return result;
}

}  // namespace fabric_topology
