// Fabric Topology examples - shared support.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Every example uses only the installed public API.

#ifndef FABRIC_TOPOLOGY_EXAMPLES_EXAMPLE_SUPPORT_HPP
#define FABRIC_TOPOLOGY_EXAMPLES_EXAMPLE_SUPPORT_HPP

#include <iostream>
#include <memory>
#include <string>

#include "fabric_topology/fabric_topology.hpp"

namespace example {

using namespace fabric_topology;

struct Engine {
    std::shared_ptr<InMemoryEntityDirectory> directory;
    std::unique_ptr<TopologyEngine> engine;
    PublisherId publisher = PublisherId::from_trusted("pub-example");
    WorkerBootId boot = WorkerBootId::from_trusted("boot-example");
    TopologyDomainId domain = TopologyDomainId::from_trusted("dom-example");

    AuthorityContext context() const {
        AuthorityContext ctx;
        ctx.coordinator_epoch = engine->coordinator_epoch();
        ctx.publisher = publisher;
        ctx.worker_boot = boot;
        ctx.evidence_generation = EvidenceGeneration{1};
        return ctx;
    }

    void add_entity(const std::string& entity_id, EntityClass entity_class,
                    EntityGeneration generation = EntityGeneration{1}) {
        EntityRecord record;
        record.entity_id = entity_id;
        record.entity_class = entity_class;
        record.generation = generation;
        static_cast<void>(directory->add(record));
    }

    TopologyNodeId add_node(const std::string& entity_id, NodeClass node_class,
                            TopologyTier tier = TopologyTier::Unspecified) {
        add_entity(entity_id, node_class_entity_class(node_class));
        AddNodeRequest request;
        request.entity_id = entity_id;
        request.node_class = node_class;
        request.tier = tier;
        request.domain = domain;
        const MutationResult result = engine->add_node(context(), request);
        if (!result.accepted()) {
            std::cerr << "add_node failed: " << result.status.one_line() << "\n";
            return TopologyNodeId{};
        }
        return *result.node;
    }

    TopologyEdgeId add_edge(RelationClass relation, const TopologyNodeId& from,
                            const TopologyNodeId& to,
                            std::optional<TopologyLayer> layer = std::nullopt) {
        AddRelationshipRequest request;
        request.relation = relation;
        request.from = from;
        request.to = to;
        request.layer = layer;
        request.domain = domain;
        const MutationResult result = engine->add_relationship(context(), request);
        if (!result.accepted()) {
            std::cerr << "add_relationship failed: " << result.status.one_line() << "\n";
            return TopologyEdgeId{};
        }
        return *result.edge;
    }
};

inline Engine make_engine(bool durable = false, const std::string& path = std::string()) {
    Engine handle;
    handle.directory = std::make_shared<InMemoryEntityDirectory>();
    TopologyEngineOptions options;
    options.directory = handle.directory;
    options.verify_indexes_on_mutation = true;
    if (durable) {
        options.persistence_path = path;
        options.persistence_mode = PersistenceMode::Immediate;
    }
    handle.engine = std::make_unique<TopologyEngine>(options);

    DomainDefinition definition;
    definition.id = handle.domain;
    definition.kind = ScopeKind::AdministrativeDomain;
    static_cast<void>(handle.engine->define_domain(definition));

    PublisherRegistration registration;
    registration.publisher = handle.publisher;
    registration.worker_boot = handle.boot;
    registration.coordinator_epoch = handle.engine->coordinator_epoch();
    ScopeGrant grant;
    grant.domain = handle.domain;
    grant.mode = GrantMode::AuthoritativeWrite;
    registration.grants.push_back(grant);
    static_cast<void>(handle.engine->register_publisher(registration));
    return handle;
}

inline void heading(const char* text) {
    std::cout << "=== " << text << " ===\n";
}

}  // namespace example

#endif  // FABRIC_TOPOLOGY_EXAMPLES_EXAMPLE_SUPPORT_HPP
