// Fabric Topology - determinism tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <algorithm>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

namespace {

struct TopologyPlan {
    std::vector<std::pair<std::string, NodeClass>> nodes;
    std::vector<std::tuple<RelationClass, std::string, std::string, bool>> edges;
};

TopologyPlan canonical_plan() {
    TopologyPlan plan;
    plan.nodes = {{"ent-fabric", NodeClass::Fabric},
                  {"ent-sw-a", NodeClass::Switch},
                  {"ent-sw-b", NodeClass::Switch},
                  {"ent-sw-a-p0", NodeClass::PhysicalPort},
                  {"ent-sw-a-p1", NodeClass::PhysicalPort},
                  {"ent-sw-b-p0", NodeClass::PhysicalPort},
                  {"ent-nic-a", NodeClass::Nic},
                  {"ent-nic-a-p0", NodeClass::PhysicalPort}};
    plan.edges = {{RelationClass::MemberOf, "ent-sw-a", "ent-fabric", true},
                  {RelationClass::MemberOf, "ent-sw-b", "ent-fabric", true},
                  {RelationClass::PresentsEndpoint, "ent-sw-a", "ent-sw-a-p0", false},
                  {RelationClass::PresentsEndpoint, "ent-sw-a", "ent-sw-a-p1", false},
                  {RelationClass::PresentsEndpoint, "ent-sw-b", "ent-sw-b-p0", false},
                  {RelationClass::PresentsEndpoint, "ent-nic-a", "ent-nic-a-p0", false},
                  {RelationClass::ConnectedTo, "ent-sw-a-p0", "ent-sw-b-p0", false},
                  {RelationClass::ConnectedTo, "ent-nic-a-p0", "ent-sw-a-p1", false}};
    return plan;
}

std::unique_ptr<TopologyEngine> build(const TopologyPlan& plan, bool reverse,
                                      std::shared_ptr<InMemoryEntityDirectory> directory) {
    auto engine = std::make_unique<TopologyEngine>(ft_test::default_options(directory));
    const TopologyDomainId domain = TopologyDomainId::from_trusted("dom-test");
    DomainDefinition definition;
    definition.id = domain;
    static_cast<void>(engine->define_domain(definition));

    const PublisherId publisher = PublisherId::from_trusted("pub-det");
    const WorkerBootId boot = WorkerBootId::from_trusted("boot-det");
    PublisherRegistration registration;
    registration.publisher = publisher;
    registration.worker_boot = boot;
    registration.coordinator_epoch = engine->coordinator_epoch();
    ScopeGrant grant;
    grant.domain = domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    static_cast<void>(engine->register_publisher(registration));

    AuthorityContext context;
    context.coordinator_epoch = engine->coordinator_epoch();
    context.publisher = publisher;
    context.worker_boot = boot;

    std::vector<std::pair<std::string, NodeClass>> nodes = plan.nodes;
    if (reverse) {
        std::reverse(nodes.begin(), nodes.end());
    }
    for (const auto& node : nodes) {
        EntityRecord record;
        record.entity_id = node.first;
        record.entity_class = node_class_entity_class(node.second);
        record.generation = EntityGeneration{1};
        static_cast<void>(directory->add(record));
        AddNodeRequest request;
        request.entity_id = node.first;
        request.node_class = node.second;
        request.domain = domain;
        static_cast<void>(engine->add_node(context, request));
    }

    std::vector<std::tuple<RelationClass, std::string, std::string, bool>> edges = plan.edges;
    if (reverse) {
        std::reverse(edges.begin(), edges.end());
    }
    for (const auto& edge : edges) {
        AddRelationshipRequest request;
        request.relation = std::get<0>(edge);
        const std::optional<TopologyNode> from_node = engine->node_for_entity(std::get<1>(edge));
        const std::optional<TopologyNode> to_node = engine->node_for_entity(std::get<2>(edge));
        request.from = from_node.has_value() ? from_node->id : TopologyNodeId{};
        request.to = to_node.has_value() ? to_node->id : TopologyNodeId{};
        request.domain = domain;
        request.layer = std::get<3>(edge) ? std::optional<TopologyLayer>{TopologyLayer::Logical}
                                          : std::optional<TopologyLayer>{TopologyLayer::Physical};
        static_cast<void>(engine->add_relationship(context, request));
    }
    return engine;
}

}  // namespace

