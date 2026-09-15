// Example: device replacement fencing.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "example_support.hpp"

using namespace example;

int main() {
    heading("device replacement");
    Engine fabric = make_engine();

    const TopologyNodeId sw = fabric.add_node("sw-1", NodeClass::Switch, TopologyTier::Leaf);
    const TopologyNodeId sw_port =
        fabric.add_node("sw-1-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
    const TopologyNodeId nic = fabric.add_node("host-1-nic0", NodeClass::Nic, TopologyTier::Endpoint);
    const TopologyNodeId nic_port =
        fabric.add_node("host-1-nic0-p0", NodeClass::PhysicalPort, TopologyTier::Endpoint);
    static_cast<void>(fabric.add_edge(RelationClass::PresentsEndpoint, sw, sw_port,
                                      TopologyLayer::Physical));
    static_cast<void>(fabric.add_edge(RelationClass::PresentsEndpoint, nic, nic_port,
                                      TopologyLayer::Physical));
    const TopologyEdgeId cable = fabric.add_edge(RelationClass::ConnectedTo, nic_port, sw_port);
    std::cout << "cable lifecycle=" << to_string(fabric.engine->edge(cable)->lifecycle) << "\n";

    // Fabric Registry replaces the physical port sw-1-p0, which the cable is bound to.
    EntityRecord successor;
    successor.entity_id = "sw-1-gen2-p0";
    successor.entity_class = EntityClass::Port;
    successor.generation = EntityGeneration{2};
    std::cout << "registry supersede: "
              << to_string(fabric.directory->supersede("sw-1-p0", successor).outcome()) << "\n";

    // Fabric Topology must not let the replacement identity inherit the old relationships: the
    // cable bound to the previous port generation is demoted to revalidation-required.
    ReplaceEndpointGenerationRequest replace;
    replace.node = sw_port;
    replace.rebind_identity = true;
    replace.successor_entity_id = "sw-1-gen2-p0";
    replace.reason = "device replaced";
    const MutationResult migrated = fabric.engine->replace_endpoint_generation(fabric.context(), replace);
    std::cout << "rebind: " << to_string(migrated.status.outcome()) << "\n";
    if (migrated.accepted()) {
        std::cout << "cable lifecycle=" << to_string(fabric.engine->edge(cable)->lifecycle) << "\n";
        std::cout << "port entity=" << fabric.engine->node(sw_port)->entity_id
                  << " generation=" << fabric.engine->node(sw_port)->entity_generation.to_string()
                  << "\n";
        std::cout << "switch node untouched: entity=" << fabric.engine->node(sw)->entity_id << "\n";
    }
    std::cout << "old edges never silently inherit the replacement identity\n";
    return 0;
}
