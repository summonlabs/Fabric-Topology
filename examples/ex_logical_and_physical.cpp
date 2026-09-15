// Example: physical structure with a logical overlay on top of it.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "example_support.hpp"

using namespace example;

int main() {
    heading("physical and logical topology");
    Engine fabric = make_engine();

    const TopologyNodeId physical_a =
        fabric.add_node("sw-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId physical_b =
        fabric.add_node("sw-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId logical_a =
        fabric.add_node("sw-a-vlan10", NodeClass::LogicalPort, TopologyTier::Leaf);
    const TopologyNodeId logical_b =
        fabric.add_node("sw-b-vlan10", NodeClass::LogicalPort, TopologyTier::Leaf);
    const TopologyNodeId tunnel_a =
        fabric.add_node("sw-a-vxlan", NodeClass::LogicalEndpoint, TopologyTier::Leaf);
    const TopologyNodeId tunnel_b =
        fabric.add_node("sw-b-vxlan", NodeClass::LogicalEndpoint, TopologyTier::Leaf);

    const TopologyEdgeId cable =
        fabric.add_edge(RelationClass::ConnectedTo, physical_a, physical_b);
    static_cast<void>(fabric.add_edge(RelationClass::BackedBy, logical_a, physical_a));
    static_cast<void>(fabric.add_edge(RelationClass::BackedBy, logical_b, physical_b));
    static_cast<void>(fabric.add_edge(RelationClass::LogicallyConnectedTo, logical_a, logical_b));
    static_cast<void>(fabric.add_edge(RelationClass::BackedBy, tunnel_a, logical_a));
    static_cast<void>(fabric.add_edge(RelationClass::BackedBy, tunnel_b, logical_b));
    static_cast<void>(fabric.add_edge(RelationClass::TunneledOver, tunnel_a, tunnel_b));

    std::cout << "physical_relationships=" << fabric.engine->physical_relationships().size()
              << " logical_relationships=" << fabric.engine->logical_relationships().size() << "\n";

    TraversalRequest request;
    request.kind = TraversalKind::LogicalDependencyChain;
    request.origin = tunnel_a;
    request.max_depth = 4;
    std::cout << fabric.engine->traverse(request).render() << "\n";

    std::cout << "physical support of the logical adjacency: "
              << (fabric.engine->edge(cable)->supported_by.has_value() ? "declared" : "not declared")
              << "\n";
    std::cout << "note: Fabric Topology does not compute the tunnel path through the fabric\n";
    return 0;
}
