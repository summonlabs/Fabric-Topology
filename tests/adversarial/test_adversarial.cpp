// Fabric Topology - adversarial hardening tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// The break-it pass: malformed identifiers, wrong classes, cross-scope injection, stale
// generations, stale boots, stale epochs, duplicate identities, oversized inputs, overflow,
// pathological graph shapes and corrupted protocol frames.

#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

FT_TEST(adversarial, malformed_identifiers_never_parse) {
    for (const char* candidate : {"", " ", "-leading-dash", "a b", "a/b", "a\\b", "a\nb",
                                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        FT_CHECK(!TopologyNodeId::parse(candidate).has_value());
    }
    FT_CHECK(TopologyNodeId::parse("n-valid.1_2~3:4").has_value());
    const auto parsed = TopologyNodeId::parse("n-valid");
    FT_CHECK(parsed.has_value());
    FT_CHECK_EQ(parsed->to_string(), std::string("n-valid"));
    FT_CHECK(parsed->hash() == TopologyNodeId::parse("n-valid")->hash());
}

FT_TEST(adversarial, identity_classes_are_not_interchangeable) {
    const TopologyNodeId node = TopologyNodeId::from_trusted("x-1");
    const TopologyEdgeId edge = TopologyEdgeId::from_trusted("x-1");
    // The two are distinct types; equality across classes is a compile-time error, and their
    // textual encodings are equal only because the caller chose the same text.
    FT_CHECK_EQ(node.value(), edge.value());
    FT_CHECK_EQ(node.hash(), edge.hash());
}

FT_TEST(adversarial, cross_domain_injection_is_refused) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyDomainId other = TopologyDomainId::from_trusted("dom-victim");
    DomainDefinition definition;
    definition.id = other;
    definition.kind = ScopeKind::AdministrativeDomain;
    FT_CHECK(fixture.engine->define_domain(definition).outcome() == Outcome::Committed);

    const PublisherId attacker = PublisherId::from_trusted("pub-attacker");
    const WorkerBootId boot = WorkerBootId::from_trusted("boot-attacker");
    PublisherRegistration registration;
    registration.publisher = attacker;
    registration.worker_boot = boot;
    registration.coordinator_epoch = fixture.engine->coordinator_epoch();
    ScopeGrant grant;
    grant.domain = fixture.domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    FT_CHECK(outcome_is_success(fixture.engine->register_publisher(registration).outcome()));

    AuthorityContext context;
    context.coordinator_epoch = fixture.engine->coordinator_epoch();
    context.publisher = attacker;
    context.worker_boot = boot;

    static_cast<void>(fixture.add_entity("ent-victim", EntityClass::Switch));
    AddNodeRequest request;
    request.entity_id = "ent-victim";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = other;
    FT_CHECK_EQ(fixture.engine->add_node(context, request).status.outcome(), Outcome::UnauthorizedScope);

    Publication publication;
    publication.id = PublicationId::from_trusted("pub-injection");
    publication.domain = other;
    publication.mode = PublicationMode::Incremental;
    publication.nodes.push_back(ObservedNode{});
    publication.nodes.front().entity_id = "ent-victim";
    publication.nodes.front().node_class = NodeClass::Switch;
    FT_CHECK_EQ(fixture.engine->publish(context, publication).status.outcome(),
                Outcome::UnauthorizedScope);
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{0});
}

FT_TEST(adversarial, stale_replay_from_a_fenced_boot_is_refused) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    const std::vector<TopologyEdge> edges = fixture.engine->all_edges();
    FT_CHECK(!edges.empty());

    const WorkerBootId stale = fixture.boot;
    static_cast<void>(fixture.engine->fence_boot(stale, "process died", true));

    // Replay the exact same relationships from the stale incarnation.
    for (const TopologyEdge& edge : edges) {
        UpdateEvidenceRequest request;
        request.edge = edge.id;
        request.evidence_generation = EvidenceGeneration{99};
        FT_CHECK_EQ(fixture.engine->update_relationship_evidence(fixture.context(), request).status.outcome(),
                    Outcome::StaleWorkerBoot);
    }
    for (const TopologyEdge& edge : fixture.engine->all_edges()) {
        FT_CHECK_EQ(edge.lifecycle, LifecycleState::RevalidationRequired);
    }
}

