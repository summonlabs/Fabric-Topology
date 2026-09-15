// Example: stale generation and stale evidence rejection.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "example_support.hpp"

using namespace example;

int main() {
    heading("stale generation rejection");
    Engine fabric = make_engine();

    const TopologyNodeId port_a = fabric.add_node("sw-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b = fabric.add_node("sw-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyEdgeId edge = fabric.add_edge(RelationClass::ConnectedTo, port_a, port_b);
    const TopologyGeneration observed = fabric.engine->generation();
    std::cout << "observed generation=" << observed.to_string() << "\n";

    // Another publisher commits first, advancing the generation.
    static_cast<void>(fabric.add_node("sw-c-p0", NodeClass::PhysicalPort, TopologyTier::Leaf));

    AddRelationshipRequest stale;
    stale.relation = RelationClass::ConnectedTo;
    stale.from = port_a;
    stale.to = fabric.engine->node_for_entity("sw-c-p0")->id;
    stale.domain = fabric.domain;
    stale.expected_generation = observed;
    const MutationResult rejected = fabric.engine->add_relationship(fabric.context(), stale);
    std::cout << "stale request outcome=" << to_string(rejected.status.outcome()) << "\n";
    std::cout << rejected.status.render() << "\n";

    // Replaying the evidence position the relationship already carries is recognised as
    // idempotent and does not advance the topology generation.
    UpdateEvidenceRequest replay;
    replay.edge = edge;
    replay.evidence_generation = fabric.engine->edge(edge)->evidence_generation;
    const TopologyGeneration before_replay = fabric.engine->generation();
    const MutationResult idempotent =
        fabric.engine->update_relationship_evidence(fabric.context(), replay);
    std::cout << "replay outcome=" << to_string(idempotent.status.outcome()) << "\n";
    std::cout << "generation after replay=" << fabric.engine->generation().to_string()
              << " (unchanged="
              << (fabric.engine->generation() == before_replay ? "true" : "false") << ")\n";
    return 0;
}
