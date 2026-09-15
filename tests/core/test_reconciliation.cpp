// Fabric Topology - publication and reconciliation semantics tests.
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

/// Observed nodes in these tests carry explicit identifiers so that observed relationships can
/// reference them without depending on the runtime's internal identifier derivation.
TopologyNodeId observed_node_id(const std::string& entity_id) {
    return TopologyNodeId::from_trusted("n-obs-" + entity_id);
}

ObservedNode make_observed_node(ft_test::Fixture& fixture, const std::string& entity_id, NodeClass node_class,
                                TopologyTier tier = TopologyTier::Unspecified) {
    static_cast<void>(fixture.add_entity(entity_id, node_class_entity_class(node_class)));
    ObservedNode node;
    node.node_id = observed_node_id(entity_id);
    node.entity_id = entity_id;
    node.node_class = node_class;
    node.tier = tier;
    node.domain = fixture.domain;
    node.entity_generation = EntityGeneration{1};
    return node;
}

ObservedEdge make_observed_edge(const ft_test::Fixture& fixture, RelationClass relation,
                                const TopologyNodeId& from, const TopologyNodeId& to,
                                std::optional<TopologyLayer> layer = std::nullopt) {
    ObservedEdge edge;
    edge.relation = relation;
    edge.from = from;
    edge.to = to;
    edge.layer = layer;
    edge.domain = fixture.domain;
    edge.evidence_generation = EvidenceGeneration{1};
    return edge;
}

Publication make_publication(const ft_test::Fixture& fixture, const char* id, PublicationMode mode) {
    Publication publication;
    publication.id = PublicationId::from_trusted(id);
    publication.domain = fixture.domain;
    publication.mode = mode;
    publication.type = PublicationType::Synthetic;
    publication.source = DiscoverySource::SyntheticGenerator;
    return publication;
}

}  // namespace

FT_TEST(reconciliation, incremental_publication_does_not_delete_unmentioned) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const ft_test::SmallFabric small = ft_test::build_small_fabric(fixture);
    const std::size_t edges_before = fixture.engine->edge_count();
    const std::size_t nodes_before = fixture.engine->node_count();

    Publication publication = make_publication(fixture, "pub-incremental", PublicationMode::Incremental);
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-extra-sw", NodeClass::Switch, TopologyTier::Leaf));
    publication.edges.push_back(make_observed_edge(fixture, RelationClass::PeersWith,
                                                   observed_node_id("ent-extra-sw"), small.sw));

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    FT_CHECK(result.committed());
    FT_CHECK_EQ(fixture.engine->node_count(), nodes_before + 1);
    FT_CHECK_EQ(fixture.engine->edge_count(), edges_before + 1);
    // The four relationships created before the publication are untouched.
    FT_CHECK_EQ(fixture.engine->validate().edges, edges_before + 1);
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(reconciliation, authoritative_snapshot_replaces_scope_transactionally) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    // None of the pre-existing participants are mentioned by the replacement snapshot, so the
    // whole prior scope is replaced.
    const std::size_t nodes_before = fixture.engine->node_count();

    Publication publication =
        make_publication(fixture, "pub-authoritative", PublicationMode::AuthoritativeSnapshot);
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-only-sw", NodeClass::Switch, TopologyTier::Leaf));

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    if (!result.committed()) {
        FT_FAIL(result.status.one_line());
    }
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{1});
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{0});
    if (result.nodes_removed != nodes_before) {
        FT_FAIL("nodes_removed=" + std::to_string(result.nodes_removed) + " nodes_before=" +
                std::to_string(nodes_before) + " edges_removed=" +
                std::to_string(result.edges_removed) + " status=" + result.status.one_line());
    }
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(reconciliation, invalid_snapshot_is_rejected_whole) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    const std::size_t nodes_before = fixture.engine->node_count();
    const TopologyGeneration generation_before = fixture.engine->generation();

    Publication publication =
        make_publication(fixture, "pub-invalid", PublicationMode::AuthoritativeSnapshot);
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-valid-sw", NodeClass::Switch, TopologyTier::Leaf));
    ObservedNode bogus;
    bogus.node_id = observed_node_id("ent-never-registered");
    bogus.entity_id = "ent-never-registered";
    bogus.node_class = NodeClass::Switch;
    bogus.domain = fixture.domain;
    publication.nodes.push_back(bogus);

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    FT_CHECK_EQ(result.status.outcome(), Outcome::UnknownEntity);
    FT_CHECK(!result.committed());
    FT_CHECK_EQ(fixture.engine->node_count(), nodes_before);
    FT_CHECK_EQ(fixture.engine->generation(), generation_before);
    FT_CHECK(!fixture.engine->node_for_entity("ent-valid-sw").has_value());
}

