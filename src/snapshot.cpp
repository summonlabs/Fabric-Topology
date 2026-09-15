// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

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

}  // namespace

SnapshotScope SnapshotScope::of_domain(TopologyDomainId domain) {
    SnapshotScope scope;
    scope.whole_topology = false;
    scope.domain = std::move(domain);
    return scope;
}

SnapshotScope SnapshotScope::of_relation(RelationClass relation) {
    SnapshotScope scope;
    scope.filter_relation = true;
    scope.relation = relation;
    return scope;
}

SnapshotScope SnapshotScope::of_layer(TopologyLayer layer) {
    SnapshotScope scope;
    scope.filter_layer = true;
    scope.layer = layer;
    return scope;
}

const char* to_string(SnapshotVerdict value) noexcept {
    switch (value) {
        case SnapshotVerdict::Current: return "CURRENT";
        case SnapshotVerdict::OlderGeneration: return "OLDER_GENERATION";
        case SnapshotVerdict::NewerGeneration: return "NEWER_GENERATION";
        case SnapshotVerdict::DifferentCoordinatorEpoch: return "DIFFERENT_COORDINATOR_EPOCH";
        case SnapshotVerdict::ScopeMismatch: return "SCOPE_MISMATCH";
        case SnapshotVerdict::DigestMismatch: return "DIGEST_MISMATCH";
    }
    return "OLDER_GENERATION";
}

std::string SnapshotCurrentness::render() const {
    std::string out;
    out.reserve(96);
    out += "current=";
    out += current ? "true" : "false";
    out += " verdict=";
    out += to_string(verdict);
    if (!detail.empty()) {
        out += " detail=";
        out += detail;
    }
    return out;
}

std::string TopologySnapshot::render() const {
    std::string out;
    out.reserve(512);
    out += "snapshot=";
    out += id.to_string();
    out += "\nsnapshot_generation=";
    out += snapshot_generation.to_string();
    out += "\ntopology_generation=";
    out += topology_generation.to_string();
    out += "\ncoordinator_epoch=";
    out += coordinator_epoch.to_string();
    out += "\nscope=";
    out += scope.whole_topology ? "ALL" : scope.domain.to_string();
    out += "\ndigest=";
    out += digest;
    out += "\nnodes=";
    out += std::to_string(nodes.size());
    out += '\n';
    for (const TopologyNode& node : nodes) {
        out += render_node(node);
        out += '\n';
    }
    out += "edges=";
    out += std::to_string(edges.size());
    out += '\n';
    for (const TopologyEdge& edge : edges) {
        out += render_edge(edge);
        out += '\n';
    }
    return out;
}

