// Fabric Topology - core graph model and mutation semantics tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <optional>
#include <string>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

namespace {

TopologyNodeId require_node(const MutationResult& result) {
    FT_CHECK(result.accepted());
    FT_CHECK(result.node.has_value());
    return *result.node;
}

TopologyEdgeId require_edge(const MutationResult& result) {
    FT_CHECK(result.accepted());
    FT_CHECK(result.edge.has_value());
    return *result.edge;
}

}  // namespace

FT_TEST(core, node_lifecycle_and_queries) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const ft_test::SmallFabric small = ft_test::build_small_fabric(fixture);

    FT_CHECK(!small.fabric.empty());
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{6});
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{4});

    const std::optional<TopologyNode> node = fixture.engine->node(small.sw);
    FT_CHECK(node.has_value());
    FT_CHECK_EQ(node->entity_id, std::string("ent-switch"));
    FT_CHECK_EQ(node->node_class, NodeClass::Switch);
    FT_CHECK_EQ(node->lifecycle, LifecycleState::Current);

    const std::optional<TopologyNode> by_entity = fixture.engine->node_for_entity("ent-nic");
    FT_CHECK(by_entity.has_value());
    FT_CHECK_EQ(by_entity->id, small.nic);

    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(core, unknown_entity_is_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    AddNodeRequest request;
    request.entity_id = "ent-not-registered";
    request.node_class = NodeClass::Switch;
    request.domain = fixture.domain;
    const MutationResult result = fixture.engine->add_node(fixture.context(), request);
    FT_CHECK_EQ(result.status.outcome(), Outcome::UnknownEntity);
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{0});
}

FT_TEST(core, incompatible_entity_class_is_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-wrong-class", EntityClass::Port);
    AddNodeRequest request;
    request.entity_id = "ent-wrong-class";
    request.node_class = NodeClass::Switch;
    request.domain = fixture.domain;
    const MutationResult result = fixture.engine->add_node(fixture.context(), request);
    FT_CHECK_EQ(result.status.outcome(), Outcome::IncompatibleEntityClass);
}

FT_TEST(core, retired_and_superseded_entities_are_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-retired", EntityClass::Switch);
    fixture.directory->retire("ent-retired");
    AddNodeRequest request;
    request.entity_id = "ent-retired";
    request.node_class = NodeClass::Switch;
    request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_node(fixture.context(), request).status.outcome(), Outcome::Retired);

    fixture.add_entity("ent-old", EntityClass::Switch);
    EntityRecord successor;
    successor.entity_id = "ent-new";
    successor.entity_class = EntityClass::Switch;
    successor.generation = EntityGeneration{2};
    fixture.directory->supersede("ent-old", successor);
    request.entity_id = "ent-old";
    FT_CHECK_EQ(fixture.engine->add_node(fixture.context(), request).status.outcome(),
                Outcome::Superseded);
}

FT_TEST(core, undirected_relationships_have_one_canonical_edge) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);

    const TopologyEdgeId forward =
        fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt);
    FT_CHECK(!forward.empty());
    const TopologyEdgeId reverse =
        fixture.add_edge(RelationClass::ConnectedTo, port_b, port_a, std::nullopt);
    FT_CHECK_EQ(forward, reverse);
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{1});

    const std::optional<TopologyEdge> edge = fixture.engine->edge(forward);
    FT_CHECK(edge.has_value());
    FT_CHECK(edge->from < edge->to);
}

FT_TEST(core, duplicate_edge_with_distinct_id_is_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    FT_CHECK(!fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt).empty());

    AddRelationshipRequest request;
    request.relation = RelationClass::ConnectedTo;
    request.from = port_a;
    request.to = port_b;
    request.domain = fixture.domain;
    request.edge_id = TopologyEdgeId::from_trusted("e_explicit_duplicate");
    const MutationResult result = fixture.engine->add_relationship(fixture.context(), request);
    FT_CHECK_EQ(result.status.outcome(), Outcome::DuplicateEdge);
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{1});
}

FT_TEST(core, self_edges_are_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port =
        fixture.add_node("ent-self-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::ConnectedTo, port, port, std::nullopt);
    FT_CHECK(edge.empty());
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{0});
}

