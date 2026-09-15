// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_TYPES_HPP
#define FABRIC_TOPOLOGY_TYPES_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fabric_topology {

/// Canonical entity classes owned by Fabric Registry. Fabric Topology never mints these
/// identifiers; it only references them.
enum class EntityClass : std::uint8_t {
    Unknown = 0,
    Fabric = 1,
    Site = 2,
    Device = 3,
    Switch = 4,
    Router = 5,
    Nic = 6,
    SmartNic = 7,
    Dpu = 8,
    Port = 9,
    LogicalPort = 10,
    Link = 11,
    Endpoint = 12,
    ControlParticipant = 13,
};

inline constexpr std::uint8_t kEntityClassCount = 14;

[[nodiscard]] const char* to_string(EntityClass value) noexcept;
[[nodiscard]] std::optional<EntityClass> entity_class_from_string(std::string_view text) noexcept;

/// Node classes representable in the governed topology graph. Every class maps onto exactly
/// one canonical Fabric Registry entity class (see node_class_entity_class). There is no
/// synthetic "aggregation" node class: no such node can be justified against registry
/// identity, so the model does not offer one.
enum class NodeClass : std::uint8_t {
    Unknown = 0,
    Site = 1,
    Fabric = 2,
    RackNetworkDomain = 3,
    Switch = 4,
    Router = 5,
    Nic = 6,
    SmartNic = 7,
    DpuEndpoint = 8,
    PhysicalPort = 9,
    LogicalPort = 10,
    FabricEndpoint = 11,
    LogicalEndpoint = 12,
};

inline constexpr std::uint8_t kNodeClassCount = 13;

[[nodiscard]] const char* to_string(NodeClass value) noexcept;
[[nodiscard]] std::optional<NodeClass> node_class_from_string(std::string_view text) noexcept;
[[nodiscard]] EntityClass node_class_entity_class(NodeClass value) noexcept;
[[nodiscard]] std::uint32_t node_class_bit(NodeClass value) noexcept;
[[nodiscard]] bool node_class_in_mask(std::uint32_t mask, NodeClass value) noexcept;
[[nodiscard]] bool node_class_is_endpoint(NodeClass value) noexcept;

/// Finite relation model. Authoritative topology state may not contain an untyped
/// relationship: every edge carries one of these classes.
enum class RelationClass : std::uint8_t {
    Unknown = 0,
    ConnectedTo = 1,
    AttachedTo = 2,
    Contains = 3,
    MemberOf = 4,
    UplinkTo = 5,
    DownlinkTo = 6,
    PeersWith = 7,
    BackedBy = 8,
    TunneledOver = 9,
    LogicallyConnectedTo = 10,
    HostedBy = 11,
    PresentsEndpoint = 12,
    FabricMembership = 13,
};

inline constexpr std::uint8_t kRelationClassCount = 14;

[[nodiscard]] const char* to_string(RelationClass value) noexcept;
[[nodiscard]] std::optional<RelationClass> relation_class_from_string(std::string_view text) noexcept;

enum class Directionality : std::uint8_t {
    Directed = 0,
    Undirected = 1,
};

[[nodiscard]] const char* to_string(Directionality value) noexcept;

enum class TopologyLayer : std::uint8_t {
    Physical = 0,
    Logical = 1,
};

[[nodiscard]] const char* to_string(TopologyLayer value) noexcept;
[[nodiscard]] std::optional<TopologyLayer> topology_layer_from_string(std::string_view text) noexcept;

/// Structural tier. Tiers are descriptive labels over general graph semantics: no network
/// architecture (spine-leaf, fat-tree, Clos, mesh, ring, direct-connect) is mandatory, and
/// none is hard-coded into validation. Unspecified means "not structurally meaningful here".
enum class TopologyTier : std::uint8_t {
    Unspecified = 0,
    Endpoint = 1,
    Access = 2,
    TopOfRack = 3,
    Leaf = 4,
    Spine = 5,
    SuperSpine = 6,
    Border = 7,
    Gateway = 8,
    SiteEdge = 9,
    InterSite = 10,
    OpticalInterconnect = 11,
};

inline constexpr std::uint8_t kTopologyTierCount = 12;

[[nodiscard]] const char* to_string(TopologyTier value) noexcept;
[[nodiscard]] std::optional<TopologyTier> topology_tier_from_string(std::string_view text) noexcept;