FT_TEST(reconciliation, partial_observation_cannot_assert_absence) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));

    Publication publication =
        make_publication(fixture, "pub-partial", PublicationMode::PartialObservation);
    ObservedNode absent;
    absent.node_id = observed_node_id("ent-nic");
    absent.entity_id = "ent-nic";
    absent.node_class = NodeClass::Nic;
    absent.domain = fixture.domain;
    absent.present = false;
    publication.nodes.push_back(absent);

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    FT_CHECK_EQ(result.status.outcome(), Outcome::MalformedRequest);
    FT_CHECK(fixture.engine->node_for_entity("ent-nic").has_value());
}

FT_TEST(reconciliation, explicit_absence_retires_relationship) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt);
    const std::optional<TopologyEdge> created = fixture.engine->edge(edge);
    FT_CHECK(created.has_value());

    Publication publication = make_publication(fixture, "pub-absence", PublicationMode::Incremental);
    publication.declared_absent_relationships.push_back(created->relationship_id);
    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    if (!result.committed()) {
        FT_FAIL(result.status.one_line());
    }
    const std::optional<TopologyEdge> retired = fixture.engine->edge(edge);
    FT_CHECK(retired.has_value());
    FT_CHECK_EQ(retired->lifecycle, LifecycleState::Retired);
}

FT_TEST(reconciliation, present_false_removes_in_authoritative_snapshot) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    FT_CHECK(!fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt).empty());
    const std::optional<TopologyEdge> existing =
        fixture.engine->edges_for_node(port_a).empty()
            ? std::nullopt
            : std::optional<TopologyEdge>(fixture.engine->edges_for_node(port_a).front());
    FT_CHECK(existing.has_value());

    Publication publication =
        make_publication(fixture, "pub-absent-edge", PublicationMode::AuthoritativeSnapshot);
    ObservedNode node_a = make_observed_node(fixture, "ent-a-p0", NodeClass::PhysicalPort,
                                             TopologyTier::Leaf);
    node_a.node_id = port_a;
    ObservedNode node_b = make_observed_node(fixture, "ent-b-p0", NodeClass::PhysicalPort,
                                             TopologyTier::Leaf);
    node_b.node_id = port_b;
    publication.nodes.push_back(node_a);
    publication.nodes.push_back(node_b);
    ObservedEdge absent =
        make_observed_edge(fixture, RelationClass::ConnectedTo, port_a, port_b, std::nullopt);
    absent.present = false;
    publication.edges.push_back(absent);

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    if (!result.committed()) {
        FT_FAIL(result.status.one_line());
    }
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{0});
}

FT_TEST(reconciliation, publication_replay_is_idempotent) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    Publication publication =
        make_publication(fixture, "pub-replay", PublicationMode::AuthoritativeSnapshot);
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-sw", NodeClass::Switch, TopologyTier::Leaf));

    const PublicationResult first = fixture.engine->publish(fixture.context(), publication);
    FT_CHECK(first.committed());
    const TopologyGeneration after_first = fixture.engine->generation();
    const PublicationResult second = fixture.engine->publish(fixture.context(), publication);
    FT_CHECK_EQ(second.status.outcome(), Outcome::Idempotent);
    FT_CHECK_EQ(fixture.engine->generation(), after_first);
}

FT_TEST(reconciliation, stale_expected_generation_rejects_publication) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(fixture.add_node("ent-sw", NodeClass::Switch, TopologyTier::Leaf));

    Publication publication = make_publication(fixture, "pub-stale", PublicationMode::Incremental);
    publication.expected_generation = TopologyGeneration{fixture.engine->generation().value() + 1};
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-sw-2", NodeClass::Switch, TopologyTier::Leaf));

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    FT_CHECK_EQ(result.status.outcome(), Outcome::StaleGeneration);
    FT_CHECK(!fixture.engine->node_for_entity("ent-sw-2").has_value());
}

