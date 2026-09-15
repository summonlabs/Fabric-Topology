// Example: worker incarnation fencing.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "example_support.hpp"

using namespace example;

int main() {
    heading("worker incarnation fencing");
    Engine fabric = make_engine();

    const TopologyNodeId port_a = fabric.add_node("sw-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b = fabric.add_node("sw-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge = fabric.add_edge(RelationClass::ConnectedTo, port_a, port_b);
    std::cout << "edge lifecycle=" << to_string(fabric.engine->edge(edge)->lifecycle) << "\n";

    // The worker process died. Its boot identifier is fenced permanently.
    const WorkerBootId dead_boot = fabric.boot;
    static_cast<void>(fabric.engine->fence_boot(dead_boot, "process died", true));

    std::cout << "edge lifecycle after fencing="
              << to_string(fabric.engine->edge(edge)->lifecycle) << "\n";
    std::cout << "durable structure preserved: nodes=" << fabric.engine->node_count()
              << " edges=" << fabric.engine->edge_count() << "\n";

    // A replay from the dead incarnation is refused.
    UpdateEvidenceRequest replay;
    replay.edge = edge;
    replay.evidence_generation = EvidenceGeneration{7};
    const MutationResult refused =
        fabric.engine->update_relationship_evidence(fabric.context(), replay);
    std::cout << "replay from dead boot: " << to_string(refused.status.outcome()) << "\n";

    // A fresh incarnation registers with a new boot identifier and revalidates.
    const WorkerBootId fresh_boot = WorkerBootId::from_trusted("boot-example-2");
    PublisherRegistration registration;
    registration.publisher = fabric.publisher;
    registration.worker_boot = fresh_boot;
    registration.coordinator_epoch = fabric.engine->coordinator_epoch();
    ScopeGrant grant;
    grant.domain = fabric.domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    std::cout << "re-registration: "
              << to_string(fabric.engine->register_publisher(registration).outcome()) << "\n";

    AuthorityContext fresh = fabric.context();
    fresh.worker_boot = fresh_boot;
    RevalidateRequest revalidate;
    revalidate.edge = edge;
    revalidate.evidence_generation = EvidenceGeneration{8};
    const MutationResult revalidated = fabric.engine->revalidate_relationship(fresh, revalidate);
    std::cout << "revalidation: " << to_string(revalidated.status.outcome()) << "\n";
    std::cout << "edge lifecycle=" << to_string(fabric.engine->edge(edge)->lifecycle) << "\n";
    return 0;
}
