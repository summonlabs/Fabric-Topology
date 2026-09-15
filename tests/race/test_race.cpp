// Fabric Topology - deterministic concurrency and race tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Interleavings are forced with barriers and latches. The assertions are about legal
// deterministic outcomes of the serialised mutation path, not about scheduling luck.

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

namespace {

TopologyNodeId make_port(ft_test::Fixture& fixture, const std::string& entity) {
    return fixture.add_node(entity, NodeClass::PhysicalPort, TopologyTier::Leaf);
}

}  // namespace

FT_TEST(race, two_publishers_add_the_same_edge) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId a = make_port(fixture, "ent-a");
    const TopologyNodeId b = make_port(fixture, "ent-b");

    for (int iteration = 0; iteration < 25; ++iteration) {
        std::barrier start(3);
        std::atomic<int> accepted{0};
        std::atomic<int> idempotent{0};
        std::vector<std::thread> threads;
        for (int worker = 0; worker < 2; ++worker) {
            threads.emplace_back([&] {
                start.arrive_and_wait();
                AddRelationshipRequest request;
                request.relation = RelationClass::ConnectedTo;
                request.from = a;
                request.to = b;
                request.domain = fixture.domain;
                const MutationResult result =
                    fixture.engine->add_relationship(fixture.context(), request);
                if (result.status.outcome() == Outcome::Committed) {
                    accepted.fetch_add(1);
                } else if (result.status.outcome() == Outcome::Idempotent) {
                    idempotent.fetch_add(1);
                }
            });
        }
        start.arrive_and_wait();
        for (std::thread& thread : threads) {
            thread.join();
        }
        FT_CHECK_EQ(accepted.load() + idempotent.load(), 2);
        FT_CHECK_EQ(fixture.engine->edges_by_relation(RelationClass::ConnectedTo).size(), std::size_t{1});
        FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
    }
}

FT_TEST(race, concurrent_conflicting_attachments) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    const TopologyNodeId nic_port = make_port(fixture, "ent-nic-p0");
    const TopologyNodeId sw_a = fixture.add_node("ent-sw-a", NodeClass::Switch, TopologyTier::Leaf);
    const TopologyNodeId sw_b = fixture.add_node("ent-sw-b", NodeClass::Switch, TopologyTier::Leaf);

    std::barrier start(3);
    std::atomic<int> conflicts{0};
    std::atomic<int> committed{0};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 2; ++worker) {
        threads.emplace_back([&, worker] {
            start.arrive_and_wait();
            AttachEndpointRequest request;
            request.endpoint = nic_port;
            request.target = worker == 0 ? sw_a : sw_b;
            request.domain = fixture.domain;
            request.attachment = AttachmentId::from_trusted("att-slot");
            const MutationResult result = fixture.engine->attach_endpoint(fixture.context(), request);
            if (result.status.outcome() == Outcome::Committed) {
                committed.fetch_add(1);
            } else if (result.status.outcome() == Outcome::RelationshipConflict) {
                conflicts.fetch_add(1);
            }
        });
    }
    start.arrive_and_wait();
    for (std::thread& thread : threads) {
        thread.join();
    }
    FT_CHECK_EQ(committed.load(), 1);
    FT_CHECK_EQ(conflicts.load(), 1);

    // Exactly one current exclusive attachment remains.
    std::size_t current = 0;
    for (const TopologyEdge& edge : fixture.engine->edges_by_relation(RelationClass::AttachedTo)) {
        if (edge.lifecycle == LifecycleState::Current) {
            ++current;
        }
    }
    FT_CHECK_EQ(current, std::size_t{1});
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(race, retirement_races_with_update) {
    for (int iteration = 0; iteration < 25; ++iteration) {
        ft_test::Fixture fixture = ft_test::make_fixture();
        const TopologyNodeId a = make_port(fixture, "ent-a");
        const TopologyNodeId b = make_port(fixture, "ent-b");
        const TopologyEdgeId edge = fixture.add_edge(RelationClass::ConnectedTo, a, b, std::nullopt);
        FT_CHECK(!edge.empty());

        std::barrier start(3);
        std::vector<std::thread> threads;
        threads.emplace_back([&] {
            start.arrive_and_wait();
            RetireRequest request;
            request.edge = edge;
            static_cast<void>(fixture.engine->retire_relationship(fixture.context(), request));
        });
        threads.emplace_back([&] {
            start.arrive_and_wait();
            UpdateEvidenceRequest request;
            request.edge = edge;
            request.evidence_generation = EvidenceGeneration{3};
            static_cast<void>(fixture.engine->update_relationship_evidence(fixture.context(), request));
        });
        start.arrive_and_wait();
        for (std::thread& thread : threads) {
            thread.join();
        }
        const std::optional<TopologyEdge> final_edge = fixture.engine->edge(edge);
        FT_CHECK(final_edge.has_value());
        // A retired relationship can never be brought back to current by a racing update.
        if (final_edge->lifecycle == LifecycleState::Current) {
            FT_CHECK_EQ(final_edge->evidence_generation.value(), std::uint64_t{3});
        } else {
            FT_CHECK_EQ(final_edge->lifecycle, LifecycleState::Retired);
        }
        FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
    }
}

