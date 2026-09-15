// Fabric Topology - resource bound and checked arithmetic tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <cstdint>
#include <limits>
#include <string>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

FT_TEST(bounds, checked_arithmetic) {
    std::uint64_t out = 0;
    FT_CHECK(checked_add_u64(1, 2, out));
    FT_CHECK_EQ(out, std::uint64_t{3});
    FT_CHECK(checked_mul_u64(4, 5, out));
    FT_CHECK_EQ(out, std::uint64_t{20});
    FT_CHECK(!checked_add_u64((std::numeric_limits<std::uint64_t>::max)(), 1, out));
    FT_CHECK(!checked_mul_u64((std::numeric_limits<std::uint64_t>::max)(), 2, out));
    FT_CHECK(checked_mul_u64(0, (std::numeric_limits<std::uint64_t>::max)(), out));
    FT_CHECK_EQ(out, std::uint64_t{0});
}

FT_TEST(bounds, node_limit_is_enforced) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    TopologyEngineOptions options = ft_test::default_options(fixture.directory);
    options.limits.max_nodes = 3;
    TopologyEngine engine(options);
    DomainDefinition definition;
    definition.id = fixture.domain;
    static_cast<void>(engine.define_domain(definition));
    PublisherRegistration registration;
    registration.publisher = fixture.publisher;
    registration.worker_boot = fixture.boot;
    registration.coordinator_epoch = engine.coordinator_epoch();
    ScopeGrant grant;
    grant.domain = fixture.domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    static_cast<void>(engine.register_publisher(registration));

    AuthorityContext context;
    context.coordinator_epoch = engine.coordinator_epoch();
    context.publisher = fixture.publisher;
    context.worker_boot = fixture.boot;

    for (int index = 0; index < 5; ++index) {
        const std::string entity = "ent-limit-" + std::to_string(index);
        EntityRecord record;
        record.entity_id = entity;
        record.entity_class = EntityClass::Switch;
        record.generation = EntityGeneration{1};
        static_cast<void>(fixture.directory->add(record));
        AddNodeRequest request;
        request.entity_id = entity;
        request.node_class = NodeClass::Switch;
        request.domain = fixture.domain;
        const MutationResult result = engine.add_node(context, request);
        if (index < 3) {
            FT_CHECK(result.accepted());
        } else {
            FT_CHECK_EQ(result.status.outcome(), Outcome::ResourceLimit);
        }
    }
    FT_CHECK_EQ(engine.node_count(), std::size_t{3});
}

FT_TEST(bounds, publication_size_limits_are_enforced) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    TopologyEngineOptions options = ft_test::default_options(fixture.directory);
    options.limits.max_nodes_per_publication = 2;
    options.limits.max_edges_per_publication = 1;
    TopologyEngine engine(options);
    DomainDefinition definition;
    definition.id = fixture.domain;
    static_cast<void>(engine.define_domain(definition));
    PublisherRegistration registration;
    registration.publisher = fixture.publisher;
    registration.worker_boot = fixture.boot;
    registration.coordinator_epoch = engine.coordinator_epoch();
    ScopeGrant grant;
    grant.domain = fixture.domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    static_cast<void>(engine.register_publisher(registration));

    AuthorityContext context;
    context.coordinator_epoch = engine.coordinator_epoch();
    context.publisher = fixture.publisher;
    context.worker_boot = fixture.boot;

    Publication publication;
    publication.id = PublicationId::from_trusted("pub-too-many-nodes");
    publication.domain = fixture.domain;
    publication.mode = PublicationMode::Incremental;
    for (int index = 0; index < 3; ++index) {
        ObservedNode node;
        node.entity_id = "ent-" + std::to_string(index);
        node.node_class = NodeClass::Switch;
        node.domain = fixture.domain;
        publication.nodes.push_back(node);
    }
    FT_CHECK_EQ(engine.publish(context, publication).status.outcome(), Outcome::ResourceLimit);

    publication.id = PublicationId::from_trusted("pub-too-many-edges");
    publication.edges.resize(2);
    FT_CHECK_EQ(engine.publish(context, publication).status.outcome(), Outcome::ResourceLimit);
    FT_CHECK_EQ(engine.node_count(), std::size_t{0});
}

