// Fabric Topology - bounded structural traversal tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <optional>
#include <string>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

namespace {

struct Hierarchy {
    TopologyNodeId site;
    TopologyNodeId rack;
    TopologyNodeId sw;
    TopologyNodeId port;
    TopologyNodeId nic;
    TopologyNodeId nic_port;
};

Hierarchy build_hierarchy(ft_test::Fixture& fixture) {
    Hierarchy hierarchy;
    hierarchy.site = fixture.add_node("ent-site", NodeClass::Site, TopologyTier::SiteEdge);
    hierarchy.rack =
        fixture.add_node("ent-rack", NodeClass::RackNetworkDomain, TopologyTier::TopOfRack);
    hierarchy.sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    hierarchy.port = fixture.add_node("ent-sw-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    hierarchy.nic = fixture.add_node("ent-nic", NodeClass::Nic, TopologyTier::Endpoint);
    hierarchy.nic_port = fixture.add_node("ent-nic-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint);

    fixture.add_edge(RelationClass::Contains, hierarchy.site, hierarchy.rack,
                     TopologyLayer::Physical);
    fixture.add_edge(RelationClass::Contains, hierarchy.rack, hierarchy.sw,
                     TopologyLayer::Physical);
    fixture.add_edge(RelationClass::PresentsEndpoint, hierarchy.sw, hierarchy.port,
                     TopologyLayer::Physical);
    fixture.add_edge(RelationClass::PresentsEndpoint, hierarchy.nic, hierarchy.nic_port,
                     TopologyLayer::Physical);
    fixture.add_edge(RelationClass::ConnectedTo, hierarchy.nic_port, hierarchy.port, std::nullopt);
    return hierarchy;
}

TopologyNodeId find(const TraversalResult& result, const TopologyNodeId& node) {
    for (const TraversalStep& step : result.steps) {
        if (step.node == node) {
            return step.node;
        }
    }
    return TopologyNodeId{};
}

}  // namespace

FT_TEST(traversal, neighbors_one_hop) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const Hierarchy hierarchy = build_hierarchy(fixture);
    TraversalRequest request;
    request.kind = TraversalKind::Neighbors;
    request.origin = hierarchy.sw;
    request.max_depth = 1;
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK(result.status.outcome() == Outcome::Ok);
    FT_CHECK_EQ(result.steps.size(), std::size_t{2});
    FT_CHECK(!find(result, hierarchy.rack).empty());
    FT_CHECK(!find(result, hierarchy.port).empty());
    FT_CHECK(result.steps.front().depth <= result.steps.back().depth);
}

FT_TEST(traversal, ancestors_follow_containment_upwards) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const Hierarchy hierarchy = build_hierarchy(fixture);
    TraversalRequest request;
    request.kind = TraversalKind::Ancestors;
    request.origin = hierarchy.sw;
    request.max_depth = 8;
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK(!find(result, hierarchy.rack).empty());
    FT_CHECK(!find(result, hierarchy.site).empty());
    FT_CHECK(find(result, hierarchy.port).empty());
}

FT_TEST(traversal, descendants_follow_containment_downwards) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const Hierarchy hierarchy = build_hierarchy(fixture);
    TraversalRequest request;
    request.kind = TraversalKind::Descendants;
    request.origin = hierarchy.site;
    request.max_depth = 8;
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK(!find(result, hierarchy.rack).empty());
    FT_CHECK(!find(result, hierarchy.sw).empty());
    FT_CHECK(find(result, hierarchy.nic).empty());
}

FT_TEST(traversal, connected_component_spans_physical_graph) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const Hierarchy hierarchy = build_hierarchy(fixture);
    TraversalRequest request;
    request.kind = TraversalKind::ConnectedComponent;
    request.origin = hierarchy.nic;
    request.max_depth = 16;
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK(!find(result, hierarchy.site).empty());
    FT_CHECK_EQ(result.steps.size(), std::size_t{5});
}

FT_TEST(traversal, physical_attachment_chain_is_bounded) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const Hierarchy hierarchy = build_hierarchy(fixture);
    TraversalRequest request;
    request.kind = TraversalKind::PhysicalAttachmentChain;
    request.origin = hierarchy.nic;
    request.max_depth = 4;
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK(!find(result, hierarchy.nic_port).empty());
    FT_CHECK(!find(result, hierarchy.port).empty());
    FT_CHECK(!find(result, hierarchy.sw).empty());
    FT_CHECK(find(result, hierarchy.site).empty());
}

FT_TEST(traversal, logical_dependency_chain_follows_support) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId physical =
        fixture.add_node("ent-phys", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId logical =
        fixture.add_node("ent-logical", NodeClass::LogicalPort, TopologyTier::Leaf);
    const TopologyNodeId overlay =
        fixture.add_node("ent-overlay", NodeClass::LogicalEndpoint, TopologyTier::Leaf);
    FT_CHECK(!fixture.add_edge(RelationClass::BackedBy, logical, physical, std::nullopt).empty());
    FT_CHECK(!fixture.add_edge(RelationClass::BackedBy, overlay, logical, std::nullopt).empty());

    TraversalRequest request;
    request.kind = TraversalKind::LogicalDependencyChain;
    request.origin = overlay;
    request.max_depth = 4;
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK(!find(result, logical).empty());
    FT_CHECK(!find(result, physical).empty());
    FT_CHECK(result.render().find("LOGICAL_DEPENDENCY_CHAIN") != std::string::npos);
}

FT_TEST(traversal, depth_limit_truncates_deterministically) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const Hierarchy hierarchy = build_hierarchy(fixture);
    TraversalRequest request;
    request.kind = TraversalKind::Descendants;
    request.origin = hierarchy.site;
    request.max_depth = 1;
    const TraversalResult shallow = fixture.engine->traverse(request);
    FT_CHECK_EQ(shallow.steps.size(), std::size_t{1});
    FT_CHECK(!find(shallow, hierarchy.rack).empty());
    FT_CHECK(find(shallow, hierarchy.sw).empty());
}

FT_TEST(traversal, visited_limit_reports_truncation) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(build_hierarchy(fixture));
    TraversalRequest request;
    request.kind = TraversalKind::ConnectedComponent;
    request.origin = fixture.engine->all_nodes().front().id;
    request.max_depth = 16;
    request.max_visited = 1;
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK(result.truncated);
    FT_CHECK_EQ(result.steps.size(), std::size_t{1});
    FT_CHECK(result.status.has("truncated"));
}

FT_TEST(traversal, non_current_relationships_are_not_traversed) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId a = fixture.add_node("ent-a", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId b = fixture.add_node("ent-b", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::ConnectedTo, a, b, std::nullopt);
    RetireRequest retire;
    retire.edge = edge;
    FT_CHECK(fixture.engine->retire_relationship(fixture.context(), retire).accepted());

    TraversalRequest request;
    request.kind = TraversalKind::Neighbors;
    request.origin = a;
    request.max_depth = 1;
    FT_CHECK(fixture.engine->traverse(request).steps.empty());
    request.include_non_current = true;
    // Retired relationships are never traversed, even when non-current ones are requested.
    FT_CHECK(fixture.engine->traverse(request).steps.empty());
}

FT_TEST(traversal, unknown_origin_is_reported) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    TraversalRequest request;
    request.kind = TraversalKind::Neighbors;
    request.origin = TopologyNodeId::from_trusted("n-missing");
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK_EQ(result.status.outcome(), Outcome::NotFound);
}