FT_TEST(race, fencing_races_with_publication) {
    for (int iteration = 0; iteration < 25; ++iteration) {
        ft_test::Fixture fixture = ft_test::make_fixture();
        static_cast<void>(fixture.add_entity("ent-sw", EntityClass::Switch));

        std::barrier start(3);
        std::atomic<bool> published{false};
        std::atomic<bool> fenced{false};
        std::vector<std::thread> threads;
        threads.emplace_back([&] {
            start.arrive_and_wait();
            Publication publication;
            publication.id = PublicationId::from_trusted("pub-race-" + std::to_string(iteration));
            publication.domain = fixture.domain;
            publication.mode = PublicationMode::Incremental;
            ObservedNode node;
            node.entity_id = "ent-sw";
            node.node_class = NodeClass::Switch;
            node.tier = TopologyTier::Leaf;
            node.domain = fixture.domain;
            node.entity_generation = EntityGeneration{1};
            publication.nodes.push_back(node);
            const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
            // Legal outcomes: committed before the fence, or refused because the incarnation
            // was already fenced when the publication arrived.
            published.store(result.committed() || result.status.outcome() == Outcome::Idempotent ||
                            result.status.outcome() == Outcome::StaleAuthority ||
                            result.status.outcome() == Outcome::StaleWorkerBoot);
        });
        threads.emplace_back([&] {
            start.arrive_and_wait();
            const Status status =
                fixture.engine->fence_publisher(fixture.publisher, fixture.boot, "race");
            fenced.store(outcome_is_success(status.outcome()));
        });
        start.arrive_and_wait();
        for (std::thread& thread : threads) {
            thread.join();
        }
        FT_CHECK(published.load());
        FT_CHECK(fenced.load());
        // Whatever the order, the publisher ends fenced and no current edge survives it.
        const std::optional<PublisherState> state = fixture.engine->publisher_state(fixture.publisher);
        FT_CHECK(state.has_value());
        FT_CHECK(state->fenced);
        for (const TopologyEdge& edge : fixture.engine->all_edges()) {
            FT_CHECK_EQ(edge.lifecycle, LifecycleState::RevalidationRequired);
        }
        FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
    }
}

FT_TEST(race, epoch_change_races_with_mutation) {
    for (int iteration = 0; iteration < 25; ++iteration) {
        ft_test::Fixture fixture = ft_test::make_fixture();
        static_cast<void>(fixture.add_entity("ent-sw", EntityClass::Switch));
        std::barrier start(3);
        std::atomic<int> outcome{0};
        std::vector<std::thread> threads;
        threads.emplace_back([&] {
            start.arrive_and_wait();
            AddNodeRequest request;
            request.entity_id = "ent-sw";
            request.node_class = NodeClass::Switch;
            request.tier = TopologyTier::Leaf;
            request.domain = fixture.domain;
            const MutationResult result = fixture.engine->add_node(fixture.context(), request);
            outcome.store(static_cast<int>(result.status.outcome()));
        });
        threads.emplace_back([&] {
            start.arrive_and_wait();
            static_cast<void>(fixture.engine->advance_coordinator_epoch("race"));
        });
        start.arrive_and_wait();
        for (std::thread& thread : threads) {
            thread.join();
        }
        const Outcome observed = static_cast<Outcome>(outcome.load());
        // Legal outcomes: the mutation committed before the epoch turned over, or it was
        // refused because the epoch had already advanced (StaleCoordinatorEpoch), or because
        // the context was read after the advance cleared process-local authority
        // (StaleAuthority). Nothing else is acceptable.
        FT_CHECK(observed == Outcome::Committed || observed == Outcome::StaleAuthority ||
                 observed == Outcome::StaleCoordinatorEpoch);
        FT_CHECK_EQ(fixture.engine->node_count(),
                    observed == Outcome::Committed ? std::size_t{1} : std::size_t{0});
        // Whatever the order, no publisher authority survives the epoch change.
        FT_CHECK(!fixture.engine->publisher_state(fixture.publisher).has_value());
        FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
    }
}