FT_TEST(adversarial, stale_epoch_replay_is_refused) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(fixture.add_entity("ent-sw", EntityClass::Switch));
    AuthorityContext stale = fixture.context();
    static_cast<void>(fixture.engine->advance_coordinator_epoch("restart"));
    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = fixture.domain;
    FT_CHECK_EQ(fixture.engine->add_node(stale, request).status.outcome(),
                Outcome::StaleCoordinatorEpoch);
}

FT_TEST(adversarial, duplicate_identity_injection) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(fixture.add_entity("ent-sw", EntityClass::Switch));
    Publication publication;
    publication.id = PublicationId::from_trusted("pub-duplicate");
    publication.domain = fixture.domain;
    publication.mode = PublicationMode::Incremental;
    for (int index = 0; index < 2; ++index) {
        ObservedNode node;
        node.entity_id = "ent-sw";
        node.node_class = NodeClass::Switch;
        node.tier = TopologyTier::Leaf;
        node.domain = fixture.domain;
        publication.nodes.push_back(node);
    }
    FT_CHECK_EQ(fixture.engine->publish(fixture.context(), publication).status.outcome(),
                Outcome::DuplicateIdentity);
}

FT_TEST(adversarial, invalid_relation_combination) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId fabric = fixture.add_node("ent-fabric", NodeClass::Fabric);
    const TopologyNodeId site = fixture.add_node("ent-site", NodeClass::Site, TopologyTier::SiteEdge);
    // A fabric cannot be a member of a site.
    FT_CHECK(fixture
                 .add_edge(RelationClass::MemberOf, fabric, site, TopologyLayer::Logical)
                 .empty());
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{0});
}

FT_TEST(adversarial, oversized_metadata_is_refused_without_allocation) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(fixture.add_entity("ent-sw", EntityClass::Switch));
    AddNodeRequest request;
    request.entity_id = "ent-sw";
    request.node_class = NodeClass::Switch;
    request.tier = TopologyTier::Leaf;
    request.domain = fixture.domain;
    const Limits limits;
    // Two hundred attempts against a bounded container never grow it beyond the limit, and the
    // rejected attempts are reported precisely.
    std::size_t refusals = 0;
    for (std::size_t index = 0; index < 200; ++index) {
        const Status status = request.metadata.set("k" + std::to_string(index), "v", limits);
        if (status.outcome() != Outcome::Ok) {
            FT_CHECK_EQ(status.outcome(), Outcome::ResourceLimit);
            ++refusals;
        }
    }
    FT_CHECK(refusals > 0);
    FT_CHECK(request.metadata.size() <= limits.max_metadata_entries);
    FT_CHECK(request.metadata.total_bytes() <= limits.max_metadata_total_bytes);
    FT_CHECK_EQ(request.metadata.set("k0", std::string(limits.max_metadata_value_bytes + 1, 'v'),
                                     limits)
                    .outcome(),
                Outcome::ResourceLimit);
    // The bounded metadata commits normally; nothing was silently widened.
    const MutationResult result = fixture.engine->add_node(fixture.context(), request);
    FT_CHECK(result.accepted());
    FT_CHECK(fixture.engine->node_count() == 1);
}

