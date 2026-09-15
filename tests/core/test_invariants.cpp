// Fabric Topology - structural invariant tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <optional>
#include <string>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

FT_TEST(invariants, containment_must_be_acyclic) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId site = fixture.add_node("ent-site", NodeClass::Site, TopologyTier::SiteEdge);
    const TopologyNodeId sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    const TopologyNodeId rack =
        fixture.add_node("ent-rack", NodeClass::RackNetworkDomain, TopologyTier::TopOfRack);

    FT_CHECK(!fixture.add_edge(RelationClass::Contains, site, sw, TopologyLayer::Physical).empty());
    FT_CHECK(!fixture.add_edge(RelationClass::Contains, sw, rack, TopologyLayer::Physical).empty());
    // rack -> site would close a containment cycle.
    const TopologyEdgeId cycle =
        fixture.add_edge(RelationClass::Contains, rack, site, TopologyLayer::Physical);
    FT_CHECK(cycle.empty());
    FT_CHECK(fixture.engine->validate().valid);
    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
}

FT_TEST(invariants, connectivity_cycles_are_legal) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId a = fixture.add_node("ent-a", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId b = fixture.add_node("ent-b", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId c = fixture.add_node("ent-c", NodeClass::PhysicalPort, TopologyTier::Leaf);
    FT_CHECK(!fixture.add_edge(RelationClass::ConnectedTo, a, b, std::nullopt).empty());
    FT_CHECK(!fixture.add_edge(RelationClass::ConnectedTo, b, c, std::nullopt).empty());
    FT_CHECK(!fixture.add_edge(RelationClass::ConnectedTo, c, a, std::nullopt).empty());
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{3});
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(invariants, dependency_backing_must_be_acyclic) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId physical =
        fixture.add_node("ent-phys", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId logical_a =
        fixture.add_node("ent-lp-a", NodeClass::LogicalPort, TopologyTier::Leaf);
    const TopologyNodeId logical_b =
        fixture.add_node("ent-lp-b", NodeClass::LogicalPort, TopologyTier::Leaf);

    FT_CHECK(!fixture.add_edge(RelationClass::BackedBy, logical_a, logical_b, std::nullopt).empty());
    FT_CHECK(!fixture.add_edge(RelationClass::BackedBy, logical_b, physical, std::nullopt).empty());
    FT_CHECK(fixture.add_edge(RelationClass::BackedBy, physical, logical_a, std::nullopt).empty());
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(invariants, membership_must_be_acyclic) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId fabric = fixture.add_node("ent-fabric", NodeClass::Fabric);
    const TopologyNodeId site = fixture.add_node("ent-site", NodeClass::Site, TopologyTier::SiteEdge);
    FT_CHECK(!fixture.add_edge(RelationClass::MemberOf, site, fabric, TopologyLayer::Logical).empty());
    FT_CHECK(fixture.add_edge(RelationClass::MemberOf, fabric, site, TopologyLayer::Logical).empty());
}

FT_TEST(invariants, orphan_edges_cannot_commit) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    AddRelationshipRequest request;
    request.relation = RelationClass::ConnectedTo;
    request.from = TopologyNodeId::from_trusted("n_missing_a");
    request.to = TopologyNodeId::from_trusted("n_missing_b");
    request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_relationship(fixture.context(), request).status.outcome(),
                Outcome::UnknownEntity);
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{0});
}

FT_TEST(invariants, logical_support_must_be_physical) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId phys_a =
        fixture.add_node("ent-pa", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId phys_b =
        fixture.add_node("ent-pb", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId logical_a =
        fixture.add_node("ent-la", NodeClass::LogicalPort, TopologyTier::Leaf);
    const TopologyNodeId logical_b =
        fixture.add_node("ent-lb", NodeClass::LogicalPort, TopologyTier::Leaf);
    const TopologyEdgeId physical =
        fixture.add_edge(RelationClass::ConnectedTo, phys_a, phys_b, std::nullopt);
    const TopologyEdgeId logical =
        fixture.add_edge(RelationClass::LogicallyConnectedTo, logical_a, logical_b, std::nullopt);
    FT_CHECK(!physical.empty());
    FT_CHECK(!logical.empty());

    AddRelationshipRequest request;
    request.relation = RelationClass::BackedBy;
    request.from = logical_a;
    request.to = phys_a;
    request.domain = fixture.domain;
    request.supported_by = logical;
    FT_CHECK_EQ(fixture.engine->add_relationship(fixture.context(), request).status.outcome(),
                Outcome::RelationshipConflict);

    request.supported_by = physical;
    FT_CHECK(fixture.engine->add_relationship(fixture.context(), request).accepted());
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(invariants, validation_detects_registry_supersession) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    FT_CHECK(!sw.empty());
    FT_CHECK(fixture.engine->validate().valid);

    EntityRecord successor;
    successor.entity_id = "ent-sw-gen2";
    successor.entity_class = EntityClass::Switch;
    successor.generation = EntityGeneration{2};
    FT_CHECK(fixture.directory->supersede("ent-sw", successor).outcome() == Outcome::Committed);

    const ValidationReport report = fixture.engine->validate();
    FT_CHECK(!report.valid);
    bool found = false;
    for (const ValidationIssue& issue : report.issues) {
        if (issue.code == "node.entity_superseded") {
            found = true;
        }
    }
    FT_CHECK(found);
}

FT_TEST(invariants, integrity_check_covers_every_index) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
    FT_CHECK(fixture.engine->validate().valid);
    FT_CHECK(!fixture.engine->all_nodes().empty());
    FT_CHECK(!fixture.engine->all_edges().empty());
    FT_CHECK(!fixture.engine->physical_relationships().empty());
    FT_CHECK(fixture.engine->logical_relationships().empty());
}

FT_TEST(invariants, explicit_cross_domain_flag_is_required) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    FT_CHECK(!sw.empty());
    const ValidationReport report = fixture.engine->validate();
    FT_CHECK(report.valid);
    FT_CHECK_EQ(report.nodes, std::size_t{1});
    FT_CHECK_EQ(report.edges, std::size_t{0});
}
