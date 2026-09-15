// Fabric Topology - synthetic scenario coverage tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <memory>
#include <string>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

namespace {

struct ScenarioHarness {
    std::shared_ptr<InMemoryEntityDirectory> directory;
    std::unique_ptr<TopologyEngine> engine;
    TopologyDomainId domain = TopologyDomainId::from_trusted("dom-syn");
    TopologyDomainId secondary_domain = TopologyDomainId::from_trusted("dom-syn-b");
    PublisherId publisher = PublisherId::from_trusted("pub-synthetic");
    WorkerBootId boot = WorkerBootId::from_trusted("boot-synthetic");

    AuthorityContext context() const {
        AuthorityContext ctx;
        ctx.coordinator_epoch = engine->coordinator_epoch();
        ctx.publisher = publisher;
        ctx.worker_boot = boot;
        return ctx;
    }
};

void prepare(ScenarioHarness& harness, const SyntheticTopology& topology) {
    harness.directory = std::make_shared<InMemoryEntityDirectory>();
    for (const EntityRecord& record : topology.directory_entries) {
        static_cast<void>(harness.directory->add(record));
    }
    TopologyEngineOptions options;
    options.directory = harness.directory;
    options.verify_indexes_on_mutation = true;
    harness.engine = std::make_unique<TopologyEngine>(options);

    for (const TopologyDomainId& domain : {harness.domain, harness.secondary_domain}) {
        DomainDefinition definition;
        definition.id = domain;
        definition.kind = ScopeKind::AdministrativeDomain;
        static_cast<void>(harness.engine->define_domain(definition));
    }

    PublisherRegistration registration;
    registration.publisher = harness.publisher;
    registration.worker_boot = harness.boot;
    registration.coordinator_epoch = harness.engine->coordinator_epoch();
    for (const TopologyDomainId& domain : {harness.domain, harness.secondary_domain}) {
        ScopeGrant grant;
        grant.domain = domain;
        grant.mode = GrantMode::AuthoritativeWrite;
        registration.grants.push_back(grant);
    }
    static_cast<void>(harness.engine->register_publisher(registration));
}

SyntheticTopology scenario(SyntheticScenario value, std::size_t scale = 0, std::size_t tiers = 0) {
    SyntheticOptions options;
    options.scenario = value;
    options.scale = scale;
    options.tiers = tiers;
    options.domain = TopologyDomainId::from_trusted("dom-syn");
    options.publisher = PublisherId::from_trusted("pub-synthetic");
    return build_synthetic_topology(options);
}

std::vector<SyntheticScenario> small_scenarios() {
    return {SyntheticScenario::SingleSwitch,      SyntheticScenario::DualSwitchRedundancy,
            SyntheticScenario::LeafSpine,         SyntheticScenario::MultiTierClos,
            SyntheticScenario::RackToLeaf,        SyntheticScenario::MultipleFabrics,
            SyntheticScenario::MultiSite,         SyntheticScenario::LogicalOverlay,
            SyntheticScenario::PhysicalAndLogical, SyntheticScenario::Partitioned,
            SyntheticScenario::DeviceReplacement, SyntheticScenario::AttachmentMove,
            SyntheticScenario::AsymmetricPublisherScopes, SyntheticScenario::StaleSnapshot};
}

}  // namespace

FT_TEST(synthetic, every_scenario_publishes_and_validates) {
    for (const SyntheticScenario value : small_scenarios()) {
        ScenarioHarness harness;
        const SyntheticTopology topology = scenario(value);
        prepare(harness, topology);
        FT_CHECK_EQ(topology.publication.type, PublicationType::Synthetic);
        FT_CHECK(!topology.publication.nodes.empty());

        const PublicationResult result =
            harness.engine->publish(harness.context(), topology.publication);
        if (!result.committed()) {
            FT_FAIL(std::string("scenario ") + to_string(value) + ": " + result.status.one_line());
        }
        const ValidationReport report = harness.engine->validate();
        if (!report.valid) {
            FT_FAIL(std::string("scenario ") + to_string(value) + ": " + report.render());
        }
        FT_CHECK(harness.engine->verify_integrity().outcome() == Outcome::Ok);
        for (const TopologyEdge& edge : harness.engine->all_edges()) {
            FT_CHECK_EQ(edge.provenance.evidence_type, PublicationType::Synthetic);
            FT_CHECK_EQ(edge.provenance.source, DiscoverySource::SyntheticGenerator);
        }
    }
}

FT_TEST(synthetic, scenarios_that_carry_follow_up_publications_converge) {
    for (const SyntheticScenario value :
         {SyntheticScenario::MultipleFabrics, SyntheticScenario::DeviceReplacement,
          SyntheticScenario::AttachmentMove, SyntheticScenario::StaleSnapshot,
          SyntheticScenario::AsymmetricPublisherScopes}) {
        ScenarioHarness harness;
        const SyntheticTopology topology = scenario(value);
        prepare(harness, topology);
        FT_CHECK(harness.engine->publish(harness.context(), topology.publication).committed());
        for (const Publication& follow_up : topology.secondary_publications) {
            const PublicationResult result = harness.engine->publish(harness.context(), follow_up);
            if (!result.committed() && result.status.outcome() != Outcome::Idempotent) {
                FT_FAIL(std::string("scenario ") + to_string(value) + " follow-up: " +
                        result.status.one_line());
            }
        }
        const ValidationReport report = harness.engine->validate();
        if (!report.valid) {
            FT_FAIL(std::string("scenario ") + to_string(value) + ": " + report.render());
        }
        FT_CHECK(harness.engine->verify_integrity().outcome() == Outcome::Ok);
    }
}

FT_TEST(synthetic, scenario_metadata_is_complete) {
    for (const SyntheticScenario value : synthetic_scenarios()) {
        const SyntheticTopology topology = scenario(value, 2, 2);
        FT_CHECK(!topology.name.empty());
        FT_CHECK(!topology.description.empty());
        FT_CHECK(!topology.publication.id.empty());
        FT_CHECK(topology.render().find("SYNTHETIC") != std::string::npos);
        FT_CHECK(synthetic_scenario_from_string(to_string(value)).has_value());
    }
    FT_CHECK(!synthetic_scenario_from_string("not-a-scenario").has_value());
}

FT_TEST(synthetic, large_scale_scenario_is_bounded_by_explicit_scale) {
    ScenarioHarness harness;
    const SyntheticTopology topology = scenario(SyntheticScenario::LargeScale, 40, 3);
    prepare(harness, topology);
    FT_CHECK(harness.engine->publish(harness.context(), topology.publication).committed());
    FT_CHECK(harness.engine->node_count() > 200);
    FT_CHECK(harness.engine->validate().valid);
}

FT_TEST(synthetic, logical_overlay_keeps_physical_and_logical_apart) {
    ScenarioHarness harness;
    const SyntheticTopology topology = scenario(SyntheticScenario::LogicalOverlay);
    prepare(harness, topology);
    FT_CHECK(harness.engine->publish(harness.context(), topology.publication).committed());
    const std::vector<TopologyEdge> physical = harness.engine->physical_relationships();
    const std::vector<TopologyEdge> logical = harness.engine->logical_relationships();
    FT_CHECK(!physical.empty());
    FT_CHECK(!logical.empty());
    for (const TopologyEdge& edge : physical) {
        FT_CHECK_EQ(edge.layer, TopologyLayer::Physical);
    }
    for (const TopologyEdge& edge : logical) {
        FT_CHECK_EQ(edge.layer, TopologyLayer::Logical);
    }
    FT_CHECK_EQ(physical.size() + logical.size(), harness.engine->edge_count());
}
