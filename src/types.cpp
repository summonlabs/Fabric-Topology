// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/types.hpp"

#include <array>
#include <string>

namespace fabric_topology {

namespace {

template <class Enum, std::size_t N>
[[nodiscard]] const char* name_of(const std::array<std::pair<Enum, const char*>, N>& table,
                                  Enum value) noexcept {
    for (const auto& entry : table) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    return nullptr;
}

template <class Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> value_of(const std::array<std::pair<Enum, const char*>, N>& table,
                                           std::string_view text) noexcept {
    for (const auto& entry : table) {
        if (text == entry.second) {
            return entry.first;
        }
    }
    return std::nullopt;
}

constexpr std::array<std::pair<EntityClass, const char*>, kEntityClassCount> kEntityClassNames{{
    {EntityClass::Unknown, "UNKNOWN"},
    {EntityClass::Fabric, "FABRIC"},
    {EntityClass::Site, "SITE"},
    {EntityClass::Device, "DEVICE"},
    {EntityClass::Switch, "SWITCH"},
    {EntityClass::Router, "ROUTER"},
    {EntityClass::Nic, "NIC"},
    {EntityClass::SmartNic, "SMARTNIC"},
    {EntityClass::Dpu, "DPU"},
    {EntityClass::Port, "PORT"},
    {EntityClass::LogicalPort, "LOGICAL_PORT"},
    {EntityClass::Link, "LINK"},
    {EntityClass::Endpoint, "ENDPOINT"},
    {EntityClass::ControlParticipant, "CONTROL_PARTICIPANT"},
}};

constexpr std::array<std::pair<NodeClass, const char*>, kNodeClassCount> kNodeClassNames{{
    {NodeClass::Unknown, "UNKNOWN"},
    {NodeClass::Site, "SITE"},
    {NodeClass::Fabric, "FABRIC"},
    {NodeClass::RackNetworkDomain, "RACK_NETWORK_DOMAIN"},
    {NodeClass::Switch, "SWITCH"},
    {NodeClass::Router, "ROUTER"},
    {NodeClass::Nic, "NIC"},
    {NodeClass::SmartNic, "SMARTNIC"},
    {NodeClass::DpuEndpoint, "DPU_ENDPOINT"},
    {NodeClass::PhysicalPort, "PHYSICAL_PORT"},
    {NodeClass::LogicalPort, "LOGICAL_PORT"},
    {NodeClass::FabricEndpoint, "FABRIC_ENDPOINT"},
    {NodeClass::LogicalEndpoint, "LOGICAL_ENDPOINT"},
}};

constexpr std::array<std::pair<RelationClass, const char*>, kRelationClassCount> kRelationNames{{
    {RelationClass::Unknown, "UNKNOWN"},
    {RelationClass::ConnectedTo, "CONNECTED_TO"},
    {RelationClass::AttachedTo, "ATTACHED_TO"},
    {RelationClass::Contains, "CONTAINS"},
    {RelationClass::MemberOf, "MEMBER_OF"},
    {RelationClass::UplinkTo, "UPLINK_TO"},
    {RelationClass::DownlinkTo, "DOWNLINK_TO"},
    {RelationClass::PeersWith, "PEERS_WITH"},
    {RelationClass::BackedBy, "BACKED_BY"},
    {RelationClass::TunneledOver, "TUNNELED_OVER"},
    {RelationClass::LogicallyConnectedTo, "LOGICALLY_CONNECTED_TO"},
    {RelationClass::HostedBy, "HOSTED_BY"},
    {RelationClass::PresentsEndpoint, "PRESENTS_ENDPOINT"},
    {RelationClass::FabricMembership, "FABRIC_MEMBERSHIP"},
}};

constexpr std::array<std::pair<TopologyTier, const char*>, kTopologyTierCount> kTierNames{{
    {TopologyTier::Unspecified, "UNSPECIFIED"},
    {TopologyTier::Endpoint, "ENDPOINT"},
    {TopologyTier::Access, "ACCESS"},
    {TopologyTier::TopOfRack, "TOP_OF_RACK"},
    {TopologyTier::Leaf, "LEAF"},
    {TopologyTier::Spine, "SPINE"},
    {TopologyTier::SuperSpine, "SUPER_SPINE"},
    {TopologyTier::Border, "BORDER"},
    {TopologyTier::Gateway, "GATEWAY"},
    {TopologyTier::SiteEdge, "SITE_EDGE"},
    {TopologyTier::InterSite, "INTER_SITE"},
    {TopologyTier::OpticalInterconnect, "OPTICAL_INTERCONNECT"},
}};

constexpr std::array<std::pair<ScopeKind, const char*>, kScopeKindCount> kScopeKindNames{{
    {ScopeKind::Fabric, "FABRIC"},
    {ScopeKind::Site, "SITE"},
    {ScopeKind::AdministrativeDomain, "ADMINISTRATIVE_DOMAIN"},
    {ScopeKind::PhysicalDomain, "PHYSICAL_DOMAIN"},
    {ScopeKind::LogicalDomain, "LOGICAL_DOMAIN"},
    {ScopeKind::ControlPlaneDomain, "CONTROL_PLANE_DOMAIN"},
}};

constexpr std::array<std::pair<LifecycleState, const char*>, kLifecycleStateCount> kLifecycleNames{{
    {LifecycleState::Unknown, "UNKNOWN"},
    {LifecycleState::Current, "CURRENT"},
    {LifecycleState::RevalidationRequired, "REVALIDATION_REQUIRED"},
    {LifecycleState::Superseded, "SUPERSEDED"},
    {LifecycleState::Retired, "RETIRED"},
    {LifecycleState::Conflicted, "CONFLICTED"},
}};

constexpr std::array<std::pair<PublicationMode, const char*>, kPublicationModeCount> kPublicationModeNames{{
    {PublicationMode::Incremental, "INCREMENTAL"},
    {PublicationMode::AuthoritativeSnapshot, "AUTHORITATIVE_SNAPSHOT"},
    {PublicationMode::PartialObservation, "PARTIAL_OBSERVATION"},
}};

constexpr std::array<std::pair<PublicationType, const char*>, kPublicationTypeCount> kPublicationTypeNames{{
    {PublicationType::Real, "REAL"},
    {PublicationType::Synthetic, "SYNTHETIC"},
    {PublicationType::Unsupported, "UNSUPPORTED"},
}};

constexpr std::array<std::pair<DiscoverySource, const char*>, kDiscoverySourceCount> kDiscoverySourceNames{{
    {DiscoverySource::Unknown, "UNKNOWN"},
    {DiscoverySource::FabricRegistry, "FABRIC_REGISTRY"},
    {DiscoverySource::HostDiscovery, "HOST_DISCOVERY"},
    {DiscoverySource::ControllerPublication, "CONTROLLER_PUBLICATION"},
    {DiscoverySource::OperatorDeclaration, "OPERATOR_DECLARATION"},
    {DiscoverySource::SyntheticGenerator, "SYNTHETIC_GENERATOR"},
    {DiscoverySource::PersistenceRecovery, "PERSISTENCE_RECOVERY"},
}};

constexpr std::uint32_t bit(NodeClass value) noexcept {
    return 1U << static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t mask_of(std::initializer_list<NodeClass> classes) noexcept {
    std::uint32_t result = 0;
    for (NodeClass value : classes) {
        result |= bit(value);
    }
    return result;
}

constexpr std::uint32_t kPortish =
    bit(NodeClass::PhysicalPort) | bit(NodeClass::LogicalPort) | bit(NodeClass::FabricEndpoint) |
    bit(NodeClass::LogicalEndpoint);

constexpr std::uint32_t kSwitchish = bit(NodeClass::Switch) | bit(NodeClass::Router);

constexpr std::uint32_t kEndpointish = bit(NodeClass::Nic) | bit(NodeClass::SmartNic) |
                                       bit(NodeClass::DpuEndpoint) | bit(NodeClass::PhysicalPort) |
                                       bit(NodeClass::LogicalPort);

constexpr std::uint32_t kContainerish = bit(NodeClass::Site) | bit(NodeClass::Fabric) |
                                        bit(NodeClass::RackNetworkDomain) | bit(NodeClass::Switch) |
                                        bit(NodeClass::Router);

constexpr std::uint32_t kAllNodeClasses = 0x1FFFU;  // bits 0..12 = 13 node classes

constexpr std::uint32_t kMemberFrom =
    bit(NodeClass::Site) | bit(NodeClass::RackNetworkDomain) | bit(NodeClass::Switch) |
    bit(NodeClass::Router) | bit(NodeClass::Nic) | bit(NodeClass::SmartNic) |
    bit(NodeClass::DpuEndpoint) | bit(NodeClass::PhysicalPort) | bit(NodeClass::LogicalPort) |
    bit(NodeClass::FabricEndpoint) | bit(NodeClass::LogicalEndpoint);

constexpr std::uint32_t kMemberTo =
    bit(NodeClass::Fabric) | bit(NodeClass::Site) | bit(NodeClass::RackNetworkDomain);

constexpr std::uint32_t kBackedFrom =
    bit(NodeClass::LogicalPort) | bit(NodeClass::LogicalEndpoint) | bit(NodeClass::FabricEndpoint);

constexpr std::uint32_t kBackedTo =
    bit(NodeClass::LogicalPort) | bit(NodeClass::PhysicalPort) | bit(NodeClass::Nic) |
    bit(NodeClass::SmartNic) | bit(NodeClass::DpuEndpoint) | bit(NodeClass::Switch) |
    bit(NodeClass::Router) | bit(NodeClass::FabricEndpoint);

constexpr std::uint32_t kPresentFrom =
    bit(NodeClass::Nic) | bit(NodeClass::SmartNic) | bit(NodeClass::DpuEndpoint) |
    bit(NodeClass::Switch) | bit(NodeClass::Router) | bit(NodeClass::PhysicalPort) |
    bit(NodeClass::LogicalPort);

constexpr std::uint32_t kPresentTo =
    bit(NodeClass::PhysicalPort) | bit(NodeClass::LogicalPort) | bit(NodeClass::FabricEndpoint) |
    bit(NodeClass::LogicalEndpoint);

constexpr std::uint32_t kHostedFrom = bit(NodeClass::Nic) | bit(NodeClass::SmartNic) |
                                      bit(NodeClass::DpuEndpoint) | bit(NodeClass::PhysicalPort) |
                                      bit(NodeClass::Switch);

constexpr std::uint32_t kHostedTo = bit(NodeClass::Switch) | bit(NodeClass::Router) |
                                    bit(NodeClass::Nic) | bit(NodeClass::SmartNic) |
                                    bit(NodeClass::DpuEndpoint);

constexpr std::uint32_t kAttachedTo = bit(NodeClass::Switch) | bit(NodeClass::Router) |
                                      bit(NodeClass::PhysicalPort) | bit(NodeClass::LogicalPort);

constexpr std::uint32_t kFabricMembershipFrom = bit(NodeClass::Site) |
                                                bit(NodeClass::RackNetworkDomain) |
                                                bit(NodeClass::Switch) | bit(NodeClass::Router);

constexpr std::array<RelationRules, kRelationClassCount> kRelationRules{{
    // Unknown: no endpoints are ever permitted.
    RelationRules{RelationClass::Unknown, Directionality::Directed, LayerPolicy::Either,
                  CycleRule::Acyclic, false, 0, 0},
    // CONNECTED_TO: undirected structural connectivity between ports/endpoints. Legal cycles.
    RelationRules{RelationClass::ConnectedTo, Directionality::Undirected, LayerPolicy::PhysicalOnly,
                  CycleRule::CyclesAllowed, false, kPortish, kPortish},
    // ATTACHED_TO: directed attachment of a network endpoint to a switch/router/port.
    RelationRules{RelationClass::AttachedTo, Directionality::Directed, LayerPolicy::PhysicalOnly,
                  CycleRule::CyclesAllowed, false, kEndpointish, kAttachedTo},
    // CONTAINS: directed containment. Must stay acyclic.
    RelationRules{RelationClass::Contains, Directionality::Directed, LayerPolicy::Either,
                  CycleRule::Acyclic, false, kContainerish, kAllNodeClasses},
    // MEMBER_OF: directed membership. Must stay acyclic.
    RelationRules{RelationClass::MemberOf, Directionality::Directed, LayerPolicy::Either,
                  CycleRule::Acyclic, false, kMemberFrom, kMemberTo},
    // UPLINK_TO / DOWNLINK_TO: directional labels over structural connectivity.
    RelationRules{RelationClass::UplinkTo, Directionality::Directed, LayerPolicy::PhysicalOnly,
                  CycleRule::CyclesAllowed, false, kSwitchish | bit(NodeClass::PhysicalPort),
                  kSwitchish | bit(NodeClass::PhysicalPort)},
    RelationRules{RelationClass::DownlinkTo, Directionality::Directed, LayerPolicy::PhysicalOnly,
                  CycleRule::CyclesAllowed, false, kSwitchish | bit(NodeClass::PhysicalPort),
                  kSwitchish | bit(NodeClass::PhysicalPort)},
    // PEERS_WITH: undirected logical peering adjacency between switches/routers.
    RelationRules{RelationClass::PeersWith, Directionality::Undirected, LayerPolicy::LogicalOnly,
                  CycleRule::CyclesAllowed, false, kSwitchish, kSwitchish},
    // BACKED_BY: directed logical dependency on supporting structure. Must stay acyclic.
    RelationRules{RelationClass::BackedBy, Directionality::Directed, LayerPolicy::LogicalOnly,
                  CycleRule::Acyclic, false, kBackedFrom, kBackedTo},
    // TUNNELED_OVER: directed logical carriage. Overlay stacks may legitimately cycle.
    RelationRules{RelationClass::TunneledOver, Directionality::Directed, LayerPolicy::LogicalOnly,
                  CycleRule::CyclesAllowed, false, kPortish, kPortish | kSwitchish},
    // LOGICALLY_CONNECTED_TO: undirected logical adjacency.
    RelationRules{RelationClass::LogicallyConnectedTo, Directionality::Undirected,
                  LayerPolicy::LogicalOnly, CycleRule::CyclesAllowed, false, kPortish, kPortish},
    // HOSTED_BY: directed hosting. Must stay acyclic.
    RelationRules{RelationClass::HostedBy, Directionality::Directed, LayerPolicy::PhysicalOnly,
                  CycleRule::Acyclic, false, kHostedFrom, kHostedTo},
    // PRESENTS_ENDPOINT: directed endpoint presentation. Must stay acyclic.
    RelationRules{RelationClass::PresentsEndpoint, Directionality::Directed,
                  LayerPolicy::PhysicalOnly, CycleRule::Acyclic, false, kPresentFrom, kPresentTo},
    // FABRIC_MEMBERSHIP: directed membership in a fabric. Must stay acyclic.
    RelationRules{RelationClass::FabricMembership, Directionality::Directed,
                  LayerPolicy::LogicalOnly, CycleRule::Acyclic, false, kFabricMembershipFrom,
                  bit(NodeClass::Fabric)},
}};

constexpr RelationRules kUnknownRules{RelationClass::Unknown, Directionality::Directed,
                                      LayerPolicy::Either, CycleRule::Acyclic, false, 0, 0};

}  // namespace

const char* to_string(EntityClass value) noexcept {
    const char* name = name_of(kEntityClassNames, value);
    return name != nullptr ? name : "UNKNOWN";
}

std::optional<EntityClass> entity_class_from_string(std::string_view text) noexcept {
    return value_of(kEntityClassNames, text);
}

const char* to_string(NodeClass value) noexcept {
    const char* name = name_of(kNodeClassNames, value);
    return name != nullptr ? name : "UNKNOWN";
}

std::optional<NodeClass> node_class_from_string(std::string_view text) noexcept {
    return value_of(kNodeClassNames, text);
}

EntityClass node_class_entity_class(NodeClass value) noexcept {
    switch (value) {
        case NodeClass::Site: return EntityClass::Site;
        case NodeClass::Fabric: return EntityClass::Fabric;
        case NodeClass::RackNetworkDomain: return EntityClass::Site;
        case NodeClass::Switch: return EntityClass::Switch;
        case NodeClass::Router: return EntityClass::Router;
        case NodeClass::Nic: return EntityClass::Nic;
        case NodeClass::SmartNic: return EntityClass::SmartNic;
        case NodeClass::DpuEndpoint: return EntityClass::Dpu;
        case NodeClass::PhysicalPort: return EntityClass::Port;
        case NodeClass::LogicalPort: return EntityClass::LogicalPort;
        case NodeClass::FabricEndpoint: return EntityClass::Endpoint;
        case NodeClass::LogicalEndpoint: return EntityClass::Endpoint;
        case NodeClass::Unknown: break;
    }
    return EntityClass::Unknown;
}

std::uint32_t node_class_bit(NodeClass value) noexcept { return bit(value); }

bool node_class_in_mask(std::uint32_t mask, NodeClass value) noexcept {
    return value != NodeClass::Unknown && (mask & bit(value)) != 0U;
}

bool node_class_is_endpoint(NodeClass value) noexcept { return value != NodeClass::Unknown; }

const char* to_string(RelationClass value) noexcept {
    const char* name = name_of(kRelationNames, value);
    return name != nullptr ? name : "UNKNOWN";
}

std::optional<RelationClass> relation_class_from_string(std::string_view text) noexcept {
    return value_of(kRelationNames, text);
}

const char* to_string(Directionality value) noexcept {
    return value == Directionality::Undirected ? "UNDIRECTED" : "DIRECTED";
}

const char* to_string(TopologyLayer value) noexcept {
    return value == TopologyLayer::Logical ? "LOGICAL" : "PHYSICAL";
}

std::optional<TopologyLayer> topology_layer_from_string(std::string_view text) noexcept {
    if (text == "PHYSICAL") {
        return TopologyLayer::Physical;
    }
    if (text == "LOGICAL") {
        return TopologyLayer::Logical;
    }
    return std::nullopt;
}

const char* to_string(TopologyTier value) noexcept {
    const char* name = name_of(kTierNames, value);
    return name != nullptr ? name : "UNSPECIFIED";
}

std::optional<TopologyTier> topology_tier_from_string(std::string_view text) noexcept {
    return value_of(kTierNames, text);
}

const char* to_string(ScopeKind value) noexcept {
    const char* name = name_of(kScopeKindNames, value);
    return name != nullptr ? name : "ADMINISTRATIVE_DOMAIN";
}

std::optional<ScopeKind> scope_kind_from_string(std::string_view text) noexcept {
    return value_of(kScopeKindNames, text);
}

const char* to_string(LifecycleState value) noexcept {
    const char* name = name_of(kLifecycleNames, value);
    return name != nullptr ? name : "UNKNOWN";
}

std::optional<LifecycleState> lifecycle_state_from_string(std::string_view text) noexcept {
    return value_of(kLifecycleNames, text);
}

bool lifecycle_is_queryable(LifecycleState value) noexcept {
    return value == LifecycleState::Current || value == LifecycleState::RevalidationRequired ||
           value == LifecycleState::Conflicted;
}

const char* to_string(PublicationMode value) noexcept {
    const char* name = name_of(kPublicationModeNames, value);
    return name != nullptr ? name : "INCREMENTAL";
}

std::optional<PublicationMode> publication_mode_from_string(std::string_view text) noexcept {
    return value_of(kPublicationModeNames, text);
}

const char* to_string(PublicationType value) noexcept {
    const char* name = name_of(kPublicationTypeNames, value);
    return name != nullptr ? name : "UNSUPPORTED";
}

std::optional<PublicationType> publication_type_from_string(std::string_view text) noexcept {
    return value_of(kPublicationTypeNames, text);
}

const char* to_string(DiscoverySource value) noexcept {
    const char* name = name_of(kDiscoverySourceNames, value);
    return name != nullptr ? name : "UNKNOWN";
}

std::optional<DiscoverySource> discovery_source_from_string(std::string_view text) noexcept {
    return value_of(kDiscoverySourceNames, text);
}

const RelationRules& relation_rules(RelationClass value) noexcept {
    const auto index = static_cast<std::size_t>(value);
    if (index >= kRelationRules.size()) {
        return kUnknownRules;
    }
    return kRelationRules[index];
}

bool relation_endpoints_permitted(RelationClass relation, NodeClass from, NodeClass to) noexcept {
    const RelationRules& rules = relation_rules(relation);
    return node_class_in_mask(rules.from_mask, from) && node_class_in_mask(rules.to_mask, to);
}

bool relation_layer_permitted(RelationClass relation, TopologyLayer layer) noexcept {
    switch (relation_rules(relation).layer_policy) {
        case LayerPolicy::PhysicalOnly:
            return layer == TopologyLayer::Physical;
        case LayerPolicy::LogicalOnly:
            return layer == TopologyLayer::Logical;
        case LayerPolicy::Either:
            return true;
    }
    return false;
}

std::string render_relation(RelationClass relation, NodeClass from_class, NodeClass to_class) {
    std::string out;
    out.reserve(64);
    out += to_string(from_class);
    out += " -[";
    out += to_string(relation);
    out += "]-> ";
    out += to_string(to_class);
    return out;
}

}  // namespace fabric_topology
