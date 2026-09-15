// Fabric Topology - seeded property and invariant testing.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Every case is driven by an explicit seed. A failure prints the seed and the reproduction
// parameters, so any counterexample can be replayed exactly.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

namespace {

/// Deterministic splitmix64 generator. No dependency on any standard library distribution.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    [[nodiscard]] std::size_t below(std::size_t bound) {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }

    [[nodiscard]] std::uint64_t state() const { return state_; }

private:
    std::uint64_t state_;
};

[[noreturn]] void property_failure(std::uint64_t seed, std::size_t step, const std::string& detail) {
    std::string message = "property violation seed=";
    message += std::to_string(seed);
    message += " step=";
    message += std::to_string(step);
    message += " detail=";
    message += detail;
    FT_FAIL(message);
}

void check_invariants(ft_test::Fixture& fixture, std::uint64_t seed, std::size_t step,
                      TopologyGeneration previous_generation) {
    const Status integrity = fixture.engine->verify_integrity();
    if (integrity.outcome() != Outcome::Ok) {
        property_failure(seed, step, integrity.one_line());
    }
    const ValidationReport report = fixture.engine->validate();
    if (!report.valid) {
        property_failure(seed, step, report.render());
    }
    if (fixture.engine->generation() < previous_generation) {
        property_failure(seed, step, "generation went backwards");
    }
}

struct RandomTopology {
    ft_test::Fixture fixture;
    std::vector<std::string> entity_ids;
};

void add_random_entity(RandomTopology& topology, Rng& rng, std::size_t index) {
    static constexpr NodeClass kClasses[] = {NodeClass::Switch,  NodeClass::PhysicalPort,
                                             NodeClass::Nic,     NodeClass::LogicalPort,
                                             NodeClass::Router,  NodeClass::FabricEndpoint};
    const NodeClass node_class = kClasses[rng.below(std::size(kClasses))];
    const std::string entity = "ent-prop-" + std::to_string(index);
    static_cast<void>(topology.fixture.add_entity(entity, node_class_entity_class(node_class)));
    AddNodeRequest request;
    request.entity_id = entity;
    request.node_class = node_class;
    request.tier = TopologyTier::Leaf;
    request.domain = topology.fixture.domain;
    const MutationResult result = topology.fixture.engine->add_node(topology.fixture.context(), request);
    if (result.accepted()) {
        topology.entity_ids.push_back(entity);
    }
}

}  // namespace

FT_TEST(property, random_mutation_sequences_preserve_invariants) {
    for (const std::uint64_t seed : {1ULL, 7ULL, 12345ULL, 0xDEADBEEFULL, 20260101ULL}) {
        RandomTopology topology;
        topology.fixture = ft_test::make_fixture();
        Rng rng(seed);
        TopologyGeneration generation = topology.fixture.engine->generation();

        for (std::size_t step = 0; step < 120; ++step) {
            const std::size_t action = rng.below(10);
            if (action < 4 || topology.entity_ids.size() < 3) {
                add_random_entity(topology, rng, step);
            } else {
                const std::string& from = topology.entity_ids[rng.below(topology.entity_ids.size())];
                const std::string& to = topology.entity_ids[rng.below(topology.entity_ids.size())];
                AddRelationshipRequest request;
                request.relation =
                    static_cast<RelationClass>(1 + rng.below(kRelationClassCount - 1));
                request.from = topology.fixture.engine->node_for_entity(from)->id;
                request.to = topology.fixture.engine->node_for_entity(to)->id;
                request.domain = topology.fixture.domain;
                if (relation_rules(request.relation).layer_policy == LayerPolicy::Either) {
                    request.layer = rng.below(2) == 0 ? TopologyLayer::Physical
                                                      : TopologyLayer::Logical;
                }
                const MutationResult result =
                    topology.fixture.engine->add_relationship(topology.fixture.context(), request);
                if (result.accepted() && result.changed()) {
                    generation = topology.fixture.engine->generation();
                }
            }
            check_invariants(topology.fixture, seed, step, generation);
        }
        FT_CHECK(topology.fixture.engine->node_count() > 0);
    }
}

