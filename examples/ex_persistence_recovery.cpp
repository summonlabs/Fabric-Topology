// Example: durable topology and conservative recovery.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <cstdio>
#include <filesystem>
#include <string>

#include "example_support.hpp"

using namespace example;

int main() {
    heading("persistence and recovery");
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "fabric_topology_example_state.ftstate";
    std::error_code error;
    std::filesystem::remove(path, error);
    const std::string state = path.string();

    {
        Engine fabric = make_engine(true, state);
        const TopologyNodeId port_a =
            fabric.add_node("sw-a-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
        const TopologyNodeId port_b =
            fabric.add_node("sw-b-p0", NodeClass::PhysicalPort, TopologyTier::Leaf);
        const TopologyEdgeId edge = fabric.add_edge(RelationClass::ConnectedTo, port_a, port_b);
        std::cout << "committed generation=" << fabric.engine->generation().to_string()
                  << " edge lifecycle=" << to_string(fabric.engine->edge(edge)->lifecycle) << "\n";
    }

    PersistenceHeaderInfo header;
    const Status inspected = inspect_persistence_file(state, header);
    std::cout << "container: " << to_string(inspected.outcome())
              << " format_version=" << header.format_version
              << " payload_sha256=" << header.payload_sha256 << "\n";

    TopologyEngineOptions options;
    auto directory = std::make_shared<InMemoryEntityDirectory>();
    for (const auto& entry : {std::pair<const char*, EntityClass>{"sw-a-p0", EntityClass::Port},
                              {"sw-b-p0", EntityClass::Port}}) {
        EntityRecord record;
        record.entity_id = entry.first;
        record.entity_class = entry.second;
        record.generation = EntityGeneration{1};
        static_cast<void>(directory->add(record));
    }
    options.directory = directory;
    options.persistence_path = state;
    LoadReport report;
    const std::unique_ptr<TopologyEngine> recovered = TopologyEngine::open(options, report);
    std::cout << report.render() << "\n";
    std::cout << "durable structure survives, live currentness does not: edges="
              << recovered->edge_count()
              << " current=" << recovered->validate().current_edges << "\n";

    std::filesystem::remove(path, error);
    return recovered->edge_count() == 1 ? 0 : 1;
}