/// Authority scope of a topology domain.
enum class ScopeKind : std::uint8_t {
    Fabric = 0,
    Site = 1,
    AdministrativeDomain = 2,
    PhysicalDomain = 3,
    LogicalDomain = 4,
    ControlPlaneDomain = 5,
};

inline constexpr std::uint8_t kScopeKindCount = 6;

[[nodiscard]] const char* to_string(ScopeKind value) noexcept;
[[nodiscard]] std::optional<ScopeKind> scope_kind_from_string(std::string_view text) noexcept;

/// Currentness of a durable topology fact. Durability and currentness are separate
/// dimensions: a recovered relationship is durable but not automatically current.
enum class LifecycleState : std::uint8_t {
    Unknown = 0,
    Current = 1,
    RevalidationRequired = 2,
    Superseded = 3,
    Retired = 4,
    Conflicted = 5,
};

inline constexpr std::uint8_t kLifecycleStateCount = 6;

[[nodiscard]] const char* to_string(LifecycleState value) noexcept;
[[nodiscard]] std::optional<LifecycleState> lifecycle_state_from_string(std::string_view text) noexcept;
[[nodiscard]] bool lifecycle_is_queryable(LifecycleState value) noexcept;

/// Publication mode. The modes have materially different semantics; an incremental or
/// partial publication never implies deletion of unmentioned authoritative relationships.
enum class PublicationMode : std::uint8_t {
    Incremental = 0,
    AuthoritativeSnapshot = 1,
    PartialObservation = 2,
};

inline constexpr std::uint8_t kPublicationModeCount = 3;

[[nodiscard]] const char* to_string(PublicationMode value) noexcept;
[[nodiscard]] std::optional<PublicationMode> publication_mode_from_string(std::string_view text) noexcept;

/// Truthfulness classification carried through topology provenance.
enum class PublicationType : std::uint8_t {
    Real = 0,
    Synthetic = 1,
    Unsupported = 2,
};

inline constexpr std::uint8_t kPublicationTypeCount = 3;

[[nodiscard]] const char* to_string(PublicationType value) noexcept;
[[nodiscard]] std::optional<PublicationType> publication_type_from_string(std::string_view text) noexcept;

/// How a topology fact was learned.
enum class DiscoverySource : std::uint8_t {
    Unknown = 0,
    FabricRegistry = 1,
    HostDiscovery = 2,
    ControllerPublication = 3,
    OperatorDeclaration = 4,
    SyntheticGenerator = 5,
    PersistenceRecovery = 6,
};

inline constexpr std::uint8_t kDiscoverySourceCount = 7;

[[nodiscard]] const char* to_string(DiscoverySource value) noexcept;
[[nodiscard]] std::optional<DiscoverySource> discovery_source_from_string(std::string_view text) noexcept;

/// Layer policy enforced for a relation class.
enum class LayerPolicy : std::uint8_t {
    PhysicalOnly = 0,
    LogicalOnly = 1,
    Either = 2,
};

/// Cycle policy enforced for a relation class. Containment and dependency-shaped relations
/// are acyclic; connectivity-shaped relations may legitimately form cycles.
enum class CycleRule : std::uint8_t {
    Acyclic = 0,
    CyclesAllowed = 1,
};

/// Complete, finite semantics of one relation class.
struct RelationRules {
    RelationClass relation = RelationClass::Unknown;
    Directionality direction = Directionality::Directed;
    LayerPolicy layer_policy = LayerPolicy::Either;
    CycleRule cycle_rule = CycleRule::Acyclic;
    bool self_edge_allowed = false;
    std::uint32_t from_mask = 0;
    std::uint32_t to_mask = 0;
};

/// Semantics of a relation class. Returns the Unknown-relation rules (all masks empty) for
/// RelationClass::Unknown.
[[nodiscard]] const RelationRules& relation_rules(RelationClass value) noexcept;
[[nodiscard]] bool relation_endpoints_permitted(RelationClass relation, NodeClass from, NodeClass to) noexcept;
[[nodiscard]] bool relation_layer_permitted(RelationClass relation, TopologyLayer layer) noexcept;

/// Deterministic rendering used by explanations and canonical dumps.
[[nodiscard]] std::string render_relation(RelationClass relation, NodeClass from_class, NodeClass to_class);

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_TYPES_HPP