FT_TEST(adversarial, pathological_high_degree_node) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId hub = fixture.add_node("ent-hub", NodeClass::PhysicalPort, TopologyTier::Leaf);
    constexpr int kSpokes = 400;
    for (int index = 0; index < kSpokes; ++index) {
        const TopologyNodeId spoke = fixture.add_node("ent-spoke-" + std::to_string(index),
                                                      NodeClass::PhysicalPort, TopologyTier::Leaf);
        FT_CHECK(!fixture.add_edge(RelationClass::ConnectedTo, hub, spoke, std::nullopt).empty());
    }
    FT_CHECK_EQ(fixture.engine->edges_for_node(hub).size(), static_cast<std::size_t>(kSpokes));
    FT_CHECK_EQ(fixture.engine->neighbors(hub).size(), static_cast<std::size_t>(kSpokes));
    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);

    TraversalRequest request;
    request.kind = TraversalKind::ConnectedComponent;
    request.origin = hub;
    request.max_depth = 4;
    const TraversalResult result = fixture.engine->traverse(request);
    FT_CHECK_EQ(result.steps.size(), static_cast<std::size_t>(kSpokes));

    // Removing the hub must clean every index.
    RemoveNodeRequest remove;
    remove.node = hub;
    remove.cascade_relationships = true;
    FT_CHECK(fixture.engine->remove_node(fixture.context(), remove).accepted());
    FT_CHECK_EQ(fixture.engine->edge_count(), std::size_t{0});
    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
}

FT_TEST(adversarial, long_containment_chain_is_iterative) {
    ft_test::Fixture fixture = ft_test::make_fast_fixture();
    constexpr int kDepth = 4000;
    TopologyNodeId previous =
        fixture.add_node("ent-chain-0", NodeClass::RackNetworkDomain, TopologyTier::TopOfRack);
    for (int index = 1; index < kDepth; ++index) {
        const TopologyNodeId current = fixture.add_node("ent-chain-" + std::to_string(index),
                                                        NodeClass::RackNetworkDomain,
                                                        TopologyTier::TopOfRack);
        FT_CHECK(!fixture.add_edge(RelationClass::Contains, previous, current, TopologyLayer::Physical)
                       .empty());
        previous = current;
    }
    FT_CHECK(fixture.engine->validate().valid);

    // Traversal is iterative and depth-limited: a four-thousand deep chain neither recurses
    // nor is traversed beyond the configured bound.
    TraversalRequest request;
    request.kind = TraversalKind::Descendants;
    request.origin = fixture.engine->node_for_entity("ent-chain-0")->id;
    request.max_depth = 64;
    request.max_visited = 1000;
    const TraversalResult bounded = fixture.engine->traverse(request);
    FT_CHECK_EQ(bounded.steps.size(), std::size_t{64});
    FT_CHECK(!bounded.truncated);

    request.max_visited = 10;
    const TraversalResult truncated = fixture.engine->traverse(request);
    FT_CHECK_EQ(truncated.steps.size(), std::size_t{10});
    FT_CHECK(truncated.truncated);

    // Deepest-first ordering is stable across repeated runs.
    request.max_visited = 1000;
    const std::string first = fixture.engine->traverse(request).render();
    FT_CHECK_EQ(fixture.engine->traverse(request).render(), first);
}

FT_TEST(adversarial, repeated_connect_disconnect_cycles) {
    ft_test::Fixture fixture = ft_test::make_fast_fixture();
    constexpr int kLinks = 50;
    std::vector<TopologyNodeId> left;
    std::vector<TopologyNodeId> right;
    for (int index = 0; index < kLinks; ++index) {
        left.push_back(fixture.add_node("ent-left-" + std::to_string(index),
                                        NodeClass::PhysicalPort, TopologyTier::Leaf));
        right.push_back(fixture.add_node("ent-right-" + std::to_string(index),
                                         NodeClass::PhysicalPort, TopologyTier::Leaf));
    }
    for (int iteration = 0; iteration < kLinks; ++iteration) {
        AddRelationshipRequest request;
        request.relation = RelationClass::ConnectedTo;
        request.from = left[static_cast<std::size_t>(iteration)];
        request.to = right[static_cast<std::size_t>(iteration)];
        request.domain = fixture.domain;
        const MutationResult added = fixture.engine->add_relationship(fixture.context(), request);
        FT_CHECK(added.accepted());
        RetireRequest retire;
        retire.edge = *added.edge;
        FT_CHECK(fixture.engine->retire_relationship(fixture.context(), retire).accepted());
        FT_CHECK_EQ(fixture.engine->retire_relationship(fixture.context(), retire).status.outcome(),
                    Outcome::Idempotent);
    }
    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
    FT_CHECK(fixture.engine->validate().valid);
    // Every historical relationship stays inspectable and non-current.
    FT_CHECK_EQ(fixture.engine->edge_count(), static_cast<std::size_t>(kLinks));
    FT_CHECK(fixture.engine->edges_by_relation(RelationClass::ConnectedTo,
                                               EdgeFilter::current_only())
                 .empty());
}