FT_TEST(race, snapshot_races_with_mutation) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    for (int index = 0; index < 40; ++index) {
        static_cast<void>(
            fixture.add_node("ent-pre-" + std::to_string(index), NodeClass::Switch, TopologyTier::Leaf));
    }

    std::atomic<bool> stop{false};
    std::atomic<int> snapshots{0};
    std::atomic<int> inconsistent{0};
    std::thread reader([&] {
        while (!stop.load()) {
            const TopologySnapshot snapshot = fixture.engine->snapshot();
            // A snapshot taken under the state lock is internally consistent: its content is
            // sorted, self-describing and never ahead of the runtime generation.
            if (snapshot.digest.empty() || snapshot.node_count() != snapshot.nodes.size() ||
                snapshot.edge_count() != snapshot.edges.size() ||
                snapshot.topology_generation > fixture.engine->generation()) {
                inconsistent.fetch_add(1);
            }
            if (!std::is_sorted(snapshot.nodes.begin(), snapshot.nodes.end(),
                                [](const TopologyNode& a, const TopologyNode& b) { return a.id < b.id; })) {
                inconsistent.fetch_add(1);
            }
            if (!std::is_sorted(snapshot.edges.begin(), snapshot.edges.end(),
                                [](const TopologyEdge& a, const TopologyEdge& b) { return a.id < b.id; })) {
                inconsistent.fetch_add(1);
            }
            snapshots.fetch_add(1);
        }
    });

    for (int index = 0; index < 40; ++index) {
        static_cast<void>(
            fixture.add_node("ent-post-" + std::to_string(index), NodeClass::Switch, TopologyTier::Leaf));
    }
    stop.store(true);
    reader.join();
    FT_CHECK(snapshots.load() > 0);
    FT_CHECK_EQ(inconsistent.load(), 0);
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{80});
    FT_CHECK(fixture.engine->validate().valid);
}

FT_TEST(race, concurrent_readers_see_consistent_state) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    static_cast<void>(ft_test::build_small_fabric(fixture));
    const std::size_t nodes = fixture.engine->node_count();
    const std::size_t edges = fixture.engine->edge_count();
    const std::string digest = fixture.engine->digest();

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&] {
            for (int iteration = 0; iteration < 200; ++iteration) {
                if (fixture.engine->node_count() != nodes || fixture.engine->edge_count() != edges ||
                    fixture.engine->digest() != digest) {
                    mismatches.fetch_add(1);
                }
                if (!fixture.engine->validate().valid) {
                    mismatches.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    FT_CHECK_EQ(mismatches.load(), 0);
}

FT_TEST(race, concurrent_reconciliation_is_serialised) {
    ft_test::Fixture fixture = ft_test::make_fixture();
    for (int index = 0; index < 8; ++index) {
        static_cast<void>(
            fixture.add_entity("ent-c-" + std::to_string(index), EntityClass::Switch));
    }

    std::barrier start(5);
    std::atomic<int> committed{0};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&, worker] {
            start.arrive_and_wait();
            Publication publication;
            publication.id = PublicationId::from_trusted("pub-concurrent-" + std::to_string(worker));
            publication.domain = fixture.domain;
            publication.mode = PublicationMode::Incremental;
            ObservedNode node;
            node.entity_id = "ent-c-" + std::to_string(worker);
            node.node_class = NodeClass::Switch;
            node.tier = TopologyTier::Leaf;
            node.domain = fixture.domain;
            node.entity_generation = EntityGeneration{1};
            publication.nodes.push_back(node);
            const PublicationResult result = fixture.engine->publish(fixture.context(), publication);
            if (result.committed()) {
                committed.fetch_add(1);
            }
        });
    }
    start.arrive_and_wait();
    for (std::thread& thread : threads) {
        thread.join();
    }
    FT_CHECK_EQ(committed.load(), 4);
    FT_CHECK_EQ(fixture.engine->node_count(), std::size_t{4});
    FT_CHECK(fixture.engine->validate().valid);
    FT_CHECK(fixture.engine->verify_integrity().outcome() == Outcome::Ok);
}