FT_TEST(reconciliation, out_of_scope_entries_are_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyDomainId other = TopologyDomainId::from_trusted("dom-other");
    DomainDefinition definition;
    definition.id = other;
    definition.kind = ScopeKind::AdministrativeDomain;
    FT_CHECK(fixture.engine->define_domain(definition).outcome() == Outcome::Committed);

    Publication publication = make_publication(fixture, "pub-out-of-scope", PublicationMode::Incremental);
    ObservedNode node = make_observed_node(fixture, "ent-sw", NodeClass::Switch, TopologyTier::Leaf);
    node.domain = other;
    publication.nodes.push_back(node);

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    FT_CHECK_EQ(result.status.outcome(), Outcome::DomainViolation);
}

FT_TEST(reconciliation, snapshot_requires_authoritative_scope) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const PublisherId limited = PublisherId::from_trusted("pub-limited");
    const WorkerBootId limited_boot = WorkerBootId::from_trusted("boot-limited");
    PublisherRegistration registration;
    registration.publisher = limited;
    registration.worker_boot = limited_boot;
    registration.coordinator_epoch = fixture.engine->coordinator_epoch();
    ScopeGrant grant;
    grant.domain = fixture.domain;
    grant.mode = GrantMode::IncrementalWrite;
    registration.grants.push_back(grant);
    FT_CHECK(outcome_is_success(fixture.engine->register_publisher(registration).outcome()));

    AuthorityContext context;
    context.coordinator_epoch = fixture.engine->coordinator_epoch();
    context.publisher = limited;
    context.worker_boot = limited_boot;

    Publication publication =
        make_publication(fixture, "pub-not-authoritative", PublicationMode::AuthoritativeSnapshot);
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-sw", NodeClass::Switch, TopologyTier::Leaf));
    FT_CHECK_EQ(fixture.engine->publish(context, publication).status.outcome(),
                Outcome::UnauthorizedScope);

    publication.id = PublicationId::from_trusted("pub-incremental-allowed");
    publication.mode = PublicationMode::Incremental;
    FT_CHECK(fixture.engine->publish(context, publication).committed());
}

FT_TEST(reconciliation, publication_diff_is_deterministic) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    Publication publication =
        make_publication(fixture, "pub-diff", PublicationMode::AuthoritativeSnapshot);
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-sw-a", NodeClass::Switch, TopologyTier::Leaf));
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-sw-b", NodeClass::Switch, TopologyTier::Leaf));

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    FT_CHECK(result.committed());
    FT_CHECK_EQ(result.nodes_added, std::size_t{2});
    FT_CHECK_EQ(result.diff.entries.size(), std::size_t{2});
    FT_CHECK_EQ(result.diff.entries[0].kind, DiffKind::NodeAdded);
    FT_CHECK(result.diff.entries[0].key < result.diff.entries[1].key);
    FT_CHECK(!result.diff.digest.empty());
}

FT_TEST(reconciliation, duplicate_relationship_identity_is_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    Publication publication = make_publication(fixture, "pub-dupes", PublicationMode::Incremental);
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-sw-a", NodeClass::Switch, TopologyTier::Leaf));
    publication.nodes.push_back(
        make_observed_node(fixture, "ent-sw-b", NodeClass::Switch, TopologyTier::Leaf));
    ObservedEdge edge = make_observed_edge(fixture, RelationClass::PeersWith,
                                           observed_node_id("ent-sw-a"),
                                           observed_node_id("ent-sw-b"));
    edge.relationship_id = RelationshipId::from_trusted("rel-fixed");
    publication.edges.push_back(edge);
    ObservedEdge duplicate = edge;
    duplicate.to = observed_node_id("ent-sw-a");
    duplicate.from = observed_node_id("ent-sw-b");
    publication.edges.push_back(duplicate);

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    FT_CHECK_EQ(result.status.outcome(), Outcome::DuplicateIdentity);
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{0});
}
