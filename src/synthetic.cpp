// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Synthetic topology backend. Every fact produced here is tagged SYNTHETIC: these are
// generated scenarios that exercise topology semantics which cannot be observed on one
// workstation. Nothing in this file is physical evidence.

#include "fabric_topology/synthetic.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "graph_state.hpp"

namespace fabric_topology {

namespace {

struct Builder {
    const SyntheticOptions& options;
    SyntheticTopology topology;
    std::size_t entity_counter = 0;

    explicit Builder(const SyntheticOptions& options_in) : options(options_in) {
        topology.scenario = options.scenario;
        topology.publication.id = PublicationId::from_trusted("pub_" + std::string(to_string(options.scenario)));
        topology.publication.mode = PublicationMode::AuthoritativeSnapshot;
        topology.publication.type = PublicationType::Synthetic;
        topology.publication.source = DiscoverySource::SyntheticGenerator;
        topology.publication.scope_kind = options.scope_kind;
        if (!options.domain.empty()) {
            topology.publication.domain = options.domain;
        }
        topology.publication.note = "synthetic scenario ";
        topology.publication.note += to_string(options.scenario);
    }

    [[nodiscard]] std::string next_entity_id(const char* kind) {
        ++entity_counter;
        std::string id = options.entity_prefix;
        id += '-';
        id += kind;
        id += '-';
        id += std::to_string(entity_counter);
        return id;
    }

    void add_directory_entry(const std::string& entity_id, EntityClass entity_class) {
        EntityRecord record;
        record.entity_id = entity_id;
        record.entity_class = entity_class;
        record.generation = EntityGeneration{1};
        topology.directory_entries.push_back(std::move(record));
    }

    TopologyNodeId add_node(const std::string& entity_id, NodeClass node_class, TopologyTier tier,
                            TopologyDomainId domain) {
        add_directory_entry(entity_id, node_class_entity_class(node_class));
        ObservedNode observed;
        observed.entity_id = entity_id;
        observed.node_class = node_class;
        observed.tier = tier;
        observed.domain = std::move(domain);
        observed.entity_generation = EntityGeneration{1};
        observed.node_id = internal::derive_node_id(observed.domain, entity_id);
        topology.publication.nodes.push_back(std::move(observed));
        return topology.publication.nodes.back().node_id;
    }

    [[nodiscard]] static std::optional<TopologyLayer> effective_layer(
        RelationClass relation, std::optional<TopologyLayer> layer) {
        if (layer.has_value()) {
            return layer;
        }
        switch (relation_rules(relation).layer_policy) {
            case LayerPolicy::PhysicalOnly:
                return TopologyLayer::Physical;
            case LayerPolicy::LogicalOnly:
                return TopologyLayer::Logical;
            case LayerPolicy::Either:
                return TopologyLayer::Physical;
        }
        return std::nullopt;
    }

    void add_edge(RelationClass relation, const TopologyNodeId& from, const TopologyNodeId& to,
                  std::optional<TopologyLayer> layer, const TopologyDomainId& domain) {
        ObservedEdge edge;
        edge.relation = relation;
        layer = effective_layer(relation, layer);
        edge.from = from;
        edge.to = to;
        edge.layer = layer;
        edge.domain = domain;
        edge.evidence_generation = EvidenceGeneration{1};
        edge.relationship_id = RelationshipId::from_trusted(
            internal::derive_relationship_id(make_edge_key(relation, from, to, domain)));
        topology.publication.edges.push_back(std::move(edge));
    }

    [[nodiscard]] Publication& secondary(std::size_t index, const char* suffix) {
        while (topology.secondary_publications.size() <= index) {
            Publication publication;
            publication.mode = PublicationMode::AuthoritativeSnapshot;
            publication.type = PublicationType::Synthetic;
            publication.source = DiscoverySource::SyntheticGenerator;
            publication.scope_kind = options.scope_kind;
            publication.note = "synthetic scenario ";
            publication.note += to_string(options.scenario);
            topology.secondary_publications.push_back(std::move(publication));
        }
        Publication& publication = topology.secondary_publications[index];
        if (publication.id.empty()) {
            publication.id = PublicationId::from_trusted(std::string("pub_") + to_string(options.scenario) +
                                                         "_" + suffix);
        }
        return publication;
    }