FT_TEST(adversarial, malformed_protocol_frames_are_refused) {
    Limits limits;
    for (std::size_t length = 0; length < kFrameHeaderBytes + 4; ++length) {
        const std::string buffer(length, '\0');
        FrameHeader header;
        std::string payload;
        std::size_t consumed = 0;
        std::string error;
        const FrameStatus status =
            decode_frame(buffer, limits, header, payload, consumed, error);
        FT_CHECK(status == FrameStatus::Incomplete || status == FrameStatus::Malformed);
        FT_CHECK(payload.empty());
    }

    // A frame that declares a huge payload must be refused, not allocated.
    std::string frame = encode_frame(FrameHeader{}, "x");
    const std::uint32_t huge = std::numeric_limits<std::uint32_t>::max();
    for (unsigned index = 0; index < 4; ++index) {
        frame[20 + index] = static_cast<char>((huge >> (8U * index)) & 0xFFU);
    }
    FrameHeader header;
    std::string payload;
    std::size_t consumed = 0;
    std::string error;
    FT_CHECK_EQ(decode_frame(frame, limits, header, payload, consumed, error), FrameStatus::Malformed);
}

FT_TEST(adversarial, shutdown_under_active_mutation) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(fixture.add_entity("ent-sw", EntityClass::Switch));
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> committed{0};
    std::thread mutator([&] {
        while (!stop.load()) {
            AddNodeRequest request;
            request.entity_id = "ent-sw";
            request.node_class = NodeClass::Switch;
            request.tier = TopologyTier::Leaf;
            request.domain = fixture.domain;
            if (fixture.engine->add_node(fixture.context(), request).accepted()) {
                committed.fetch_add(1);
            }
        }
    });

    // The reader work must overlap a live writer: wait until the mutator has committed before
    // stopping it, otherwise the test would silently exercise nothing.
    for (int spin = 0; spin < 1000000 && committed.load() == 0; ++spin) {
        std::this_thread::yield();
    }
    FT_CHECK(committed.load() > 0);

    for (int index = 0; index < 200; ++index) {
        static_cast<void>(fixture.engine->statistics());
        static_cast<void>(fixture.engine->validate());
        static_cast<void>(fixture.engine->snapshot());
    }
    stop.store(true);
    mutator.join();
    if (fixture.engine->node_count() != 1) {
        FT_FAIL("unexpected node count after concurrent mutation: " +
                std::to_string(fixture.engine->node_count()) + " generation=" +
                fixture.engine->generation().to_string() + " detail=" +
                fixture.engine->statistics());
    }
    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
}

FT_TEST(adversarial, unsupported_and_unknown_requests_are_named) {
    FT_CHECK_EQ(std::string(to_string(Outcome::UnsupportedOperation)), std::string("UNSUPPORTED_OPERATION"));
    FT_CHECK_EQ(std::string(to_string(Outcome::MalformedRequest)), std::string("MALFORMED_REQUEST"));
    FT_CHECK_EQ(std::string(to_string(Outcome::StaleWorkerBoot)), std::string("STALE_WORKER_BOOT"));
    FT_CHECK_EQ(std::string(to_string(Outcome::StaleCoordinatorEpoch)),
                std::string("STALE_COORDINATOR_EPOCH"));
    FT_CHECK_EQ(std::string(to_string(Outcome::StaleEntityGeneration)),
                std::string("STALE_ENTITY_GENERATION"));
    FT_CHECK(!outcome_from_code(60000).has_value());
    FT_CHECK(!outcome_from_string("NOT_AN_OUTCOME").has_value());
}
