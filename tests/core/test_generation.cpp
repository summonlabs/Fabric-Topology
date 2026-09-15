// Fabric Topology - generation semantics tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <optional>
#include <string>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

FT_TEST(generation, advances_once_per_state_change) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyGeneration start = fixture.engine->generation();
    FT_CHECK_EQ(start.value(), std::uint64_t{0});

    const TopologyNodeId sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    FT_CHECK(!sw.empty());
    const TopologyGeneration after_node = fixture.engine->generation();
    FT_CHECK_EQ(after_node.value(), std::uint64_t{1});

    const TopologyNodeId port =
        fixture.add_node("ent-sw-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    FT_CHECK(!fixture.add_edge(RelationClass::PresentsEndpoint, sw, port,
                               TopologyLayer::Physical)
                   .empty());
    FT_CHECK_EQ(fixture.engine->generation().value(), std::uint64_t{3});
}

FT_TEST(generation, idempotent_replay_does_not_advance) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    FT_CHECK(!fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt).empty());
    const TopologyGeneration after = fixture.engine->generation();

    AddRelationshipRequest request;
    request.relation = RelationClass::ConnectedTo;
    request.from = port_a;
    request.to = port_b;
    request.domain = fixture.domain;
    for (int i = 0; i < 5; ++i) {
        const MutationResult result = fixture.engine->add_relationship(fixture.context(), request);
        FT_CHECK_EQ(result.status.outcome(), Outcome::Idempotent);
        FT_CHECK_EQ(fixture.engine->generation(), after);
    }

    AddNodeRequest node_request;
    node_request.entity_id = "ent-a-p0";
    node_request.node_class = NodeClass::PhysicalPort;
    node_request.tier = TopologyTier::Leaf;
    node_request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_node(fixture.context(), node_request).status.outcome(),
                Outcome::Idempotent);
    FT_CHECK_EQ(fixture.engine->generation(), after);
}

FT_TEST(generation, stale_expected_generation_rejects_before_mutation) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-sw", EntityClass::Switch);
    const TopologyGeneration start = fixture.engine->generation();

    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = fixture.domain;
    request.expected_generation = TopologyGeneration{start.value() + 7};
    const MutationResult result = fixture.engine->add_node(fixture.context(), request);
    FT_CHECK_EQ(result.status.outcome(), Outcome::StaleGeneration);
    FT_CHECK_EQ(fixture.engine->generation(), start);
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{0});
}

FT_TEST(generation, edge_generation_advances_on_change) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt);
    const std::optional<TopologyEdge> created = fixture.engine->edge(edge);
    FT_CHECK(created.has_value());
    FT_CHECK_EQ(created->generation.value(), std::uint64_t{1});

    UpdateEvidenceRequest update;
    update.edge = edge;
    update.evidence_generation = EvidenceGeneration{9};
    const MutationResult result = fixture.engine->update_relationship_evidence(fixture.context(), update);
    FT_CHECK(result.accepted());
    const std::optional<TopologyEdge> changed = fixture.engine->edge(edge);
    FT_CHECK(changed.has_value());
    FT_CHECK_EQ(changed->generation.value(), std::uint64_t{2});
    FT_CHECK_EQ(changed->evidence_generation.value(), std::uint64_t{9});

    const MutationResult replay = fixture.engine->update_relationship_evidence(fixture.context(), update);
    FT_CHECK_EQ(replay.status.outcome(), Outcome::Idempotent);
}

FT_TEST(generation, stale_edge_generation_rejects) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt);

    UpdateEvidenceRequest update;
    update.edge = edge;
    update.evidence_generation = EvidenceGeneration{2};
    update.expected_edge_generation = EdgeGeneration{99};
    FT_CHECK_EQ(fixture.engine->update_relationship_evidence(fixture.context(), update).status.outcome(),
                Outcome::StaleGeneration);
}

FT_TEST(generation, entity_generation_change_fences_relationships) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    const TopologyNodeId port =
        fixture.add_node("ent-sw-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::PresentsEndpoint, sw, port, TopologyLayer::Physical);
    FT_CHECK(!edge.empty());
    const std::optional<TopologyEdge> before = fixture.engine->edge(edge);
    FT_CHECK(before.has_value());
    FT_CHECK_EQ(before->lifecycle, LifecycleState::Current);

    FT_CHECK(fixture.directory->bump_generation("ent-sw", EntityGeneration{2}).outcome() ==
             Outcome::Committed);

    ReplaceEndpointGenerationRequest replace;
    replace.node = sw;
    replace.new_entity_generation = EntityGeneration{2};
    replace.reason = "registry generation advanced";
    const MutationResult result = fixture.engine->replace_endpoint_generation(fixture.context(), replace);
    FT_CHECK(result.accepted());

    const std::optional<TopologyEdge> after = fixture.engine->edge(edge);
    FT_CHECK(after.has_value());
    FT_CHECK_EQ(after->lifecycle, LifecycleState::RevalidationRequired);
    FT_CHECK_EQ(after->from_entity_generation.value(), std::uint64_t{2});
    FT_CHECK(fixture.engine->validate().valid);

    // Revalidation restores currentness under fresh evidence.
    RevalidateRequest revalidate;
    revalidate.edge = edge;
    revalidate.evidence_generation = EvidenceGeneration{5};
    const MutationResult revalidated = fixture.engine->revalidate_relationship(fixture.context(), revalidate);
    FT_CHECK(revalidated.accepted());
    const std::optional<TopologyEdge> restored = fixture.engine->edge(edge);
    FT_CHECK(restored.has_value());
    FT_CHECK_EQ(restored->lifecycle, LifecycleState::Current);
}

FT_TEST(generation, revalidation_requires_matching_entity_generation) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt);
    FT_CHECK(!edge.empty());

    // Advancing the registry generation out from under an existing current edge makes an
    // explicit revalidation impossible until the node binding is updated.
    FT_CHECK(fixture.directory->bump_generation("ent-a-p0", EntityGeneration{4}).outcome() ==
             Outcome::Committed);
    RevalidateRequest revalidate;
    revalidate.edge = edge;
    revalidate.evidence_generation = EvidenceGeneration{3};
    FT_CHECK_EQ(fixture.engine->revalidate_relationship(fixture.context(), revalidate).status.outcome(),
                Outcome::StaleEntityGeneration);
}
