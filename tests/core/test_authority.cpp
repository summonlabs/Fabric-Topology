// Fabric Topology - authority, scope and worker fencing tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <optional>
#include <string>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

namespace {

PublisherRegistration make_registration(const PublisherId& publisher, const WorkerBootId& boot,
                                        CoordinatorEpoch epoch, const TopologyDomainId& domain,
                                        GrantMode mode) {
    PublisherRegistration registration;
    registration.publisher = publisher;
    registration.worker_boot = boot;
    registration.coordinator_epoch = epoch;
    ScopeGrant grant;
    grant.domain = domain;
    grant.mode = mode;
    registration.grants.push_back(grant);
    return registration;
}

}  // namespace

FT_TEST(authority, unregistered_publisher_is_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-sw", EntityClass::Switch);
    AuthorityContext context = fixture.context();
    context.publisher = PublisherId::from_trusted("pub-stranger");
    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_node(context, request).status.outcome(), Outcome::StaleAuthority);
}

FT_TEST(authority, scope_grant_is_enforced) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyDomainId other = TopologyDomainId::from_trusted("dom-other");
    DomainDefinition definition;
    definition.id = other;
    definition.kind = ScopeKind::AdministrativeDomain;
    FT_CHECK(fixture.engine->define_domain(definition).outcome() == Outcome::Committed);

    const PublisherId publisher = PublisherId::from_trusted("pub-scoped");
    const WorkerBootId boot = WorkerBootId::from_trusted("boot-scoped");
    FT_CHECK(outcome_is_success(
        fixture.engine
            ->register_publisher(make_registration(publisher, boot, fixture.engine->coordinator_epoch(),
                                                   fixture.domain, GrantMode::IncrementalWrite))
            .outcome()));

    fixture.add_entity("ent-sw", EntityClass::Switch);
    AuthorityContext context;
    context.coordinator_epoch = fixture.engine->coordinator_epoch();
    context.publisher = publisher;
    context.worker_boot = boot;

    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = other;
    FT_CHECK_EQ(fixture.engine->add_node(context, request).status.outcome(), Outcome::UnauthorizedScope);

    request.domain = fixture.domain;
    FT_CHECK(fixture.engine->add_node(context, request).accepted());
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{1});
}

FT_TEST(authority, wrong_worker_boot_is_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-sw", EntityClass::Switch);
    AuthorityContext context = fixture.context();
    context.worker_boot = WorkerBootId::from_trusted("boot-impostor");
    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_node(context, request).status.outcome(), Outcome::StaleWorkerBoot);
}

FT_TEST(authority, stale_coordinator_epoch_is_rejected) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-sw", EntityClass::Switch);
    AuthorityContext context = fixture.context();
    context.coordinator_epoch = CoordinatorEpoch{context.coordinator_epoch.value() + 3};
    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_node(context, request).status.outcome(),
                Outcome::StaleCoordinatorEpoch);
}

FT_TEST(authority, fenced_publisher_cannot_mutate) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-sw", EntityClass::Switch);
    FT_CHECK(fixture.engine->fence_publisher(fixture.publisher, fixture.boot, "test").outcome() ==
             Outcome::Committed);

    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = fixture.domain;
    // The precise cause is reported: this incarnation's boot identifier is permanently stale.
    FT_CHECK_EQ(fixture.engine->add_node(fixture.context(), request).status.outcome(),
                Outcome::StaleWorkerBoot);
}

FT_TEST(authority, stale_worker_boot_stays_stale_forever) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const WorkerBootId stale = WorkerBootId::from_trusted("boot-stale-incarnation");
    static_cast<void>(fixture.engine->fence_boot(stale, "process died", true));

    // A fresh incarnation of the same publisher is accepted only with a new boot id.
    const WorkerBootId fresh = WorkerBootId::from_trusted("boot-fresh-incarnation");
    FT_CHECK(outcome_is_success(
        fixture.engine
            ->register_publisher(make_registration(fixture.publisher, fresh,
                                                   fixture.engine->coordinator_epoch(), fixture.domain,
                                                   GrantMode::AuthoritativeWrite))
            .outcome()));

    // Re-registering the stale boot is refused.
    FT_CHECK_EQ(fixture.engine
                    ->register_publisher(make_registration(fixture.publisher, stale,
                                                           fixture.engine->coordinator_epoch(),
                                                           fixture.domain, GrantMode::AuthoritativeWrite))
                    .outcome(),
                Outcome::StaleWorkerBoot);
}

FT_TEST(authority, fence_demotes_asserted_relationships) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt);
    FT_CHECK(!edge.empty());
    const TopologyGeneration before = fixture.engine->generation();

    FT_CHECK(fixture.engine->fence_publisher(fixture.publisher, fixture.boot, "worker died").outcome() ==
             Outcome::Committed);
    const std::optional<TopologyEdge> demoted = fixture.engine->edge(edge);
    FT_CHECK(demoted.has_value());
    FT_CHECK_EQ(demoted->lifecycle, LifecycleState::RevalidationRequired);
    FT_CHECK(fixture.engine->generation() > before);
    // Durable structure survives fencing.
    FT_CHECK(fixture.engine->node(port_a).has_value());
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(authority, coordinator_epoch_advance_invalidates_authority) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    fixture.add_entity("ent-sw", EntityClass::Switch);
    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = fixture.domain;

    const CoordinatorEpoch before = fixture.engine->coordinator_epoch();
    const CoordinatorEpoch after = fixture.engine->advance_coordinator_epoch("restart");
    FT_CHECK_EQ(after.value(), before.value() + 1);

    // Process-local publisher authority never survives a coordinator restart.
    FT_CHECK(!fixture.engine->publisher_state(fixture.publisher).has_value());
    FT_CHECK_EQ(fixture.engine->add_node(fixture.context(), request).status.outcome(),
                Outcome::StaleAuthority);

    // A fresh registration under the new epoch is accepted.
    FT_CHECK(outcome_is_success(
        fixture.engine
            ->register_publisher(make_registration(fixture.publisher, fixture.boot, after,
                                                   fixture.domain, GrantMode::AuthoritativeWrite))
            .outcome()));
    AuthorityContext context = fixture.context();
    context.coordinator_epoch = after;
    FT_CHECK(fixture.engine->add_node(context, request).accepted());
}

FT_TEST(authority, epoch_advance_marks_published_evidence_for_revalidation) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId port_a =
        fixture.add_node("ent-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b =
        fixture.add_node("ent-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge =
        fixture.add_edge(RelationClass::ConnectedTo, port_a, port_b, std::nullopt);
    FT_CHECK(fixture.engine->advance_coordinator_epoch("restart").value() == 2);

    const std::optional<TopologyEdge> demoted = fixture.engine->edge(edge);
    FT_CHECK(demoted.has_value());
    FT_CHECK_EQ(demoted->lifecycle, LifecycleState::RevalidationRequired);
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(authority, publisher_state_is_inspectable) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const std::optional<PublisherState> state = fixture.engine->publisher_state(fixture.publisher);
    FT_CHECK(state.has_value());
    FT_CHECK_EQ(state->worker_boot, fixture.boot);
    FT_CHECK_EQ(state->grant_for(fixture.domain), GrantMode::AuthoritativeWrite);
    FT_CHECK_EQ(state->grant_for(TopologyDomainId::from_trusted("dom-other")), GrantMode::None);
    FT_CHECK_EQ(fixture.engine->publishers().size(), std::size_t{1});
}
