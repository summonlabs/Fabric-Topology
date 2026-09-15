// Fabric Topology test support - shared fixture for engine-level tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_TESTS_TOPOLOGY_FIXTURE_HPP
#define FABRIC_TOPOLOGY_TESTS_TOPOLOGY_FIXTURE_HPP

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"

namespace ft_test {

using namespace fabric_topology;

struct Fixture {
    std::shared_ptr<InMemoryEntityDirectory> directory;
    std::unique_ptr<TopologyEngine> engine;
    PublisherId publisher = PublisherId::from_trusted("pub-test");
    WorkerBootId boot = WorkerBootId::from_trusted("boot-test-1");
    TopologyDomainId domain = TopologyDomainId::from_trusted("dom-test");

    AuthorityContext context() const {
        AuthorityContext ctx;
        ctx.coordinator_epoch = engine->coordinator_epoch();
        ctx.publisher = publisher;
        ctx.worker_boot = boot;
        ctx.evidence_generation = EvidenceGeneration{1};
        return ctx;
    }

    EntityRecord add_entity(const std::string& id, EntityClass entity_class,
                            EntityGeneration generation = EntityGeneration{1}) {
        EntityRecord record;
        record.entity_id = id;
        record.entity_class = entity_class;
        record.generation = generation;
        directory->add(record);
        return record;
    }

    static EntityClass class_for(NodeClass node_class) { return node_class_entity_class(node_class); }

    TopologyNodeId add_node(const std::string& entity_id, NodeClass node_class,
                            TopologyTier tier = TopologyTier::Unspecified) {
        add_entity(entity_id, class_for(node_class));
        AddNodeRequest request;
        request.entity_id = entity_id;
        request.node_class = node_class;
        request.tier = tier;
        request.domain = domain;
        const MutationResult result = engine->add_node(context(), request);
        if (!result.accepted()) {
            return TopologyNodeId{};
        }
        return *result.node;
    }

    /// Resolve the layer the way a caller must: relations that permit either layer need an
    /// explicit choice, so the helper supplies the policy default instead of leaving it unset.
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

    TopologyEdgeId add_edge(RelationClass relation, const TopologyNodeId& from, const TopologyNodeId& to,
                            std::optional<TopologyLayer> layer = std::nullopt) {
        AddRelationshipRequest request;
        request.relation = relation;
        request.from = from;
        request.to = to;
        request.layer = effective_layer(relation, layer);
        request.domain = domain;
        const MutationResult result = engine->add_relationship(context(), request);
        if (!result.accepted()) {
            return TopologyEdgeId{};
        }
        return *result.edge;
    }
};

inline TopologyEngineOptions default_options(const std::shared_ptr<InMemoryEntityDirectory>& directory) {
    TopologyEngineOptions options;
    options.directory = directory;
    options.verify_indexes_on_mutation = true;
    return options;
}

inline Fixture make_fixture_with_indexes(bool verify_indexes) {
    Fixture fixture;
    fixture.directory = std::make_shared<InMemoryEntityDirectory>();
    TopologyEngineOptions options = default_options(fixture.directory);
    options.verify_indexes_on_mutation = verify_indexes;
    fixture.engine = std::make_unique<TopologyEngine>(options);

    DomainDefinition definition;
    definition.id = fixture.domain;
    definition.kind = ScopeKind::AdministrativeDomain;
    static_cast<void>(fixture.engine->define_domain(definition));

    PublisherRegistration registration;
    registration.publisher = fixture.publisher;
    registration.worker_boot = fixture.boot;
    registration.coordinator_epoch = fixture.engine->coordinator_epoch();
    registration.reason = "fixture";
    ScopeGrant grant;
    grant.domain = fixture.domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    static_cast<void>(fixture.engine->register_publisher(registration));
    return fixture;
}

/// Standard fixture: every committed mutation self-checks its indexes.
inline Fixture make_fixture() { return make_fixture_with_indexes(true); }

/// Fixture without the O(N) per-mutation index self-check, for large-graph tests.
inline Fixture make_fast_fixture() { return make_fixture_with_indexes(false); }

/// A small but complete physical fabric: fabric + switch + two ports + NIC + endpoints.
struct SmallFabric {
    TopologyNodeId fabric;
    TopologyNodeId sw;
    TopologyNodeId sw_port_a;
    TopologyNodeId sw_port_b;
    TopologyNodeId nic;
    TopologyNodeId nic_port;
};

inline SmallFabric build_small_fabric(Fixture& fixture) {
    SmallFabric small;
    small.fabric = fixture.add_node("ent-fabric", NodeClass::Fabric);
    small.sw = fixture.add_node("ent-switch", NodeClass::Switch, TopologyTier::Leaf);
    small.sw_port_a = fixture.add_node("ent-switch-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    small.sw_port_b = fixture.add_node("ent-switch-p1", NodeClass::PhysicalPort, TopologyTier::Leaf);
    small.nic = fixture.add_node("ent-nic", NodeClass::Nic, TopologyTier::Endpoint);
    small.nic_port = fixture.add_node("ent-nic-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint);
    fixture.add_edge(RelationClass::MemberOf, small.sw, small.fabric, std::nullopt);
    fixture.add_edge(RelationClass::PresentsEndpoint, small.sw, small.sw_port_a,
                     std::optional<TopologyLayer>{TopologyLayer::Physical});
    fixture.add_edge(RelationClass::PresentsEndpoint, small.sw, small.sw_port_b,
                     std::optional<TopologyLayer>{TopologyLayer::Physical});
    fixture.add_edge(RelationClass::PresentsEndpoint, small.nic, small.nic_port,
                     std::optional<TopologyLayer>{TopologyLayer::Physical});
    return small;
}

}  // namespace ft_test

#endif  // FABRIC_TOPOLOGY_TESTS_TOPOLOGY_FIXTURE_HPP
