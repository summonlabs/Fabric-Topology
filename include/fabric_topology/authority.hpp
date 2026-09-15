// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_AUTHORITY_HPP
#define FABRIC_TOPOLOGY_AUTHORITY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric_topology/ids.hpp"
#include "fabric_topology/types.hpp"

namespace fabric_topology {

/// What a publisher may do inside one authority scope.
enum class GrantMode : std::uint8_t {
    None = 0,
    Read = 1,
    /// May add and update relationships; may not delete unmentioned ones.
    IncrementalWrite = 2,
    /// May additionally replace the whole scope transactionally.
    AuthoritativeWrite = 3,
};

inline constexpr std::uint8_t kGrantModeCount = 4;

[[nodiscard]] const char* to_string(GrantMode value) noexcept;
[[nodiscard]] std::optional<GrantMode> grant_mode_from_string(std::string_view text) noexcept;

struct ScopeGrant {
    TopologyDomainId domain;
    GrantMode mode = GrantMode::None;

    friend bool operator==(const ScopeGrant&, const ScopeGrant&) = default;
};

struct DomainDefinition {
    TopologyDomainId id;
    ScopeKind kind = ScopeKind::AdministrativeDomain;
    /// Canonical Fabric Registry identity this domain is anchored to (FabricId or SiteId).
    /// Empty for pure administrative/physical/logical/control-plane domains.
    std::string anchor_entity_id;
    EntityClass anchor_entity_class = EntityClass::Unknown;
    /// Optional parent domain. Domain nesting must stay acyclic.
    TopologyDomainId parent;

    friend bool operator==(const DomainDefinition&, const DomainDefinition&) = default;
};

/// Explicit permission for a relationship class to cross between two scopes. Cross-domain
/// edges are rejected unless a rule permits the exact ordered pair.
struct CrossDomainRule {
    TopologyDomainId from_domain;
    TopologyDomainId to_domain;
    RelationClass relation = RelationClass::Unknown;
    bool symmetric = true;

    friend bool operator==(const CrossDomainRule&, const CrossDomainRule&) = default;
};

/// Authority carried by one mutation or publication request. Every field is checked before
/// any state is touched.
struct AuthorityContext {
    CoordinatorEpoch coordinator_epoch;
    PublisherId publisher;
    WorkerBootId worker_boot;
    EvidenceGeneration evidence_generation;
    /// Optional attempt identifier; when set, replayed attempts are recognised as idempotent
    /// instead of being re-applied.
    MutationAttemptId attempt;
    /// Publication carrying this mutation, when it arrives through the distributed path.
    PublicationId publication;

    friend bool operator==(const AuthorityContext&, const AuthorityContext&) = default;
};

struct PublisherRegistration {
    PublisherId publisher;
    WorkerBootId worker_boot;
    CoordinatorEpoch coordinator_epoch;
    std::vector<ScopeGrant> grants;
    std::string reason;

    friend bool operator==(const PublisherRegistration&, const PublisherRegistration&) = default;
};

/// Live authority record for one publisher.
struct PublisherState {
    PublisherId publisher;
    WorkerBootId worker_boot;
    CoordinatorEpoch coordinator_epoch;
    std::vector<ScopeGrant> grants;
    bool fenced = false;
    std::string fence_reason;
    std::string reason;
    /// Number of committed mutations attributed to this publisher.
    std::uint64_t committed_mutations = 0;
    /// Number of rejected mutations attributed to this publisher.
    std::uint64_t rejected_mutations = 0;

    [[nodiscard]] GrantMode grant_for(const TopologyDomainId& domain) const noexcept;
    [[nodiscard]] std::string render() const;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_AUTHORITY_HPP
