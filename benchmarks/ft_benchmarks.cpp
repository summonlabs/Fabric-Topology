// Fabric Topology benchmarks.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Every figure below is the wall-clock time of completed work: the operation has returned and
// its effect is observable in the authoritative state before the timer is stopped. Nothing
// here measures enqueue-only paths. Values are printed as measured; no target throughput is
// claimed.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"

using namespace fabric_topology;

namespace {

class Timer {
public:
    Timer() : start_(std::chrono::steady_clock::now()) {}
    [[nodiscard]] double ms() const {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

void report(const char* name, std::size_t operations, double milliseconds) {
    const double per_operation = operations == 0 ? 0.0 : milliseconds / static_cast<double>(operations);
    std::cout << std::left << std::setw(38) << name << std::right << std::setw(12) << operations
              << std::setw(14) << std::fixed << std::setprecision(3) << milliseconds << std::setw(14)
              << std::setprecision(6) << per_operation << "\n";
}

struct Harness {
    std::shared_ptr<InMemoryEntityDirectory> directory = std::make_shared<InMemoryEntityDirectory>();
    std::unique_ptr<TopologyEngine> engine;
    PublisherId publisher = PublisherId::from_trusted("pub-bench");
    WorkerBootId boot = WorkerBootId::from_trusted("boot-bench");
    TopologyDomainId domain = TopologyDomainId::from_trusted("dom-bench");

    explicit Harness(bool verify_indexes = false) {
        TopologyEngineOptions options;
        options.directory = directory;
        options.verify_indexes_on_mutation = verify_indexes;
        engine = std::make_unique<TopologyEngine>(options);
        DomainDefinition definition;
        definition.id = domain;
        definition.kind = ScopeKind::AdministrativeDomain;
        static_cast<void>(engine->define_domain(definition));
        PublisherRegistration registration;
        registration.publisher = publisher;
        registration.worker_boot = boot;
        registration.coordinator_epoch = engine->coordinator_epoch();
        ScopeGrant grant;
        grant.domain = domain;
        grant.mode = GrantMode::AuthoritativeWrite;
        registration.grants.push_back(grant);
        static_cast<void>(engine->register_publisher(registration));
    }

    [[nodiscard]] AuthorityContext context() const {
        AuthorityContext ctx;
        ctx.coordinator_epoch = engine->coordinator_epoch();
        ctx.publisher = publisher;
        ctx.worker_boot = boot;
        ctx.evidence_generation = EvidenceGeneration{1};
        return ctx;
    }

    TopologyNodeId seed_entity(const std::string& id, EntityClass entity_class, NodeClass node_class,
                               TopologyTier tier = TopologyTier::Leaf) {
        EntityRecord record;
        record.entity_id = id;
        record.entity_class = entity_class;
        record.generation = EntityGeneration{1};
        static_cast<void>(directory->add(record));
        AddNodeRequest request;
        request.entity_id = id;
        request.node_class = node_class;
        request.tier = tier;
        request.domain = domain;
        const MutationResult result = engine->add_node(context(), request);
        return result.accepted() ? *result.node : TopologyNodeId{};
    }
};

constexpr std::size_t kSwitches = 2000;
constexpr std::size_t kHostsPerSwitch = 4;

Publication build_bulk_publication(Harness& harness, const char* id) {
    Publication publication;
    publication.id = PublicationId::from_trusted(id);
    publication.domain = harness.domain;
    publication.mode = PublicationMode::AuthoritativeSnapshot;
    publication.type = PublicationType::Synthetic;
    publication.source = DiscoverySource::SyntheticGenerator;

    EntityRecord fabric_record;
    fabric_record.entity_id = "bench-fabric";
    fabric_record.entity_class = EntityClass::Fabric;
    fabric_record.generation = EntityGeneration{1};
    static_cast<void>(harness.directory->add(fabric_record));
    ObservedNode fabric;
    fabric.entity_id = "bench-fabric";
    fabric.node_class = NodeClass::Fabric;
    fabric.domain = harness.domain;
    fabric.entity_generation = EntityGeneration{1};
    fabric.node_id = TopologyNodeId::from_trusted("n-bench-fabric");
    publication.nodes.push_back(fabric);

    std::vector<std::string> ports;
    for (std::size_t index = 0; index < kSwitches; ++index) {
        const std::string switch_entity = "bench-sw-" + std::to_string(index);
        EntityRecord record;
        record.entity_id = switch_entity;
        record.entity_class = EntityClass::Switch;
        record.generation = EntityGeneration{1};
        static_cast<void>(harness.directory->add(record));
        ObservedNode sw;
        sw.entity_id = switch_entity;
        sw.node_class = NodeClass::Switch;
        sw.tier = TopologyTier::Leaf;
        sw.domain = harness.domain;
        sw.entity_generation = EntityGeneration{1};
        sw.node_id = TopologyNodeId::from_trusted("n-bench-sw-" + std::to_string(index));
        publication.nodes.push_back(sw);

        ObservedEdge membership;
        membership.relation = RelationClass::MemberOf;
        membership.from = sw.node_id;
        membership.to = fabric.node_id;
        membership.layer = TopologyLayer::Logical;
        membership.domain = harness.domain;
        membership.evidence_generation = EvidenceGeneration{1};
        publication.edges.push_back(membership);

        for (std::size_t host = 0; host < kHostsPerSwitch; ++host) {
            const std::string nic_entity = switch_entity + "-nic" + std::to_string(host);
            EntityRecord nic_record;
            nic_record.entity_id = nic_entity;
            nic_record.entity_class = EntityClass::Nic;
            nic_record.generation = EntityGeneration{1};
            static_cast<void>(harness.directory->add(nic_record));
            ObservedNode nic;
            nic.entity_id = nic_entity;
            nic.node_class = NodeClass::Nic;
            nic.tier = TopologyTier::Endpoint;
            nic.domain = harness.domain;
            nic.entity_generation = EntityGeneration{1};
            nic.node_id = TopologyNodeId::from_trusted("n-bench-nic-" + std::to_string(index) + "-" +
                                                       std::to_string(host));
            publication.nodes.push_back(nic);

            const std::string port_entity = nic_entity + "-p0";
            EntityRecord port_record;
            port_record.entity_id = port_entity;
            port_record.entity_class = EntityClass::Port;
            port_record.generation = EntityGeneration{1};
            static_cast<void>(harness.directory->add(port_record));
            ObservedNode port;
            port.entity_id = port_entity;
            port.node_class = NodeClass::PhysicalPort;
            port.tier = TopologyTier::Endpoint;
            port.domain = harness.domain;
            port.entity_generation = EntityGeneration{1};
            port.node_id = TopologyNodeId::from_trusted("n-bench-port-" + std::to_string(index) + "-" +
                                                        std::to_string(host));
            publication.nodes.push_back(port);

            ObservedEdge presents;
            presents.relation = RelationClass::PresentsEndpoint;
            presents.from = nic.node_id;
            presents.to = port.node_id;
            presents.layer = TopologyLayer::Physical;
            presents.domain = harness.domain;
            presents.evidence_generation = EvidenceGeneration{1};
            publication.edges.push_back(presents);

            ports.push_back(port.node_id.value());
        }
    }

    // Dense connectivity mesh: twenty connections per port slot.
    for (std::size_t index = 0; index < ports.size(); ++index) {
        for (std::size_t link = 1; link <= 20; ++link) {
            const std::size_t peer = (index * 13 + link * 977) % ports.size();
            if (peer <= index) {
                continue;
            }
            ObservedEdge edge;
            edge.relation = RelationClass::ConnectedTo;
            edge.from = TopologyNodeId::from_trusted(ports[index]);
            edge.to = TopologyNodeId::from_trusted(ports[peer]);
            edge.domain = harness.domain;
            edge.evidence_generation = EvidenceGeneration{1};
            publication.edges.push_back(edge);
        }
    }
    return publication;
}

}  // namespace

int main() {
    std::cout << product_banner() << " benchmarks\n";
    std::cout << std::left << std::setw(38) << "operation" << std::right << std::setw(12) << "count"
              << std::setw(14) << "total_ms" << std::setw(14) << "per_op_ms" << "\n";

    {
        Harness harness;
        constexpr std::size_t kNodes = 10000;
        Timer timer;
        std::size_t created = 0;
        for (std::size_t index = 0; index < kNodes; ++index) {
            const std::string entity = "solo-node-" + std::to_string(index);
            EntityRecord record;
            record.entity_id = entity;
            record.entity_class = EntityClass::Switch;
            record.generation = EntityGeneration{1};
            static_cast<void>(harness.directory->add(record));
            AddNodeRequest request;
            request.entity_id = entity;
            request.node_class = NodeClass::Switch;
            request.tier = TopologyTier::Leaf;
            request.domain = harness.domain;
            if (harness.engine->add_node(harness.context(), request).accepted()) {
                ++created;
            }
        }
        const double elapsed = timer.ms();
        if (harness.engine->node_count() != created) {
            std::cerr << "node insert benchmark lost operations\n";
            return 1;
        }
        report("node_insert", created, elapsed);
    }

    Harness bulk;
    const Publication publication = build_bulk_publication(bulk, "bench-bulk-1");
    std::cout << "bulk publication declares nodes=" << publication.nodes.size()
              << " edges=" << publication.edges.size() << "\n";

    {
        Timer timer;
        const PublicationResult result = bulk.engine->publish(bulk.context(), publication);
        const double elapsed = timer.ms();
        if (!result.committed()) {
            std::cerr << "bulk publication failed: " << result.status.one_line() << "\n";
            return 1;
        }
        report("authoritative_snapshot_reconcile", publication.nodes.size() + publication.edges.size(),
               elapsed);
        report("snapshot_edges_added", result.edges_added, elapsed);
    }

    const std::size_t node_count = bulk.engine->node_count();
    const std::size_t edge_count = bulk.engine->edge_count();
    std::cout << "resulting topology nodes=" << node_count << " edges=" << edge_count << "\n";

    {
        const std::vector<TopologyEdge> edges = bulk.engine->all_edges();
        constexpr std::size_t kLookups = 200000;
        Timer timer;
        std::size_t found = 0;
        for (std::size_t index = 0; index < kLookups; ++index) {
            if (bulk.engine->edge(edges[index % edges.size()].id).has_value()) {
                ++found;
            }
        }
        const double elapsed = timer.ms();
        if (found != kLookups) {
            std::cerr << "edge lookup benchmark lost operations\n";
            return 1;
        }
        report("edge_lookup", kLookups, elapsed);
    }

    {
        const std::vector<TopologyNode> nodes = bulk.engine->all_nodes();
        constexpr std::size_t kQueries = 5000;
        Timer timer;
        std::size_t total = 0;
        for (std::size_t index = 0; index < kQueries; ++index) {
            total += bulk.engine->neighbors(nodes[index % nodes.size()].id).size();
        }
        const double elapsed = timer.ms();
        if (total == 0) {
            std::cerr << "neighbor query benchmark returned nothing\n";
            return 1;
        }
        report("neighbor_query", kQueries, elapsed);
    }

    {
        constexpr std::size_t kSnapshots = 5;
        Timer timer;
        std::size_t nodes_seen = 0;
        for (std::size_t index = 0; index < kSnapshots; ++index) {
            const TopologySnapshot snapshot = bulk.engine->snapshot();
            nodes_seen += snapshot.node_count();
        }
        const double elapsed = timer.ms();
        if (nodes_seen == 0) {
            std::cerr << "snapshot benchmark produced no content\n";
            return 1;
        }
        report("snapshot_construction", kSnapshots, elapsed);
    }

    {
        constexpr std::size_t kDigests = 5;
        Timer timer;
        for (std::size_t index = 0; index < kDigests; ++index) {
            if (bulk.engine->digest().empty()) {
                std::cerr << "digest benchmark produced nothing\n";
                return 1;
            }
        }
        report("deterministic_digest", kDigests, timer.ms());
    }

    {
        const TopologySnapshot after = bulk.engine->snapshot();
        Timer timer;
        const TopologyDiff diff = bulk.engine->diff(after, after);
        const double elapsed = timer.ms();
        if (!diff.empty()) {
            std::cerr << "identical snapshots produced a non-empty diff\n";
            return 1;
        }
        report("diff_generation", after.node_count() + after.edge_count(), elapsed);
    }

    {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "fabric_topology_bench.ftstate";
        std::error_code error;
        std::filesystem::remove(path, error);
        Timer timer;
        const Status saved = bulk.engine->save_to(path.string());
        const double elapsed = timer.ms();
        if (saved.outcome() != Outcome::Ok) {
            std::cerr << "save failed: " << saved.one_line() << "\n";
            return 1;
        }
        report("persistence_save", node_count + edge_count, elapsed);

        TopologyEngineOptions options;
        options.directory = bulk.directory;
        options.persistence_path = path.string();
        LoadReport report_data;
        Timer load_timer;
        const std::unique_ptr<TopologyEngine> loaded = TopologyEngine::open(options, report_data);
        const double load_elapsed = load_timer.ms();
        if (!report_data.loaded || loaded->node_count() != node_count ||
            loaded->edge_count() != edge_count) {
            std::cerr << "load failed or lost state\n";
            return 1;
        }
        report("persistence_load", node_count + edge_count, load_elapsed);
        std::filesystem::remove(path, error);
    }

    {
        const std::vector<TopologyNode> nodes = bulk.engine->all_nodes();
        constexpr int kReaders = 8;
        constexpr int kIterations = 2000;
        Timer timer;
        std::vector<std::thread> threads;
        std::vector<std::size_t> per_thread(static_cast<std::size_t>(kReaders), 0);
        for (int worker = 0; worker < kReaders; ++worker) {
            threads.emplace_back([&, worker] {
                std::size_t completed = 0;
                for (int iteration = 0; iteration < kIterations; ++iteration) {
                    const std::size_t index = static_cast<std::size_t>(iteration) * 7U +
                                              static_cast<std::size_t>(worker);
                    const TopologyNodeId& id = nodes[index % nodes.size()].id;
                    completed += bulk.engine->neighbors(id).size();
                    completed += bulk.engine->edges_for_node(id).size();
                    completed += bulk.engine->node(id).has_value() ? 1U : 0U;
                    completed += bulk.engine->node_count() == node_count ? 1U : 0U;
                }
                per_thread[static_cast<std::size_t>(worker)] = completed;
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        std::size_t total = 0;
        for (const std::size_t value : per_thread) {
            if (value == 0) {
                std::cerr << "concurrent reader benchmark observed no completed work\n";
                return 1;
            }
            total += value;
        }
        report("concurrent_readers", total, timer.ms());
    }

    {
        // A whole-graph structural validation: one completed pass over every node, edge and
        // acyclicity constraint.
        Timer timer;
        const ValidationReport report_data = bulk.engine->validate();
        const double elapsed = timer.ms();
        if (!report_data.valid) {
            std::cerr << "validation benchmark found an invalid topology\n";
            return 1;
        }
        report("full_validation", report_data.nodes + report_data.edges, elapsed);
    }

    std::cout << "final generation=" << bulk.engine->generation().to_string() << "\n";
    return 0;
}