FT_TEST(core, endpoint_class_pairing_is_enforced) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    const TopologyNodeId fabric = fixture.add_node("ent-fab", NodeClass::Fabric);
    // CONNECTED_TO permits port/endpoint classes only.
    const TopologyEdgeId edge = fixture.add_edge(RelationClass::ConnectedTo, sw, fabric, std::nullopt);
    FT_CHECK(edge.empty());
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{0});
}

FT_TEST(core, layer_policy_is_enforced) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);

    AddRelationshipRequest request;
    request.relation = RelationClass::ConnectedTo;
    request.from = port_a;
    request.to = port_b;
    request.domain = fixture.domain;
    request.layer = TopologyLayer::Logical;
    FT_CHECK_EQ(fixture.engine->add_relationship(fixture.context(), request).status.outcome(),
                Outcome::MalformedRequest);

    request.relation = RelationClass::BackedBy;
    request.layer.reset();
    FT_CHECK_EQ(fixture.engine->add_relationship(fixture.context(), request).status.outcome(),
                Outcome::IncompatibleEntityClass);
}

FT_TEST(core, layer_required_when_relation_allows_both) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId site = fixture.add_node("ent-site", NodeClass::Site, TopologyTier::SiteEdge);
    const TopologyNodeId sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    AddRelationshipRequest request;
    request.relation = RelationClass::Contains;
    request.from = site;
    request.to = sw;
    request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_relationship(fixture.context(), request).status.outcome(),
                Outcome::MalformedRequest);
    request.layer = TopologyLayer::Physical;
    FT_CHECK(fixture.engine->add_relationship(fixture.context(), request).accepted());
}

FT_TEST(core, unknown_domain_is_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-sw", EntityClass::Switch);
    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.domain = TopologyDomainId::from_trusted("dom-missing");
    FT_CHECK_EQ(fixture.engine->add_node(fixture.context(), request).status.outcome(),
                Outcome::DomainViolation);
}

FT_TEST(core, cross_domain_edges_require_an_explicit_rule) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyDomainId other = TopologyDomainId::from_trusted("dom-other");
    DomainDefinition definition;
    definition.id = other;
    definition.kind = ScopeKind::AdministrativeDomain;
    FT_CHECK(fixture.engine->define_domain(definition).outcome() == Outcome::Committed);

    // Re-registration replaces the previous grant set, so the publisher must be granted both
    // scopes explicitly to hold authority over either.
    PublisherRegistration registration;
    registration.publisher = fixture.publisher;
    registration.worker_boot = fixture.boot;
    registration.coordinator_epoch = fixture.engine->coordinator_epoch();
    for (const TopologyDomainId& scope : {fixture.domain, other}) {
        ScopeGrant grant;
        grant.domain = scope;
        grant.mode = GrantMode::AuthoritativeWrite;
        registration.grants.push_back(grant);
    }
    FT_CHECK(outcome_is_success(fixture.engine->register_publisher(registration).outcome()));

    const TopologyNodeId sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    // A node in the second domain, created through the same engine.
    fixture.add_entity("ent-sw-2", EntityClass::Switch);
    AddNodeRequest node_request;
    node_request.entity_id = "ent-sw-2";
    node_request.node_class = NodeClass::Switch;
    node_request.tier = TopologyTier::Leaf;
    node_request.domain = other;
    const TopologyNodeId sw2 = require_node(fixture.engine->add_node(fixture.context(), node_request));

    AddRelationshipRequest request;
    request.relation = RelationClass::PeersWith;
    request.from = sw;
    request.to = sw2;
    request.domain = fixture.domain;
    request.allow_cross_domain = true;
    FT_CHECK_EQ(fixture.engine->add_relationship(fixture.context(), request).status.outcome(),
                Outcome::DomainViolation);

    CrossDomainRule rule;
    rule.from_domain = fixture.domain;
    rule.to_domain = other;
    rule.relation = RelationClass::PeersWith;
    FT_CHECK(fixture.engine->define_cross_domain_rule(rule).outcome() == Outcome::Committed);
    const MutationResult result = fixture.engine->add_relationship(fixture.context(), request);
    FT_CHECK(result.accepted());
    const std::optional<TopologyEdge> edge = fixture.engine->edge(*result.edge);
    FT_CHECK(edge.has_value());
    FT_CHECK(edge->cross_domain);
    FT_CHECK_EQ(edge->secondary_domain, other);
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(core, removing_a_node_with_relationships_requires_cascade) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const ft_test::SmallFabric small = ft_test::build_small_fabric(fixture);

    RemoveNodeRequest request;
    request.node = small.sw;
    FT_CHECK_EQ(fixture.engine->remove_node(fixture.context(), request).status.outcome(),
                Outcome::StructuralInvariantViolation);

    request.cascade_relationships = true;
    const MutationResult removed = fixture.engine->remove_node(fixture.context(), request);
    FT_CHECK(removed.accepted());
    FT_CHECK(!fixture.engine->node(small.sw).has_value());
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{1});
    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
}