FT_TEST(determinism, digest_is_insertion_order_independent) {
    auto directory_a = std::make_shared<InMemoryEntityDirectory>();
    auto directory_b = std::make_shared<InMemoryEntityDirectory>();
    const TopologyPlan plan = canonical_plan();
    const std::unique_ptr<TopologyEngine> forward = build(plan, false, directory_a);
    const std::unique_ptr<TopologyEngine> reverse = build(plan, true, directory_b);

    FT_CHECK_EQ(forward->node_count(), reverse->node_count());
    FT_CHECK_EQ(forward->edge_count(), reverse->edge_count());
    FT_CHECK_EQ(forward->digest(), reverse->digest());
    FT_CHECK_EQ(forward->render_canonical(), reverse->render_canonical());
    FT_CHECK_EQ(forward->generation(), reverse->generation());
}

FT_TEST(determinism, digest_tracks_content) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const std::string empty_digest = fixture.engine->digest();
    const TopologyNodeId sw = fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    const std::string with_node = fixture.engine->digest();
    FT_CHECK(empty_digest != with_node);

    const TopologyNodeId port =
        fixture.add_node("ent-sw-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::PresentsEndpoint, sw, port, TopologyLayer::Physical);
    const std::string with_edge = fixture.engine->digest();
    FT_CHECK(with_edge != with_node);

    RetireRequest retire;
    retire.edge = edge;
    FT_CHECK(fixture.engine->retire_relationship(fixture.context(), retire).accepted());
    FT_CHECK(fixture.engine->digest() != with_edge);
}

FT_TEST(determinism, scoped_digests_are_stable) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    const std::string all = fixture.engine->digest();
    const std::string again = fixture.engine->digest();
    FT_CHECK_EQ(all, again);
    const std::string physical = fixture.engine->digest(SnapshotScope::of_layer(TopologyLayer::Physical));
    const std::string logical = fixture.engine->digest(SnapshotScope::of_layer(TopologyLayer::Logical));
    FT_CHECK(physical != all);
    FT_CHECK(logical != physical);
}

FT_TEST(determinism, snapshot_rendering_is_stable) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    const TopologySnapshot first = fixture.engine->snapshot();
    const TopologySnapshot second = fixture.engine->snapshot();
    FT_CHECK_EQ(first.digest, second.digest);
    FT_CHECK_EQ(first.topology_generation, second.topology_generation);
    FT_CHECK(first.snapshot_generation != second.snapshot_generation);
    const std::string render_first = first.render();
    TopologySnapshot copy = first;
    copy.id = second.id;
    copy.snapshot_generation = second.snapshot_generation;
    FT_CHECK_EQ(copy.render().substr(copy.render().find("topology_generation")),
                first.render().substr(first.render().find("topology_generation")));
    FT_CHECK(!render_first.empty());
}

FT_TEST(determinism, snapshot_currentness_is_reported) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    const TopologySnapshot snapshot = fixture.engine->snapshot();
    FT_CHECK(fixture.engine->check_snapshot(snapshot).current);

    static_cast<void>(fixture.add_node("ent-extra", NodeClass::Switch, TopologyTier::Leaf));
    const SnapshotCurrentness stale = fixture.engine->check_snapshot(snapshot);
    FT_CHECK(!stale.current);
    FT_CHECK_EQ(stale.verdict, SnapshotVerdict::DigestMismatch);
    FT_CHECK(!stale.render().empty());

    const TopologySnapshot fresh = fixture.engine->snapshot();
    FT_CHECK(fixture.engine->check_snapshot(fresh).current);
    const TopologyDiff diff = fixture.engine->diff(snapshot, fresh);
    FT_CHECK_EQ(diff.size(), std::size_t{1});
    FT_CHECK_EQ(diff.entries.front().kind, DiffKind::NodeAdded);
}

