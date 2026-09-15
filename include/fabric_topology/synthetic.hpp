// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_SYNTHETIC_HPP
#define FABRIC_TOPOLOGY_SYNTHETIC_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric_topology/publication.hpp"
#include "fabric_topology/registry.hpp"

namespace fabric_topology {

/// Synthetic topology scenarios. Every fact produced by this backend is tagged
/// PublicationType::Synthetic. Nothing here is physical evidence.
enum class SyntheticScenario : std::uint8_t {
    SingleSwitch = 0,
    DualSwitchRedundancy = 1,
    LeafSpine = 2,
    MultiTierClos = 3,
    RackToLeaf = 4,
    MultipleFabrics = 5,
    MultiSite = 6,
    LogicalOverlay = 7,
    PhysicalAndLogical = 8,
    Partitioned = 9,
    DeviceReplacement = 10,
    AttachmentMove = 11,
    AsymmetricPublisherScopes = 12,
    StaleSnapshot = 13,
    LargeScale = 14,
};

inline constexpr std::uint8_t kSyntheticScenarioCount = 15;

[[nodiscard]] const char* to_string(SyntheticScenario value) noexcept;
[[nodiscard]] std::optional<SyntheticScenario> synthetic_scenario_from_string(std::string_view text) noexcept;
[[nodiscard]] std::vector<SyntheticScenario> synthetic_scenarios();

struct SyntheticOptions {
    SyntheticScenario scenario = SyntheticScenario::SingleSwitch;
    /// Scenario-specific scale. Zero selects the documented default for the scenario.
    std::size_t scale = 0;
    /// Number of spine switches for LeafSpine; number of tiers for MultiTierClos.
    std::size_t tiers = 2;
    /// Deterministic seed for scenarios that vary structure with scale.
    std::uint64_t seed = 20260101ULL;
    /// Domain the publication targets.
    TopologyDomainId domain;
    ScopeKind scope_kind = ScopeKind::AdministrativeDomain;
    PublisherId publisher;
    /// Fabric Registry identities are generated as "<prefix><index>".
    std::string entity_prefix = "syn";
};

/// A complete synthetic scenario: the publication to submit, the canonical identities the
/// publication references, and a human-readable description.
struct SyntheticTopology {
    SyntheticScenario scenario = SyntheticScenario::SingleSwitch;
    std::string name;
    std::string description;
    Publication publication;
    std::vector<EntityRecord> directory_entries;
    /// Additional scenario-specific publications (second fabric, second site, partition).
    std::vector<Publication> secondary_publications;

    [[nodiscard]] std::string render() const;
};

[[nodiscard]] SyntheticTopology build_synthetic_topology(const SyntheticOptions& options);

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_SYNTHETIC_HPP
