// Fabric Topology - persistence and conservative recovery tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

namespace {

struct TempDirectory {
    std::filesystem::path path;

    explicit TempDirectory(const std::string& name) {
        path = std::filesystem::temp_directory_path() / ("fabric_topology_" + name);
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    [[nodiscard]] std::string file(const std::string& name) const {
        return (path / name).string();
    }
};

std::shared_ptr<InMemoryEntityDirectory> seed_directory() {
    auto directory = std::make_shared<InMemoryEntityDirectory>();
    for (const auto& entry : {std::pair<const char*, EntityClass>{"ent-fabric", EntityClass::Fabric},
                              {"ent-sw", EntityClass::Switch},
                              {"ent-sw-p0", EntityClass::Port},
                              {"ent-nic", EntityClass::Nic},
                              {"ent-nic-p0", EntityClass::Port}}) {
        EntityRecord record;
        record.entity_id = entry.first;
        record.entity_class = entry.second;
        record.generation = EntityGeneration{1};
        static_cast<void>(directory->add(record));
    }
    return directory;
}

void build_demo_topology(TopologyEngine& engine, const TopologyDomainId& domain) {
    DomainDefinition definition;
    definition.id = domain;
    static_cast<void>(engine.define_domain(definition));

    const PublisherId publisher = PublisherId::from_trusted("pub-persist");
    const WorkerBootId boot = WorkerBootId::from_trusted("boot-persist");
    PublisherRegistration registration;
    registration.publisher = publisher;
    registration.worker_boot = boot;
    registration.coordinator_epoch = engine.coordinator_epoch();
    ScopeGrant grant;
    grant.domain = domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    static_cast<void>(engine.register_publisher(registration));

    AuthorityContext context;
    context.coordinator_epoch = engine.coordinator_epoch();
    context.publisher = publisher;
    context.worker_boot = boot;

    for (const auto& entry : {std::pair<const char*, NodeClass>{"ent-fabric", NodeClass::Fabric},
                              {"ent-sw", NodeClass::Switch},
                              {"ent-sw-p0", NodeClass::PhysicalPort},
                              {"ent-nic", NodeClass::Nic},
                              {"ent-nic-p0", NodeClass::PhysicalPort}}) {
        AddNodeRequest request;
        request.entity_id = entry.first;
        request.node_class = entry.second;
        request.tier = TopologyTier::Leaf;
        request.domain = domain;
        static_cast<void>(engine.add_node(context, request));
    }

    AddRelationshipRequest membership;
    membership.relation = RelationClass::MemberOf;
    membership.from = engine.node_for_entity("ent-sw")->id;
    membership.to = engine.node_for_entity("ent-fabric")->id;
    membership.domain = domain;
    membership.layer = TopologyLayer::Logical;
    static_cast<void>(engine.add_relationship(context, membership));

    AddRelationshipRequest link;
    link.relation = RelationClass::ConnectedTo;
    link.from = engine.node_for_entity("ent-nic-p0")->id;
    link.to = engine.node_for_entity("ent-sw-p0")->id;
    link.domain = domain;
    static_cast<void>(engine.add_relationship(context, link));
}

}  // namespace

FT_TEST(persistence, round_trip_preserves_durable_structure) {
    const TempDirectory temp("round_trip");
    const std::string path = temp.file("state.ftstate");
    const TopologyDomainId domain = TopologyDomainId::from_trusted("dom-persist");

    std::string digest_before;
    TopologyGeneration generation_before;
    CoordinatorEpoch epoch_before;
    std::size_t nodes_before = 0;
    std::size_t edges_before = 0;
    {
        auto directory = seed_directory();
        TopologyEngineOptions options;
        options.directory = directory;
        options.persistence_path = path;
        options.persistence_mode = PersistenceMode::Immediate;
        TopologyEngine engine(options);
        build_demo_topology(engine, domain);
        digest_before = engine.digest();
        generation_before = engine.generation();
        epoch_before = engine.coordinator_epoch();
        nodes_before = engine.node_count();
        edges_before = engine.edge_count();
        FT_CHECK_EQ(engine.save().outcome(), Outcome::Ok);
        FT_CHECK(std::filesystem::exists(path));
    }

    PersistenceHeaderInfo header;
    const Status inspected = inspect_persistence_file(path, header);
    if (inspected.outcome() != Outcome::Ok) {
        FT_FAIL(inspected.render() + " file_bytes=" +
                std::to_string(std::filesystem::file_size(path)));
    }
    FT_CHECK_EQ(header.format_version, kPersistenceFormatVersion);
    FT_CHECK_EQ(header.payload_sha256.size(), std::size_t{64});

    auto directory = seed_directory();
    TopologyEngineOptions options;
    options.directory = directory;
    options.persistence_path = path;
    LoadReport report;
    const std::unique_ptr<TopologyEngine> engine = TopologyEngine::open(options, report);
    FT_CHECK(report.loaded);
    FT_CHECK(report.recovered);
    FT_CHECK_EQ(engine->node_count(), nodes_before);
    FT_CHECK_EQ(engine->edge_count(), edges_before);
    FT_CHECK_EQ(engine->generation(), generation_before);
    FT_CHECK_EQ(report.coordinator_epoch.value(), epoch_before.value() + 1);
    FT_CHECK_EQ(engine->coordinator_epoch(), report.coordinator_epoch);
    FT_CHECK(engine->validate().valid);
    const Status integrity = engine->verify_integrity();
    if (integrity.outcome() != Outcome::Ok) {
        FT_FAIL(integrity.render() + " nodes=" + std::to_string(engine->node_count()) +
                " edges=" + std::to_string(engine->edge_count()) + " domains=" +
                std::to_string(engine->domains().size()));
    }
    // Recovery is not a topology mutation: the generation is preserved.
    FT_CHECK_EQ(engine->generation(), generation_before);
    FT_CHECK(!digest_before.empty());
}

FT_TEST(persistence, recovery_demotes_published_evidence) {
    const TempDirectory temp("recovery_demote");
    const std::string path = temp.file("state.ftstate");
    const TopologyDomainId domain = TopologyDomainId::from_trusted("dom-persist");

    {
        auto directory = seed_directory();
        TopologyEngineOptions options;
        options.directory = directory;
        options.persistence_path = path;
        options.persistence_mode = PersistenceMode::Immediate;
        TopologyEngine engine(options);
        build_demo_topology(engine, domain);
        FT_CHECK_EQ(engine.validate().current_edges, std::size_t{2});
    }

    auto directory = seed_directory();
    TopologyEngineOptions options;
    options.directory = directory;
    options.persistence_path = path;
    LoadReport report;
    const std::unique_ptr<TopologyEngine> engine = TopologyEngine::open(options, report);
    FT_CHECK(report.loaded);
    FT_CHECK_EQ(report.revalidation_required_edges, std::size_t{2});
    const ValidationReport validation = engine->validate();
    FT_CHECK(validation.valid);
    FT_CHECK_EQ(validation.current_edges, std::size_t{0});
    FT_CHECK_EQ(validation.non_current_edges, std::size_t{2});

    // Durable structure remains inspectable.
    FT_CHECK(engine->node_for_entity("ent-sw").has_value());
    FT_CHECK(engine->node_for_entity("ent-nic-p0").has_value());
}

FT_TEST(persistence, revalidation_restores_currentness_under_fresh_authority) {
    const TempDirectory temp("revalidate");
    const std::string path = temp.file("state.ftstate");
    const TopologyDomainId domain = TopologyDomainId::from_trusted("dom-persist");

    {
        auto directory = seed_directory();
        TopologyEngineOptions options;
        options.directory = directory;
        options.persistence_path = path;
        TopologyEngine engine(options);
        build_demo_topology(engine, domain);
        FT_CHECK_EQ(engine.save().outcome(), Outcome::Ok);
    }

    auto directory = seed_directory();
    TopologyEngineOptions options;
    options.directory = directory;
    options.persistence_path = path;
    LoadReport report;
    const std::unique_ptr<TopologyEngine> engine = TopologyEngine::open(options, report);
    FT_CHECK(report.loaded);

    const PublisherId publisher = PublisherId::from_trusted("pub-fresh");
    const WorkerBootId boot = WorkerBootId::from_trusted("boot-fresh");
    PublisherRegistration registration;
    registration.publisher = publisher;
    registration.worker_boot = boot;
    registration.coordinator_epoch = engine->coordinator_epoch();
    ScopeGrant grant;
    grant.domain = domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    FT_CHECK(outcome_is_success(engine->register_publisher(registration).outcome()));

    AuthorityContext context;
    context.coordinator_epoch = engine->coordinator_epoch();
    context.publisher = publisher;
    context.worker_boot = boot;

    const TopologyGeneration recovered_generation = engine->generation();
    std::size_t revalidated = 0;
    for (const TopologyEdge& edge : engine->all_edges()) {
        RevalidateRequest request;
        request.edge = edge.id;
        request.evidence_generation = EvidenceGeneration{2};
        const MutationResult result = engine->revalidate_relationship(context, request);
        FT_CHECK(result.accepted());
        ++revalidated;
        FT_CHECK_EQ(engine->generation().value(), recovered_generation.value() + revalidated);
    }
    FT_CHECK_EQ(revalidated, std::size_t{2});
    FT_CHECK_EQ(engine->validate().current_edges, std::size_t{2});
    FT_CHECK(engine->validate().valid);
}

FT_TEST(persistence, fenced_worker_boots_survive_recovery) {
    const TempDirectory temp("fenced_boots");
    const std::string path = temp.file("state.ftstate");
    const TopologyDomainId domain = TopologyDomainId::from_trusted("dom-persist");
    const WorkerBootId fenced = WorkerBootId::from_trusted("boot-permanently-fenced");

    {
        auto directory = seed_directory();
        TopologyEngineOptions options;
        options.directory = directory;
        options.persistence_path = path;
        TopologyEngine engine(options);
        build_demo_topology(engine, domain);
        static_cast<void>(engine.fence_boot(fenced, "worker died", true));
        FT_CHECK_EQ(engine.save().outcome(), Outcome::Ok);
    }

    auto directory = seed_directory();
    TopologyEngineOptions options;
    options.directory = directory;
    options.persistence_path = path;
    LoadReport report;
    const std::unique_ptr<TopologyEngine> engine = TopologyEngine::open(options, report);
    FT_CHECK(report.loaded);

    PublisherRegistration registration;
    registration.publisher = PublisherId::from_trusted("pub-revived");
    registration.worker_boot = fenced;
    registration.coordinator_epoch = engine->coordinator_epoch();
    ScopeGrant grant;
    grant.domain = domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    FT_CHECK_EQ(engine->register_publisher(registration).outcome(), Outcome::StaleWorkerBoot);
}

FT_TEST(persistence, absent_file_is_not_an_error) {
    const TempDirectory temp("absent");
    auto directory = seed_directory();
    TopologyEngineOptions options;
    options.directory = directory;
    options.persistence_path = temp.file("does-not-exist.ftstate");
    LoadReport report;
    const std::unique_ptr<TopologyEngine> engine = TopologyEngine::open(options, report);
    FT_CHECK(!report.loaded);
    FT_CHECK_EQ(report.status.outcome(), Outcome::Ok);
    FT_CHECK_EQ(engine->node_count(), std::size_t{0});
}

FT_TEST(persistence, immediate_mode_keeps_disk_and_memory_in_step) {
    const TempDirectory temp("immediate");
    const std::string path = temp.file("live.ftstate");
    const TopologyDomainId domain = TopologyDomainId::from_trusted("dom-persist");

    auto directory = seed_directory();
    TopologyEngineOptions options;
    options.directory = directory;
    options.persistence_path = path;
    options.persistence_mode = PersistenceMode::Immediate;
    TopologyEngine engine(options);
    build_demo_topology(engine, domain);

    PersistenceHeaderInfo header;
    const Status inspected = inspect_persistence_file(path, header);
    if (inspected.outcome() != Outcome::Ok) {
        FT_FAIL(inspected.render() + " file_bytes=" +
                std::to_string(std::filesystem::file_size(path)));
    }

    auto reopened_directory = seed_directory();
    TopologyEngineOptions reopened_options;
    reopened_options.directory = reopened_directory;
    reopened_options.persistence_path = path;
    LoadReport report;
    const std::unique_ptr<TopologyEngine> reopened =
        TopologyEngine::open(reopened_options, report);
    FT_CHECK_EQ(reopened->node_count(), engine.node_count());
    FT_CHECK_EQ(reopened->edge_count(), engine.edge_count());
}