FT_TEST(core, attachment_slot_is_exclusive) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId sw_a = fixture.add_node("ent-sw-a", NodeClass::Switch, TopologyTier::Leaf);
    const TopologyNodeId sw_b = fixture.add_node("ent-sw-b", NodeClass::Switch, TopologyTier::Leaf);
    const TopologyNodeId nic = fixture.add_node("ent-nic", NodeClass::Nic, TopologyTier::Endpoint);
    const TopologyNodeId nic_port =
        fixture.add_node("ent-nic-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint);

    AttachEndpointRequest attach;
    attach.endpoint = nic_port;
    attach.target = sw_a;
    attach.domain = fixture.domain;
    attach.attachment = AttachmentId::from_trusted("att-nic-p0");
    const MutationResult attached = fixture.engine->attach_endpoint(fixture.context(), attach);
    FT_CHECK(attached.accepted());

    AttachEndpointRequest second = attach;
    second.target = sw_b;
    FT_CHECK_EQ(fixture.engine->attach_endpoint(fixture.context(), second).status.outcome(),
                Outcome::RelationshipConflict);

    DetachEndpointRequest detach;
    detach.endpoint = nic_port;
    detach.attachment = attach.attachment;
    const MutationResult detached = fixture.engine->detach_endpoint(fixture.context(), detach);
    FT_CHECK(detached.accepted());
    const std::optional<TopologyEdge> edge = fixture.engine->edge(*attached.edge);
    FT_CHECK(edge.has_value());
    FT_CHECK_EQ(edge->lifecycle, LifecycleState::Retired);
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(core, retired_relationships_cannot_be_revived) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt);
    FT_CHECK(!edge.empty());

    RetireRequest retire;
    retire.edge = edge;
    FT_CHECK(fixture.engine->retire_relationship(fixture.context(), retire).accepted());
    FT_CHECK_EQ(fixture.engine->retire_relationship(fixture.context(), retire).status.outcome(),
                Outcome::Idempotent);

    AddRelationshipRequest request;
    request.relation = RelationClass::ConnectedTo;
    request.from = port_a;
    request.to = port_b;
    request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_relationship(fixture.context(), request).status.outcome(),
                Outcome::Retired);
}

FT_TEST(core, unknown_queries_return_empty_results) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    FT_CHECK(!fixture.engine->node(TopologyNodeId::from_trusted("n_missing")).has_value());
    FT_CHECK(!fixture.engine->edge(TopologyEdgeId::from_trusted("e_missing")).has_value());
    FT_CHECK(fixture.engine->edges_for_node(TopologyNodeId::from_trusted("n_missing")).empty());
    FT_CHECK(fixture.engine->neighbors(TopologyNodeId::from_trusted("n_missing")).empty());
}

FT_TEST(core, identical_entity_cannot_be_bound_twice) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-sw", EntityClass::Switch);
    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = fixture.domain;
    FT_CHECK(require_node(fixture.engine->add_node(fixture.context(), request)) ==
              require_node(fixture.engine->add_node(fixture.context(), request)));
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{1});

    AddNodeRequest other = request;
    other.node_id = TopologyNodeId::from_trusted("n_explicit_second");
    FT_CHECK_EQ(fixture.engine->add_node(fixture.context(), other).status.outcome(),
                Outcome::DuplicateIdentity);
}