FT_TEST(bounds, note_and_identifier_bounds) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    Publication publication;
    publication.id = PublicationId::from_trusted("pub-note");
    publication.domain = fixture.domain;
    publication.mode = PublicationMode::Incremental;
    publication.note = std::string(Limits{}.max_string_bytes + 1, 'x');
    FT_CHECK_EQ(fixture.engine->publish(fixture.context(), publication).status.outcome(),
                Outcome::ResourceLimit);

    AddNodeRequest request;
    request.entity_id = std::string(400, 'a');
    request.node_class = NodeClass::Switch;
    request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_node(fixture.context(), request).status.outcome(),
                Outcome::ResourceLimit);
}

FT_TEST(bounds, metadata_bounds) {
    Limits limits;
    FT_CHECK(limits.max_metadata_entries >= 8);
    Metadata metadata;
    for (std::size_t index = 0; index < limits.max_metadata_entries; ++index) {
        FT_CHECK(metadata.set("k" + std::to_string(index), "v", limits).outcome() == Outcome::Ok);
    }
    FT_CHECK_EQ(metadata.set("overflow", "v", limits).outcome(), Outcome::ResourceLimit);
    FT_CHECK_EQ(metadata.set("k0", std::string(limits.max_metadata_value_bytes + 1, 'v'), limits)
                    .outcome(),
                Outcome::ResourceLimit);
    FT_CHECK_EQ(metadata.set(std::string(limits.max_metadata_key_bytes + 1, 'k'), "v", limits)
                    .outcome(),
                Outcome::MalformedRequest);
    FT_CHECK_EQ(metadata.set("control\nbyte", "v", limits).outcome(), Outcome::MalformedRequest);
    FT_CHECK_EQ(metadata.set("", "v", limits).outcome(), Outcome::MalformedRequest);
    FT_CHECK(metadata.erase("k0").outcome() == Outcome::Committed);
    FT_CHECK(metadata.erase("k0").outcome() == Outcome::NotFound);
}

FT_TEST(bounds, duplicate_metadata_keys_are_rejected_on_import) {
    std::vector<Metadata::Item> items;
    items.emplace_back("key", "one");
    items.emplace_back("key", "two");
    FT_CHECK_EQ(Metadata::validate(items, Limits{}).outcome(), Outcome::DuplicateIdentity);
}

FT_TEST(bounds, checked_reader_rejects_oversized_length_prefix) {
    RecordWriter writer(16);
    writer.u32(0xFFFFFFFFU);
    RecordReader reader(writer.data(), Limits{});
    FT_CHECK(reader.text(64).empty());
    FT_CHECK(!reader.ok());
    FT_CHECK_EQ(reader.error(), std::string("codec.length_over_limit"));
}

FT_TEST(bounds, reader_rejects_truncated_payloads) {
    RecordWriter writer(16);
    writer.u64(0x0102030405060708ULL);
    const std::string data = writer.take();
    for (std::size_t length = 0; length < data.size(); ++length) {
        RecordReader reader(std::string_view(data).substr(0, length), Limits{});
        static_cast<void>(reader.u64());
        FT_CHECK(!reader.ok());
    }
}

FT_TEST(bounds, traversal_limits_are_clamped) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId a = fixture.add_node("ent-a", NodeClass::PhysicalPort, TopologyTier::Leaf);
    static_cast<void>(a);
    TraversalRequest request;
    request.kind = TraversalKind::Neighbors;
    request.origin = a;
    request.max_depth = 1000000;
    request.max_visited = 100000000;
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK(result.status.outcome() == Outcome::Ok);
    FT_CHECK(result.status.has("depth_limit"));
}