TopologySnapshot TopologyEngine::snapshot(const SnapshotScope& scope) const {
    // The expensive part -- copying the content and computing the deterministic digest -- runs
    // under a shared lock, so concurrent readers are never blocked by snapshot construction.
    // The exclusive lock is only taken to allocate the snapshot ordinal and record history,
    // and it is retried if a mutation landed in between.
    TopologySnapshot snapshot;
    for (int attempt = 0; attempt < 8; ++attempt) {
        {
            std::shared_lock content_lock(impl_->state_mutex);
            snapshot.topology_generation = impl_->graph.generation;
            snapshot.coordinator_epoch = impl_->coordinator_epoch;
            snapshot.scope = scope;
            snapshot.nodes = impl_->graph.sorted_nodes(scope);
            snapshot.edges = impl_->graph.sorted_edges(scope);
            snapshot.digest = impl_->graph.topology_digest(scope);
        }
        std::unique_lock state_lock(impl_->state_mutex);
        if (impl_->graph.generation != snapshot.topology_generation ||
            impl_->coordinator_epoch != snapshot.coordinator_epoch) {
            continue;
        }
        if (impl_->graph.snapshot_counter.can_advance()) {
            impl_->graph.snapshot_counter = impl_->graph.snapshot_counter.next();
        }
        snapshot.snapshot_generation = impl_->graph.snapshot_counter;
        snapshot.id =
            TopologySnapshotId::from_trusted("snap_" + snapshot.snapshot_generation.to_string());
        impl_->snapshots.push_back(snapshot);
        while (impl_->snapshots.size() > impl_->options.snapshot_history &&
               !impl_->snapshots.empty()) {
            impl_->snapshots.erase(impl_->snapshots.begin());
        }
        return snapshot;
    }

    // Pathological contention: fall back to the fully serialised construction so the caller
    // always receives a self-consistent snapshot.
    std::unique_lock state_lock(impl_->state_mutex);
    if (impl_->graph.snapshot_counter.can_advance()) {
        impl_->graph.snapshot_counter = impl_->graph.snapshot_counter.next();
    }
    snapshot.snapshot_generation = impl_->graph.snapshot_counter;
    snapshot.id = TopologySnapshotId::from_trusted("snap_" + snapshot.snapshot_generation.to_string());
    snapshot.topology_generation = impl_->graph.generation;
    snapshot.coordinator_epoch = impl_->coordinator_epoch;
    snapshot.scope = scope;
    snapshot.nodes = impl_->graph.sorted_nodes(scope);
    snapshot.edges = impl_->graph.sorted_edges(scope);
    snapshot.digest = impl_->graph.topology_digest(scope);
    impl_->snapshots.push_back(snapshot);
    while (impl_->snapshots.size() > impl_->options.snapshot_history && !impl_->snapshots.empty()) {
        impl_->snapshots.erase(impl_->snapshots.begin());
    }
    return snapshot;
}

std::string TopologyEngine::digest(const SnapshotScope& scope) const {
    std::shared_lock lock(impl_->state_mutex);
    return impl_->graph.topology_digest(scope);
}

SnapshotCurrentness TopologyEngine::check_snapshot(const TopologySnapshot& snapshot) const {
    std::shared_lock lock(impl_->state_mutex);

    SnapshotCurrentness currentness;
    if (snapshot.coordinator_epoch != impl_->coordinator_epoch) {
        currentness.verdict = SnapshotVerdict::DifferentCoordinatorEpoch;
        currentness.current = false;
        currentness.detail = "snapshot epoch " + snapshot.coordinator_epoch.to_string() +
                             " runtime epoch " + impl_->coordinator_epoch.to_string();
        return currentness;
    }
    const std::string expected_digest = impl_->graph.topology_digest(snapshot.scope);
    if (snapshot.digest != expected_digest) {
        currentness.verdict = SnapshotVerdict::DigestMismatch;
        currentness.current = false;
        currentness.detail = "snapshot digest " + snapshot.digest + " runtime digest " + expected_digest;
        return currentness;
    }
    if (snapshot.topology_generation < impl_->graph.generation) {
        currentness.verdict = SnapshotVerdict::OlderGeneration;
        currentness.current = false;
        currentness.detail = "snapshot generation " + snapshot.topology_generation.to_string() +
                             " runtime generation " + impl_->graph.generation.to_string();
        return currentness;
    }
    if (snapshot.topology_generation > impl_->graph.generation) {
        currentness.verdict = SnapshotVerdict::NewerGeneration;
        currentness.current = false;
        currentness.detail = "snapshot generation is ahead of the runtime generation";
        return currentness;
    }
    currentness.verdict = SnapshotVerdict::Current;
    currentness.current = true;
    currentness.detail = "digest, generation and coordinator epoch match the runtime";
    return currentness;
}

std::optional<TopologySnapshot> TopologyEngine::snapshot_history(std::size_t index) const {
    std::shared_lock lock(impl_->state_mutex);
    if (index >= impl_->snapshots.size()) {
        return std::nullopt;
    }
    return impl_->snapshots[index];
}

std::size_t TopologyEngine::snapshot_count() const {
    std::shared_lock lock(impl_->state_mutex);
    return impl_->snapshots.size();
}

}  // namespace fabric_topology