    void secondary_node(Publication& publication, const std::string& entity_id, NodeClass node_class,
                        TopologyTier tier, const TopologyDomainId& domain) {
        add_directory_entry(entity_id, node_class_entity_class(node_class));
        ObservedNode observed;
        observed.entity_id = entity_id;
        observed.node_class = node_class;
        observed.tier = tier;
        observed.domain = domain;
        observed.entity_generation = EntityGeneration{1};
        observed.node_id = internal::derive_node_id(domain, entity_id);
        publication.nodes.push_back(std::move(observed));
    }

    void secondary_edge(Publication& publication, RelationClass relation, const TopologyNodeId& from,
                        const TopologyNodeId& to, std::optional<TopologyLayer> layer,
                        const TopologyDomainId& domain) {
        ObservedEdge edge;
        edge.relation = relation;
        edge.from = from;
        edge.to = to;
        edge.layer = effective_layer(relation, layer);
        edge.domain = domain;
        edge.evidence_generation = EvidenceGeneration{1};
        edge.relationship_id = RelationshipId::from_trusted(
            internal::derive_relationship_id(make_edge_key(relation, from, to, domain)));
        publication.edges.push_back(std::move(edge));
    }
};

[[nodiscard]] std::size_t scale_or(std::size_t value, std::size_t fallback) {
    return value == 0 ? fallback : value;
}

void build_switch_fabric(Builder& builder, const std::string& fabric_entity, TopologyDomainId domain,
                         std::size_t switch_count, std::size_t hosts_per_switch,
                         TopologyTier switch_tier) {
    const TopologyNodeId fabric =
        builder.add_node(fabric_entity, NodeClass::Fabric, TopologyTier::Unspecified, domain);
    for (std::size_t s = 0; s < switch_count; ++s) {
        const std::string switch_entity = fabric_entity + "-sw" + std::to_string(s);
        const TopologyNodeId sw = builder.add_node(switch_entity, NodeClass::Switch, switch_tier, domain);
        builder.add_edge(RelationClass::MemberOf, sw, fabric, std::nullopt, domain);
        for (std::size_t h = 0; h < hosts_per_switch; ++h) {
            const std::string nic_entity =
                switch_entity + "-nic" + std::to_string(h);
            const TopologyNodeId nic =
                builder.add_node(nic_entity, NodeClass::Nic, TopologyTier::Endpoint, domain);
            const TopologyNodeId nic_port = builder.add_node(
                nic_entity + "-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint, domain);
            const TopologyNodeId sw_port = builder.add_node(
                switch_entity + "-p" + std::to_string(h), NodeClass::PhysicalPort, switch_tier, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, nic, nic_port,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw, sw_port,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::ConnectedTo, nic_port, sw_port, std::nullopt, domain);
        }
    }
}

}  // namespace

const char* to_string(SyntheticScenario value) noexcept {
    switch (value) {
        case SyntheticScenario::SingleSwitch: return "single_switch";
        case SyntheticScenario::DualSwitchRedundancy: return "dual_switch_redundancy";
        case SyntheticScenario::LeafSpine: return "leaf_spine";
        case SyntheticScenario::MultiTierClos: return "multi_tier_clos";
        case SyntheticScenario::RackToLeaf: return "rack_to_leaf";
        case SyntheticScenario::MultipleFabrics: return "multiple_fabrics";
        case SyntheticScenario::MultiSite: return "multi_site";
        case SyntheticScenario::LogicalOverlay: return "logical_overlay";
        case SyntheticScenario::PhysicalAndLogical: return "physical_and_logical";
        case SyntheticScenario::Partitioned: return "partitioned";
        case SyntheticScenario::DeviceReplacement: return "device_replacement";
        case SyntheticScenario::AttachmentMove: return "attachment_move";
        case SyntheticScenario::AsymmetricPublisherScopes: return "asymmetric_publisher_scopes";
        case SyntheticScenario::StaleSnapshot: return "stale_snapshot";
        case SyntheticScenario::LargeScale: return "large_scale";
    }
    return "single_switch";
}

std::optional<SyntheticScenario> synthetic_scenario_from_string(std::string_view text) noexcept {
    for (std::uint8_t raw = 0; raw < kSyntheticScenarioCount; ++raw) {
        const auto scenario = static_cast<SyntheticScenario>(raw);
        if (text == to_string(scenario)) {
            return scenario;
        }
    }
    return std::nullopt;
}

std::vector<SyntheticScenario> synthetic_scenarios() {
    std::vector<SyntheticScenario> result;
    result.reserve(kSyntheticScenarioCount);
    for (std::uint8_t raw = 0; raw < kSyntheticScenarioCount; ++raw) {
        result.push_back(static_cast<SyntheticScenario>(raw));
    }
    return result;
}

std::string SyntheticTopology::render() const {
    std::string out;
    out.reserve(256);
    out += "scenario=";
    out += to_string(scenario);
    out += "\nname=";
    out += name;
    out += "\ndescription=";
    out += description;
    out += "\npublication=";
    out += publication.id.to_string();
    out += "\nnodes=";
    out += std::to_string(publication.nodes.size());
    out += "\nedges=";
    out += std::to_string(publication.edges.size());
    out += "\nentities=";
    out += std::to_string(directory_entries.size());
    out += "\nsecondary_publications=";
    out += std::to_string(secondary_publications.size());
    out += "\nevidence_type=";
    out += to_string(publication.type);
    return out;
}

SyntheticTopology build_synthetic_topology(const SyntheticOptions& options_in) {
    SyntheticOptions options = options_in;
    if (options.domain.empty()) {
        options.domain = TopologyDomainId::from_trusted("dom-synthetic");
    }
    if (options.publisher.empty()) {
        options.publisher = PublisherId::from_trusted("pub-synthetic");
    }

    Builder builder(options);
    SyntheticTopology& topology = builder.topology;
    topology.scenario = options.scenario;
    const TopologyDomainId domain = options.domain;

    switch (options.scenario) {
        case SyntheticScenario::SingleSwitch: {
            topology.name = "single-switch";
            topology.description =
                "One fabric with one switch, two dual-homed hosts and their physical ports.";
            build_switch_fabric(builder, "syn-single-fabric", domain, 1, 2, TopologyTier::Leaf);
            break;
        }
        case SyntheticScenario::DualSwitchRedundancy: {
            topology.name = "dual-switch-redundancy";
            topology.description =
                "Two switches in one fabric; a host with two NICs, each attached to a different "
                "switch, plus a switch-to-switch interconnect.";
            const TopologyNodeId fabric = builder.add_node("syn-dual-fabric", NodeClass::Fabric,
                                                           TopologyTier::Unspecified, domain);
            const TopologyNodeId sw_a = builder.add_node("syn-dual-sw-a", NodeClass::Switch,
                                                         TopologyTier::Leaf, domain);
            const TopologyNodeId sw_b = builder.add_node("syn-dual-sw-b", NodeClass::Switch,
                                                         TopologyTier::Leaf, domain);
            builder.add_edge(RelationClass::MemberOf, sw_a, fabric, std::nullopt, domain);
            builder.add_edge(RelationClass::MemberOf, sw_b, fabric, std::nullopt, domain);
            const TopologyNodeId sw_a_port =
                builder.add_node("syn-dual-sw-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf, domain);
            const TopologyNodeId sw_b_port =
                builder.add_node("syn-dual-sw-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf, domain);
            const TopologyNodeId fabric_port_a =
                builder.add_node("syn-dual-sw-a-p1", NodeClass::PhysicalPort, TopologyTier::Leaf, domain);
            const TopologyNodeId fabric_port_b =
                builder.add_node("syn-dual-sw-b-p1", NodeClass::PhysicalPort, TopologyTier::Leaf, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw_a, sw_a_port,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw_b, sw_b_port,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::ConnectedTo, fabric_port_a, fabric_port_b, std::nullopt,
                             domain);
            for (std::size_t host = 0; host < 2; ++host) {
                const std::string nic_entity = "syn-dual-nic" + std::to_string(host);
                const TopologyNodeId nic =
                    builder.add_node(nic_entity, NodeClass::Nic, TopologyTier::Endpoint, domain);
                const TopologyNodeId nic_port = builder.add_node(
                    nic_entity + "-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint, domain);
                builder.add_edge(RelationClass::PresentsEndpoint, nic, nic_port,
                                 std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
                builder.add_edge(RelationClass::ConnectedTo, nic_port,
                                 host == 0 ? sw_a_port : sw_b_port, std::nullopt, domain);
            }
            break;
        }
        case SyntheticScenario::LeafSpine: {
            const std::size_t leaves = scale_or(options.scale, 4);
            const std::size_t spines = scale_or(options.tiers, 2);
            topology.name = "leaf-spine";
            topology.description = "Leaf-spine fabric: " + std::to_string(leaves) + " leaves, " +
                                   std::to_string(spines) + " spines, full bipartite mesh.";
            const TopologyNodeId fabric =
                builder.add_node("syn-ls-fabric", NodeClass::Fabric, TopologyTier::Unspecified, domain);
            std::vector<TopologyNodeId> leaf_ports;
            std::vector<TopologyNodeId> spine_ports;
            for (std::size_t s = 0; s < spines; ++s) {
                const std::string spine_entity = "syn-ls-spine" + std::to_string(s);
                const TopologyNodeId spine = builder.add_node(spine_entity, NodeClass::Switch,
                                                              TopologyTier::Spine, domain);
                builder.add_edge(RelationClass::MemberOf, spine, fabric, std::nullopt, domain);
                for (std::size_t l = 0; l < leaves; ++l) {
                    spine_ports.push_back(builder.add_node(
                        spine_entity + "-p" + std::to_string(l), NodeClass::PhysicalPort,
                        TopologyTier::Spine, domain));
                }
            }
            for (std::size_t l = 0; l < leaves; ++l) {
                const std::string leaf_entity = "syn-ls-leaf" + std::to_string(l);
                const TopologyNodeId leaf = builder.add_node(leaf_entity, NodeClass::Switch,
                                                             TopologyTier::Leaf, domain);
                builder.add_edge(RelationClass::MemberOf, leaf, fabric, std::nullopt, domain);
                for (std::size_t s = 0; s < spines; ++s) {
                    const TopologyNodeId leaf_port = builder.add_node(
                        leaf_entity + "-p" + std::to_string(s), NodeClass::PhysicalPort,
                        TopologyTier::Leaf, domain);
                    const TopologyNodeId spine_port = spine_ports[s * leaves + l];
                    builder.add_edge(RelationClass::ConnectedTo, leaf_port, spine_port, std::nullopt,
                                     domain);
                }
                const std::string host_entity = leaf_entity + "-host";
                const TopologyNodeId nic =
                    builder.add_node(host_entity, NodeClass::Nic, TopologyTier::Endpoint, domain);
                const TopologyNodeId nic_port = builder.add_node(
                    host_entity + "-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint, domain);
                const TopologyNodeId leaf_host_port = builder.add_node(
                    leaf_entity + "-hostport", NodeClass::PhysicalPort, TopologyTier::Leaf, domain);
                builder.add_edge(RelationClass::PresentsEndpoint, nic, nic_port,
                                 std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
                builder.add_edge(RelationClass::ConnectedTo, nic_port, leaf_host_port, std::nullopt,
                                 domain);
            }
            (void)leaf_ports;
            break;
        }
        case SyntheticScenario::MultiTierClos: {
            const std::size_t tiers = scale_or(options.tiers, 3);
            const std::size_t width = scale_or(options.scale, 2);
            topology.name = "multi-tier-clos";
            topology.description = "Clos fabric with " + std::to_string(tiers) + " tiers and width " +
                                   std::to_string(width) + ".";
            const TopologyNodeId fabric =
                builder.add_node("syn-clos-fabric", NodeClass::Fabric, TopologyTier::Unspecified, domain);
            std::vector<TopologyNodeId> previous_ports;
            for (std::size_t tier = 0; tier < tiers; ++tier) {
                const TopologyTier tier_kind = tier == 0 ? TopologyTier::Leaf
                                                : tier + 1 == tiers ? TopologyTier::SuperSpine
                                                                    : TopologyTier::Spine;
                std::vector<TopologyNodeId> current_ports;
                for (std::size_t index = 0; index < width; ++index) {
                    const std::string entity = "syn-clos-t" + std::to_string(tier) + "-" +
                                               std::to_string(index);
                    const TopologyNodeId node =
                        builder.add_node(entity, NodeClass::Switch, tier_kind, domain);
                    builder.add_edge(RelationClass::MemberOf, node, fabric, std::nullopt, domain);
                    for (std::size_t link = 0; link < width; ++link) {
                        current_ports.push_back(builder.add_node(
                            entity + "-p" + std::to_string(link), NodeClass::PhysicalPort, tier_kind,
                            domain));
                    }
                }
                if (!previous_ports.empty()) {
                    for (std::size_t index = 0; index < previous_ports.size(); ++index) {
                        builder.add_edge(RelationClass::ConnectedTo, previous_ports[index],
                                         current_ports[index % current_ports.size()], std::nullopt,
                                         domain);
                    }
                }
                previous_ports = current_ports;
            }
            break;
        }
        case SyntheticScenario::RackToLeaf: {
            const std::size_t racks = scale_or(options.scale, 3);
            const std::size_t hosts_per_rack = scale_or(options.tiers, 2);
            topology.name = "rack-to-leaf";
            topology.description = std::to_string(racks) + " racks, each with a top-of-rack switch " +
                                   "uplinked to a leaf switch, " + std::to_string(hosts_per_rack) +
                                   " hosts per rack.";
            const TopologyNodeId fabric = builder.add_node("syn-rack-fabric", NodeClass::Fabric,
                                                           TopologyTier::Unspecified, domain);
            const TopologyNodeId leaf =
                builder.add_node("syn-rack-leaf", NodeClass::Switch, TopologyTier::Leaf, domain);
            builder.add_edge(RelationClass::MemberOf, leaf, fabric, std::nullopt, domain);
            for (std::size_t r = 0; r < racks; ++r) {
                const std::string tor_entity = "syn-rack-tor" + std::to_string(r);
                const TopologyNodeId tor = builder.add_node(tor_entity, NodeClass::Switch,
                                                            TopologyTier::TopOfRack, domain);
                builder.add_edge(RelationClass::MemberOf, tor, fabric, std::nullopt, domain);
                const TopologyNodeId tor_uplink = builder.add_node(
                    tor_entity + "-uplink", NodeClass::PhysicalPort, TopologyTier::TopOfRack, domain);
                const TopologyNodeId leaf_downlink = builder.add_node(
                    "syn-rack-leaf-p" + std::to_string(r), NodeClass::PhysicalPort,
                    TopologyTier::Leaf, domain);
                builder.add_edge(RelationClass::ConnectedTo, tor_uplink, leaf_downlink, std::nullopt,
                                 domain);
                for (std::size_t h = 0; h < hosts_per_rack; ++h) {
                    const std::string nic_entity = tor_entity + "-nic" + std::to_string(h);
                    const TopologyNodeId nic =
                        builder.add_node(nic_entity, NodeClass::Nic, TopologyTier::Endpoint, domain);
                    const TopologyNodeId nic_port = builder.add_node(
                        nic_entity + "-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint, domain);
                    const TopologyNodeId tor_port = builder.add_node(
                        tor_entity + "-p" + std::to_string(h), NodeClass::PhysicalPort,
                        TopologyTier::TopOfRack, domain);
                    builder.add_edge(RelationClass::PresentsEndpoint, nic, nic_port,
                                     std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
                    builder.add_edge(RelationClass::ConnectedTo, nic_port, tor_port, std::nullopt,
                                     domain);
                }
            }
            break;
        }
        case SyntheticScenario::MultipleFabrics: {
            topology.name = "multiple-fabrics";
            topology.description =
                "Two independent fabric scopes; the second fabric is published separately.";
            build_switch_fabric(builder, "syn-mf-fabric-a", domain, 1, 1, TopologyTier::Leaf);
            Publication& second = builder.secondary(0, "fabric_b");
            second.domain = domain;
            builder.secondary_node(second, "syn-mf-fabric-b", NodeClass::Fabric,
                                   TopologyTier::Unspecified, domain);
            const TopologyNodeId fabric_b =
                internal::derive_node_id(domain, "syn-mf-fabric-b");
            builder.secondary_node(second, "syn-mf-sw-b", NodeClass::Switch, TopologyTier::Leaf,
                                   domain);
            const TopologyNodeId sw_b = internal::derive_node_id(domain, "syn-mf-sw-b");
            builder.secondary_edge(second, RelationClass::MemberOf, sw_b, fabric_b, std::nullopt,
                                   domain);
            break;
        }
        case SyntheticScenario::MultiSite: {
            topology.name = "multi-site";
            topology.description =
                "Two sites, each containing a fabric and a switch, joined by an inter-site link.";
            const TopologyNodeId site_a = builder.add_node("syn-ms-site-a", NodeClass::Site,
                                                           TopologyTier::SiteEdge, domain);
            const TopologyNodeId site_b = builder.add_node("syn-ms-site-b", NodeClass::Site,
                                                           TopologyTier::SiteEdge, domain);
            const TopologyNodeId sw_a = builder.add_node("syn-ms-sw-a", NodeClass::Switch,
                                                         TopologyTier::Border, domain);
            const TopologyNodeId sw_b = builder.add_node("syn-ms-sw-b", NodeClass::Switch,
                                                         TopologyTier::Border, domain);
            builder.add_edge(RelationClass::Contains, site_a, sw_a, std::nullopt, domain);
            builder.add_edge(RelationClass::Contains, site_b, sw_b, std::nullopt, domain);
            const TopologyNodeId port_a = builder.add_node("syn-ms-sw-a-p0", NodeClass::PhysicalPort,
                                                           TopologyTier::InterSite, domain);
            const TopologyNodeId port_b = builder.add_node("syn-ms-sw-b-p0", NodeClass::PhysicalPort,
                                                           TopologyTier::InterSite, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw_a, port_a,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw_b, port_b,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt, domain);
            break;
        }
        case SyntheticScenario::LogicalOverlay:
        case SyntheticScenario::PhysicalAndLogical: {
            topology.name = options.scenario == SyntheticScenario::LogicalOverlay
                                ? "logical-overlay"
                                : "physical-and-logical";
            topology.description =
                "Physical switch ports carrying logical ports that are backed by the physical "
                "structure and joined by a logical adjacency and an overlay tunnel.";
            const TopologyNodeId fabric = builder.add_node("syn-lo-fabric", NodeClass::Fabric,
                                                           TopologyTier::Unspecified, domain);
            const TopologyNodeId sw_a =
                builder.add_node("syn-lo-sw-a", NodeClass::Switch, TopologyTier::Leaf, domain);
            const TopologyNodeId sw_b =
                builder.add_node("syn-lo-sw-b", NodeClass::Switch, TopologyTier::Leaf, domain);
            builder.add_edge(RelationClass::MemberOf, sw_a, fabric, std::nullopt, domain);
            builder.add_edge(RelationClass::MemberOf, sw_b, fabric, std::nullopt, domain);
            const TopologyNodeId phys_a = builder.add_node("syn-lo-sw-a-p0", NodeClass::PhysicalPort,
                                                           TopologyTier::Leaf, domain);
            const TopologyNodeId phys_b = builder.add_node("syn-lo-sw-b-p0", NodeClass::PhysicalPort,
                                                           TopologyTier::Leaf, domain);
            const TopologyNodeId logical_a = builder.add_node("syn-lo-a-lp0", NodeClass::LogicalPort,
                                                              TopologyTier::Leaf, domain);
            const TopologyNodeId logical_b = builder.add_node("syn-lo-b-lp0", NodeClass::LogicalPort,
                                                              TopologyTier::Leaf, domain);
            const TopologyNodeId tunnel_a = builder.add_node(
                "syn-lo-a-tun0", NodeClass::LogicalEndpoint, TopologyTier::Leaf, domain);
            const TopologyNodeId tunnel_b = builder.add_node(
                "syn-lo-b-tun0", NodeClass::LogicalEndpoint, TopologyTier::Leaf, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw_a, phys_a,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw_b, phys_b,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::ConnectedTo, phys_a, phys_b, std::nullopt, domain);
            builder.add_edge(RelationClass::BackedBy, logical_a, phys_a, std::nullopt, domain);
            builder.add_edge(RelationClass::BackedBy, logical_b, phys_b, std::nullopt, domain);
            builder.add_edge(RelationClass::LogicallyConnectedTo, logical_a, logical_b, std::nullopt,
                             domain);
            builder.add_edge(RelationClass::BackedBy, tunnel_a, logical_a, std::nullopt, domain);
            builder.add_edge(RelationClass::BackedBy, tunnel_b, logical_b, std::nullopt, domain);
            builder.add_edge(RelationClass::TunneledOver, tunnel_a, tunnel_b, std::nullopt, domain);
            break;
        }
        case SyntheticScenario::Partitioned: {
            topology.name = "partitioned";
            topology.description =
                "Two disconnected components in one fabric scope: honest incomplete topology, "
                "not a graph that has been artificially connected.";
            build_switch_fabric(builder, "syn-part-fabric-a", domain, 1, 1, TopologyTier::Leaf);
            const TopologyNodeId fabric =
                internal::derive_node_id(domain, "syn-part-fabric-a");
            const TopologyNodeId isolated =
                builder.add_node("syn-part-sw-isolated", NodeClass::Switch, TopologyTier::Leaf, domain);
            builder.add_edge(RelationClass::MemberOf, isolated, fabric, std::nullopt, domain);
            break;
        }
        case SyntheticScenario::DeviceReplacement: {
            topology.name = "device-replacement";
            topology.description =
                "Device gen-1 with two attached host NICs; the registry supersedes it with gen-2 "
                "and the second publication binds the replacement.";
            const TopologyNodeId sw = builder.add_node("syn-repl-sw", NodeClass::Switch,
                                                       TopologyTier::Leaf, domain);
            const TopologyNodeId sw_port = builder.add_node("syn-repl-sw-p0", NodeClass::PhysicalPort,
                                                            TopologyTier::Leaf, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw, sw_port,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            for (std::size_t i = 0; i < 2; ++i) {
                const std::string nic_entity = "syn-repl-nic" + std::to_string(i);
                const TopologyNodeId nic =
                    builder.add_node(nic_entity, NodeClass::Nic, TopologyTier::Endpoint, domain);
                const TopologyNodeId nic_port = builder.add_node(
                    nic_entity + "-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint, domain);
                builder.add_edge(RelationClass::PresentsEndpoint, nic, nic_port,
                                 std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
                builder.add_edge(RelationClass::ConnectedTo, nic_port, sw_port, std::nullopt, domain);
            }
            Publication& replacement = builder.secondary(0, "replacement");
            replacement.domain = domain;
            replacement.mode = PublicationMode::Incremental;
            builder.secondary_node(replacement, "syn-repl-sw-gen2", NodeClass::Switch,
                                   TopologyTier::Leaf, domain);
            const TopologyNodeId sw_gen2 = internal::derive_node_id(domain, "syn-repl-sw-gen2");
            const TopologyNodeId port_gen2 = internal::derive_node_id(domain, "syn-repl-sw-gen2-p0");
            builder.secondary_node(replacement, "syn-repl-sw-gen2-p0", NodeClass::PhysicalPort,
                                   TopologyTier::Leaf, domain);
            builder.secondary_edge(replacement, RelationClass::PresentsEndpoint, sw_gen2, port_gen2,
                                   std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            break;
        }
        case SyntheticScenario::AttachmentMove: {
            topology.name = "attachment-move";
            topology.description =
                "NIC port attached to switch A port 0; the second publication is an attachment "
                "move to switch B port 0 under the same attachment slot.";
            const TopologyNodeId sw_a = builder.add_node("syn-move-sw-a", NodeClass::Switch,
                                                         TopologyTier::Leaf, domain);
            const TopologyNodeId sw_b = builder.add_node("syn-move-sw-b", NodeClass::Switch,
                                                         TopologyTier::Leaf, domain);
            const TopologyNodeId port_a = builder.add_node("syn-move-sw-a-p0", NodeClass::PhysicalPort,
                                                           TopologyTier::Leaf, domain);
            const TopologyNodeId port_b = builder.add_node("syn-move-sw-b-p0", NodeClass::PhysicalPort,
                                                           TopologyTier::Leaf, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw_a, port_a,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, sw_b, port_b,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            const TopologyNodeId nic =
                builder.add_node("syn-move-nic", NodeClass::Nic, TopologyTier::Endpoint, domain);
            const TopologyNodeId nic_port = builder.add_node("syn-move-nic-p0",
                                                             NodeClass::PhysicalPort,
                                                             TopologyTier::Endpoint, domain);
            builder.add_edge(RelationClass::PresentsEndpoint, nic, nic_port,
                             std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
            builder.add_edge(RelationClass::ConnectedTo, nic_port, port_a, std::nullopt, domain);
            break;
        }
        case SyntheticScenario::AsymmetricPublisherScopes: {
            topology.name = "asymmetric-publisher-scopes";
            topology.description =
                "Two administrative domains in one runtime; the publication covers only the "
                "first domain, so the second stays untouched.";
            build_switch_fabric(builder, "syn-asym-fabric-a", domain, 1, 1, TopologyTier::Leaf);
            Publication& other = builder.secondary(0, "scope_b");
            other.domain = TopologyDomainId::from_trusted(std::string(domain.value()) + "-b");
            builder.secondary_node(other, "syn-asym-fabric-b", NodeClass::Fabric,
                                   TopologyTier::Unspecified, other.domain);
            builder.secondary_node(other, "syn-asym-sw-b", NodeClass::Switch, TopologyTier::Leaf,
                                   other.domain);
            const TopologyNodeId fabric_b = internal::derive_node_id(other.domain, "syn-asym-fabric-b");
            const TopologyNodeId sw_b = internal::derive_node_id(other.domain, "syn-asym-sw-b");
            builder.secondary_edge(other, RelationClass::MemberOf, sw_b, fabric_b, std::nullopt,
                                   other.domain);
            break;
        }
        case SyntheticScenario::StaleSnapshot: {
            topology.name = "stale-snapshot";
            topology.description =
                "Fabric with one switch; the second publication adds a second switch so a "
                "snapshot taken before it is provably stale.";
            const TopologyNodeId fabric = builder.add_node("syn-stale-fabric", NodeClass::Fabric,
                                                           TopologyTier::Unspecified, domain);
            const TopologyNodeId sw_a =
                builder.add_node("syn-stale-sw-a", NodeClass::Switch, TopologyTier::Leaf, domain);
            builder.add_edge(RelationClass::MemberOf, sw_a, fabric, std::nullopt, domain);
            Publication& later = builder.secondary(0, "later");
            later.domain = domain;
            later.mode = PublicationMode::Incremental;
            builder.secondary_node(later, "syn-stale-sw-b", NodeClass::Switch, TopologyTier::Leaf,
                                   domain);
            const TopologyNodeId sw_b = internal::derive_node_id(domain, "syn-stale-sw-b");
            builder.secondary_edge(later, RelationClass::MemberOf, sw_b, fabric, std::nullopt, domain);
            break;
        }
        case SyntheticScenario::LargeScale: {
            const std::size_t switches = scale_or(options.scale, 100);
            const std::size_t hosts = scale_or(options.tiers, 4);
            topology.name = "large-scale";
            topology.description = std::to_string(switches) + " leaf switches with " +
                                   std::to_string(hosts) + " hosts each, plus a full mesh of " +
                                   "switch interconnects.";
            const TopologyNodeId fabric = builder.add_node("syn-large-fabric", NodeClass::Fabric,
                                                           TopologyTier::Unspecified, domain);
            std::vector<TopologyNodeId> switch_nodes;
            switch_nodes.reserve(switches);
            for (std::size_t s = 0; s < switches; ++s) {
                const std::string switch_entity = "syn-large-sw" + std::to_string(s);
                const TopologyNodeId sw = builder.add_node(switch_entity, NodeClass::Switch,
                                                           TopologyTier::Leaf, domain);
                switch_nodes.push_back(sw);
                builder.add_edge(RelationClass::MemberOf, sw, fabric, std::nullopt, domain);
                for (std::size_t h = 0; h < hosts; ++h) {
                    const std::string nic_entity = switch_entity + "-nic" + std::to_string(h);
                    const TopologyNodeId nic =
                        builder.add_node(nic_entity, NodeClass::Nic, TopologyTier::Endpoint, domain);
                    const TopologyNodeId nic_port = builder.add_node(
                        nic_entity + "-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint, domain);
                    const TopologyNodeId sw_port = builder.add_node(
                        switch_entity + "-p" + std::to_string(h), NodeClass::PhysicalPort,
                        TopologyTier::Leaf, domain);
                    builder.add_edge(RelationClass::PresentsEndpoint, nic, nic_port,
                                     std::optional<TopologyLayer>{TopologyLayer::Physical}, domain);
                    builder.add_edge(RelationClass::ConnectedTo, nic_port, sw_port, std::nullopt,
                                     domain);
                }
            }
            for (std::size_t s = 0; s + 1 < switches; ++s) {
                const TopologyNodeId uplink = builder.add_node(
                    "syn-large-sw" + std::to_string(s) + "-uplink", NodeClass::PhysicalPort,
                    TopologyTier::Leaf, domain);
                const TopologyNodeId downlink = builder.add_node(
                    "syn-large-sw" + std::to_string(s + 1) + "-downlink", NodeClass::PhysicalPort,
                    TopologyTier::Leaf, domain);
                builder.add_edge(RelationClass::ConnectedTo, uplink, downlink, std::nullopt, domain);
            }
            break;
        }
    }

    if (topology.description.empty()) {
        topology.description = "synthetic scenario " + std::string(to_string(options.scenario));
    }
    topology.publication.type = PublicationType::Synthetic;
    for (Publication& publication : topology.secondary_publications) {
        publication.type = PublicationType::Synthetic;
    }
    return topology;
}

}  // namespace fabric_topology