FT_TEST(property, reconciliation_sequences_preserve_invariants) {
    for (const std::uint64_t seed : {2ULL, 99ULL, 424242ULL}) {
        ft_test::Fixture fixture = ft_test::make_fixture();
        Rng rng(seed);
        std::vector<std::string> entities;
        TopologyGeneration generation = fixture.engine->generation();

        for (std::size_t step = 0; step < 40; ++step) {
            const std::size_t count = 1 + rng.below(4);
            Publication publication;
            publication.id = PublicationId::from_trusted("pub-prop-" + std::to_string(step));
            publication.domain = fixture.domain;
            publication.mode = rng.below(4) == 0 ? PublicationMode::AuthoritativeSnapshot
                                                 : PublicationMode::Incremental;
            publication.type = PublicationType::Synthetic;
            publication.source = DiscoverySource::SyntheticGenerator;
            std::vector<std::string> publication_entities;
            for (std::size_t index = 0; index < count; ++index) {
                const std::string entity = "ent-rec-" + std::to_string(rng.below(6));
                // A publication may not declare the same participant twice.
                if (std::find(publication_entities.begin(), publication_entities.end(), entity) !=
                    publication_entities.end()) {
                    continue;
                }
                publication_entities.push_back(entity);
                if (std::find(entities.begin(), entities.end(), entity) == entities.end()) {
                    if (!fixture.directory->lookup(entity).has_value()) {
                        static_cast<void>(
                            fixture.add_entity(entity, node_class_entity_class(NodeClass::Switch)));
                    }
                    entities.push_back(entity);
                }
                if (publication_entities.empty()) {
                    break;
                }
                ObservedNode node;
                node.entity_id = entity;
                node.node_class = NodeClass::Switch;
                node.tier = TopologyTier::Leaf;
                node.domain = fixture.domain;
                node.entity_generation = EntityGeneration{1};
                node.node_id = TopologyNodeId::from_trusted("n-rec-" + entity);
                publication.nodes.push_back(node);
            }
            const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
            if (!result.committed() && result.status.outcome() != Outcome::Idempotent) {
                property_failure(seed, step, result.status.one_line());
            }
            if (result.generation_advanced) {
                generation = fixture.engine->generation();
            }
            check_invariants(fixture, seed, step, generation);
        }
    }
}

FT_TEST(property, digest_stability_under_repeated_reads) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    const std::string digest = fixture.engine->digest();
    const std::string canonical = fixture.engine->render_canonical();
    for (int index = 0; index < 32; ++index) {
        FT_CHECK_EQ(fixture.engine->digest(), digest);
        FT_CHECK_EQ(fixture.engine->render_canonical(), canonical);
        FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
    }
}

FT_TEST(property, retirement_prevents_resurrection) {
    for (const std::uint64_t seed : {5ULL, 77ULL, 8080ULL}) {
        ft_test::Fixture fixture = ft_test::make_fixture();
        Rng rng(seed);
        const TopologyNodeId a = fixture.add_node("ent-a", NodeClass::PhysicalPort, TopologyTier::Leaf);
        const TopologyNodeId b = fixture.add_node("ent-b", NodeClass::PhysicalPort, TopologyTier::Leaf);
        const TopologyEdgeId edge = fixture.add_edge(RelationClass::ConnectedTo, a, b, std::nullopt);
        FT_CHECK(!edge.empty());
        RetireRequest retire;
        retire.edge = edge;
        FT_CHECK(fixture.engine->retire_relationship(fixture.context(), retire).accepted());

        for (std::size_t step = 0; step < 20; ++step) {
            AddRelationshipRequest request;
            request.relation = RelationClass::ConnectedTo;
            request.from = rng.below(2) == 0 ? a : b;
            request.to = request.from == a ? b : a;
            request.domain = fixture.domain;
            const MutationResult result =
                fixture.engine->add_relationship(fixture.context(), request);
            FT_CHECK_EQ(result.status.outcome(), Outcome::Retired);
            const std::optional<TopologyEdge> current = fixture.engine->edge(edge);
            FT_CHECK(current.has_value());
            FT_CHECK_EQ(current->lifecycle, LifecycleState::Retired);
        }
    }
}