FT_TEST(determinism, diff_rendering_is_stable) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologySnapshot before = fixture.engine->snapshot();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    const TopologySnapshot after = fixture.engine->snapshot();
    const TopologyDiff first = fixture.engine->diff(before, after);
    const TopologyDiff second = fixture.engine->diff(before, after);
    FT_CHECK_EQ(first.render(), second.render());
    FT_CHECK_EQ(first.digest, second.digest);
    FT_CHECK_EQ(first.count(DiffKind::NodeAdded), std::size_t{6});
    FT_CHECK_EQ(first.count(DiffKind::EdgeAdded), std::size_t{4});
}

FT_TEST(determinism, explanations_are_stable) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const ft_test::SmallFabric small = ft_test::build_small_fabric(fixture);
    const std::vector<TopologyEdge> edges = fixture.engine->edges_for_node(small.sw);
    FT_CHECK(!edges.empty());

    Explanation first;
    Explanation second;
    FT_CHECK(fixture.engine->explain_relationship(edges.front().id, first).outcome() == Outcome::Ok);
    FT_CHECK(fixture.engine->explain_relationship(edges.front().id, second).outcome() == Outcome::Ok);
    FT_CHECK_EQ(first.render(), second.render());
    FT_CHECK_EQ(first.one_line(), second.one_line());
    FT_CHECK(first.has("lifecycle"));
    FT_CHECK(first.has("currentness_reason"));

    Explanation node_explanation;
    FT_CHECK(fixture.engine->explain_node(small.sw, node_explanation).outcome() == Outcome::Ok);
    FT_CHECK(node_explanation.has("participates_in_current_topology"));
    FT_CHECK_EQ(node_explanation.render(), node_explanation.render());
}

FT_TEST(determinism, published_state_is_order_independent) {
    ft_test::Fixture forward = ft_test::make_fixture();
    ft_test::Fixture reverse = ft_test::make_fixture();
    const std::vector<std::string> entities = {"ent-sw-a", "ent-sw-b", "ent-sw-c"};
    for (const std::string& entity : entities) {
        static_cast<void>(forward.add_entity(entity, EntityClass::Switch));
        static_cast<void>(reverse.add_entity(entity, EntityClass::Switch));
    }

    Publication forward_publication;
    forward_publication.id = PublicationId::from_trusted("pub-order-forward");
    forward_publication.domain = forward.domain;
    forward_publication.mode = PublicationMode::AuthoritativeSnapshot;
    forward_publication.type = PublicationType::Synthetic;
    Publication reverse_publication = forward_publication;
    reverse_publication.id = PublicationId::from_trusted("pub-order-reverse");

    for (const std::string& entity : entities) {
        ObservedNode node;
        node.entity_id = entity;
        node.node_class = NodeClass::Switch;
        node.tier = TopologyTier::Leaf;
        node.domain = forward.domain;
        node.entity_generation = EntityGeneration{1};
        forward_publication.nodes.push_back(node);
    }
    for (auto it = entities.rbegin(); it != entities.rend(); ++it) {
        ObservedNode node;
        node.entity_id = *it;
        node.node_class = NodeClass::Switch;
        node.tier = TopologyTier::Leaf;
        node.domain = reverse.domain;
        node.entity_generation = EntityGeneration{1};
        reverse_publication.nodes.push_back(node);
    }

    FT_CHECK(forward.engine->publish(forward.context(), forward_publication).committed());
    FT_CHECK(reverse.engine->publish(reverse.context(), reverse_publication).committed());
    FT_CHECK_EQ(forward.engine->digest(), reverse.engine->digest());
    FT_CHECK_EQ(forward.engine->render_canonical(), reverse.engine->render_canonical());
}
