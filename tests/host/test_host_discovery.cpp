// Fabric Topology - REAL host discovery tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// These tests exercise REAL host-local evidence. They never claim external fabric structure.

#include <algorithm>
#include <string>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

FT_TEST(host, discovery_reports_real_evidence_or_unsupported) {
    if (!host_discovery_supported()) {
        FT_SKIP("no host discovery backend on this platform");
    }
    const HostTopologyEvidence evidence = discover_host_topology();
    FT_CHECK_EQ(evidence.platform, std::string("windows"));
    FT_CHECK(!evidence.host_name.empty());
    FT_CHECK(!evidence.unsupported.empty());
    const std::string note = host_discovery_capability_note();
    FT_CHECK(note.find("cannot") != std::string::npos || note.find("not ") != std::string::npos);
    if (evidence.interfaces.empty()) {
        FT_SKIP("no network adapters are present in this environment");
    }
    for (const HostInterfaceEvidence& interface : evidence.interfaces) {
        FT_CHECK(!interface.interface_guid.empty());
        FT_CHECK(TopologyNodeId::parse(interface.interface_guid).has_value());
    }
    FT_CHECK(std::is_sorted(evidence.interfaces.begin(), evidence.interfaces.end(),
                            [](const HostInterfaceEvidence& a, const HostInterfaceEvidence& b) {
                                return a.interface_guid < b.interface_guid;
                            }));
}

FT_TEST(host, publication_is_real_and_publishes_cleanly) {
    if (!host_discovery_supported()) {
        FT_SKIP("no host discovery backend on this platform");
    }
    const HostTopologyEvidence evidence = discover_host_topology();
    if (evidence.interfaces.empty()) {
        FT_SKIP("no network adapters are present in this environment");
    }

    HostDiscoveryOptions options;
    options.domain = TopologyDomainId::from_trusted("dom-host");
    options.publisher = PublisherId::from_trusted("pub-host");
    const HostDiscoveryResult discovery = build_host_publication(evidence, options);

    auto directory = std::make_shared<InMemoryEntityDirectory>();
    for (const EntityRecord& record : discovery.directory_entries) {
        static_cast<void>(directory->add(record));
    }
    FT_CHECK_EQ(discovery.publication.type, PublicationType::Real);
    FT_CHECK_EQ(discovery.publication.source, DiscoverySource::HostDiscovery);
    FT_CHECK(!discovery.publication.nodes.empty());

    TopologyEngineOptions engine_options;
    engine_options.directory = directory;
    engine_options.verify_indexes_on_mutation = true;
    TopologyEngine engine(engine_options);

    DomainDefinition definition;
    definition.id = options.domain;
    definition.kind = ScopeKind::AdministrativeDomain;
    FT_CHECK(engine.define_domain(definition).outcome() == Outcome::Committed);

    const WorkerBootId boot = WorkerBootId::from_trusted("boot-host");
    PublisherRegistration registration;
    registration.publisher = options.publisher;
    registration.worker_boot = boot;
    registration.coordinator_epoch = engine.coordinator_epoch();
    ScopeGrant grant;
    grant.domain = options.domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    FT_CHECK(outcome_is_success(engine.register_publisher(registration).outcome()));

    AuthorityContext context;
    context.coordinator_epoch = engine.coordinator_epoch();
    context.publisher = options.publisher;
    context.worker_boot = boot;

    const PublicationResult result = engine.publish(context, discovery.publication);
    if (!result.committed()) {
        FT_FAIL(result.status.one_line());
    }
    FT_CHECK(engine.node_count() >= evidence.interfaces.size());
    FT_CHECK(engine.validate().valid);
    FT_CHECK(engine.verify_integrity().outcome() == Outcome::Ok);
    // Every published relationship carries REAL host-local provenance.
    for (const TopologyEdge& edge : engine.all_edges()) {
        FT_CHECK_EQ(edge.provenance.evidence_type, PublicationType::Real);
        FT_CHECK_EQ(edge.provenance.source, DiscoverySource::HostDiscovery);
    }
}