FT_TEST(property, large_scale_topology_holds_invariants) {
    ft_test::Fixture fixture = ft_test::make_fixture();

    constexpr std::size_t kSwitches = 2000;
    constexpr std::size_t kPortsPerSwitch = 5;
    Publication publication;
    publication.id = PublicationId::from_trusted("pub-large-scale");
    publication.domain = fixture.domain;
    publication.mode = PublicationMode::AuthoritativeSnapshot;
    publication.type = PublicationType::Synthetic;
    publication.source = DiscoverySource::SyntheticGenerator;

    static_cast<void>(fixture.add_entity("ent-ls-fabric", EntityClass::Fabric));
    ObservedNode fabric;
    fabric.entity_id = "ent-ls-fabric";
    fabric.node_class = NodeClass::Fabric;
    fabric.domain = fixture.domain;
    fabric.entity_generation = EntityGeneration{1};
    fabric.node_id = TopologyNodeId::from_trusted("n-ls-fabric");
    publication.nodes.push_back(fabric);

    std::vector<std::string> port_nodes;
    port_nodes.reserve(kSwitches * kPortsPerSwitch);
    for (std::size_t index = 0; index < kSwitches; ++index) {
        const std::string switch_entity = "ent-ls-sw-" + std::to_string(index);
        static_cast<void>(fixture.add_entity(switch_entity, EntityClass::Switch));
        ObservedNode sw;
        sw.entity_id = switch_entity;
        sw.node_class = NodeClass::Switch;
        sw.tier = TopologyTier::Leaf;
        sw.domain = fixture.domain;
        sw.entity_generation = EntityGeneration{1};
        sw.node_id = TopologyNodeId::from_trusted("n-ls-sw-" + std::to_string(index));
        publication.nodes.push_back(sw);

        ObservedEdge membership;
        membership.relation = RelationClass::MemberOf;
        membership.from = sw.node_id;
        membership.to = fabric.node_id;
        membership.layer = TopologyLayer::Logical;
        membership.domain = fixture.domain;
        membership.evidence_generation = EvidenceGeneration{1};
        publication.edges.push_back(membership);

        for (std::size_t port = 0; port < kPortsPerSwitch; ++port) {
            const std::string port_entity =
                switch_entity + "-p" + std::to_string(port);
            static_cast<void>(fixture.add_entity(port_entity, EntityClass::Port));
            ObservedNode port_node;
            port_node.entity_id = port_entity;
            port_node.node_class = NodeClass::PhysicalPort;
            port_node.tier = TopologyTier::Leaf;
            port_node.domain = fixture.domain;
            port_node.entity_generation = EntityGeneration{1};
            port_node.node_id =
                TopologyNodeId::from_trusted("n-ls-p-" + std::to_string(index) + "-" +
                                             std::to_string(port));
            publication.nodes.push_back(port_node);
            port_nodes.push_back(port_node.node_id.value());

            ObservedEdge presents;
            presents.relation = RelationClass::PresentsEndpoint;
            presents.from = sw.node_id;
            presents.to = port_node.node_id;
            presents.layer = TopologyLayer::Physical;
            presents.domain = fixture.domain;
            presents.evidence_generation = EvidenceGeneration{1};
            publication.edges.push_back(presents);
        }
    }

    // A dense but valid connectivity mesh: ten connections per port slot.
    const std::size_t connections_per_port = 30;
    for (std::size_t index = 0; index < port_nodes.size(); ++index) {
        for (std::size_t link = 1; link <= connections_per_port; ++link) {
            const std::size_t peer = (index * 7 + link * 131) % port_nodes.size();
            // Connectivity is undirected: emitting both orientations would declare the same
            // relationship twice, which the runtime correctly refuses.
            if (peer <= index) {
                continue;
            }
            ObservedEdge edge;
            edge.relation = RelationClass::ConnectedTo;
            edge.from = TopologyNodeId::from_trusted(port_nodes[index]);
            edge.to = TopologyNodeId::from_trusted(port_nodes[peer]);
            edge.domain = fixture.domain;
            edge.evidence_generation = EvidenceGeneration{1};
            publication.edges.push_back(edge);
        }
    }

    const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
    if (!result.committed()) {
        FT_FAIL(result.status.one_line());
    }
    FT_CHECK(fixture.engine->node_count() >= 10000);
    FT_CHECK(fixture.engine->edge_count() >= 100000);
    FT_CHECK(fixture.engine->validate().valid);
    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);

    const std::string first = fixture.engine->digest();
    FT_CHECK_EQ(fixture.engine->digest(), first);
    const TopologySnapshot snapshot = fixture.engine->snapshot();
    FT_CHECK_EQ(snapshot.node_count(), fixture.engine->node_count());
    FT_CHECK(fixture.engine->check_snapshot(snapshot).current);
}
