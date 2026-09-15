// Example: a basic physical fabric topology.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "example_support.hpp"

using namespace example;

int main() {
    heading("basic physical topology");
    Engine fabric = make_engine();

    const TopologyNodeId fabric_node = fabric.add_node("fab-1", NodeClass::Fabric);
    const TopologyNodeId leaf = fabric.add_node("sw-leaf-1", NodeClass::Switch, TopologyTier::Leaf);
    const TopologyNodeId leaf_port =
        fabric.add_node("sw-leaf-1-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId nic = fabric.add_node("host-1-nic0", NodeClass::Nic, TopologyTier::Endpoint);
    const TopologyNodeId nic_port =
        fabric.add_node("host-1-nic0-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint);

    static_cast<void>(fabric.add_edge(RelationClass::MemberOf, leaf, fabric_node,
                                      TopologyLayer::Logical));
    static_cast<void>(fabric.add_edge(RelationClass::PresentsEndpoint, leaf, leaf_port,
                                      TopologyLayer::Physical));
    static_cast<void>(fabric.add_edge(RelationClass::PresentsEndpoint, nic, nic_port,
                                      TopologyLayer::Physical));
    const TopologyEdgeId cable = fabric.add_edge(RelationClass::ConnectedTo, nic_port, leaf_port);

    std::cout << "nodes=" << fabric.engine->node_count() << " edges=" << fabric.engine->edge_count()
              << "\n";
    std::cout << "generation=" << fabric.engine->generation().to_string() << "\n";

    Explanation explanation;
    static_cast<void>(fabric.engine->explain_relationship(cable, explanation));
    std::cout << explanation.render() << "\n";
    std::cout << "validation: " << fabric.engine->validate().render() << "\n";

    // A topology edge is structural only. It never asserts link health or reachability.
    std::cout << "note: CONNECTED_TO states structure, not link state or reachability\n";
    return 0;
}
