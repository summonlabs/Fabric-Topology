// Example: immutable snapshots, currentness and deterministic diffs.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "example_support.hpp"

using namespace example;

int main() {
    heading("snapshot, currentness and diff");
    Engine fabric = make_engine();

    const TopologyNodeId port_a = fabric.add_node("sw-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId port_b = fabric.add_node("sw-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    static_cast<void>(fabric.add_edge(RelationClass::ConnectedTo, port_a, port_b));

    const TopologySnapshot before = fabric.engine->snapshot();
    std::cout << "snapshot id=" << before.id.to_string()
              << " generation=" << before.topology_generation.to_string()
              << " digest=" << before.digest << "\n";
    std::cout << "current=" << fabric.engine->check_snapshot(before).render() << "\n";

    static_cast<void>(fabric.add_node("sw-c-p0", NodeClass::PhysicalPort, TopologyTier::Leaf));
    static_cast<void>(fabric.add_edge(RelationClass::ConnectedTo,
                                      fabric.engine->node_for_entity("sw-c-p0")->id, port_a));

    const SnapshotCurrentness stale = fabric.engine->check_snapshot(before);
    std::cout << "old snapshot still inspectable, current=" << stale.render() << "\n";

    const TopologySnapshot after = fabric.engine->snapshot();
    const TopologyDiff diff = fabric.engine->diff(before, after);
    std::cout << diff.render();
    std::cout << "diff digest=" << diff.digest << "\n";
    std::cout << "digest is independent of insertion order and of process-local incarnations\n";
    return 0;
}
