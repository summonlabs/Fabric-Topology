// Example: a synthetic leaf-spine fabric.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "example_support.hpp"

using namespace example;

int main() {
    heading("synthetic leaf-spine topology");
    Engine fabric = make_engine();

    SyntheticOptions options;
    options.scenario = SyntheticScenario::LeafSpine;
    options.scale = 4;
    options.tiers = 2;
    options.domain = fabric.domain;
    options.publisher = fabric.publisher;
    const SyntheticTopology topology = build_synthetic_topology(options);
    std::cout << topology.render() << "\n";

    for (const EntityRecord& record : topology.directory_entries) {
        static_cast<void>(fabric.directory->add(record));
    }
    const PublicationResult result = fabric.engine->publish(fabric.context(), topology.publication);
    std::cout << result.render() << "\n";
    if (!result.committed()) {
        return 1;
    }
    std::cout << "validation: " << fabric.engine->validate().render() << "\n";
    std::cout << "evidence classification: SYNTHETIC (no physical hardware is involved)\n";
    return 0;
}
