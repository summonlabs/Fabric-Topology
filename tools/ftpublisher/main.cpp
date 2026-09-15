// Fabric Topology publisher worker process.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "fabric_topology/runtime/publisher_client.hpp"
#include "fabric_topology/runtime/registry_seed.hpp"

namespace {

using namespace fabric_topology;

void print_usage() {
    std::cout << "usage: ftpublisher [options]\n"
                 "  --host ADDRESS            coordinator address (default 127.0.0.1)\n"
                 "  --port N                  coordinator port (required)\n"
                 "  --publisher ID            publisher identity (default pub-worker)\n"
                 "  --domain ID               publication scope (default dom-synthetic)\n"
                 "  --scenario NAME           synthetic scenario to publish (default single_switch)\n"
                 "  --scale N                 scenario scale override\n"
                 "  --tiers N                 scenario tier/spine override\n"
                 "  --grant include|authoritative|read   requested grant mode (default authoritative)\n"
                 "  --mode NAME               publication mode: authoritative|incremental|partial\n"
                 "  --publish-count N         number of publications to submit (default 1)\n"
                 "  --worker-boot ID          reuse an explicit worker boot identifier\n"
                 "  --claimed-epoch N         claim a coordinator epoch (0 means unset)\n"
                 "  --write-seed PATH         write the scenario's canonical identities and exit\n"
                 "  --token TOKEN             shared secret\n"
                 "  --observe                 attach as an observer instead of a publisher\n"
                 "  --query NAME              run one query and exit\n"
                 "  --hold-seconds N          keep the process alive for N seconds (0 = until killed)\n"
                 "  --request-shutdown        ask the coordinator to stop after publishing\n";
}

std::optional<GrantMode> parse_mode(const std::string& text) {
    if (text == "read") return GrantMode::Read;
    if (text == "incremental") return GrantMode::IncrementalWrite;
    if (text == "authoritative" || text == "include") return GrantMode::AuthoritativeWrite;
    return std::nullopt;
}

std::optional<PublicationMode> parse_publication_mode(const std::string& text) {
    if (text == "authoritative") return PublicationMode::AuthoritativeSnapshot;
    if (text == "incremental") return PublicationMode::Incremental;
    if (text == "partial") return PublicationMode::PartialObservation;
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    runtime::PublisherClientOptions options;
    options.publisher = PublisherId::from_trusted("pub-worker");
    TopologyDomainId domain = TopologyDomainId::from_trusted("dom-synthetic");
    std::string scenario_name = "single_switch";
    std::size_t scale = 0;
    std::size_t tiers = 0;
    int publish_count = 1;
    double hold_seconds = 0.0;
    bool request_shutdown = false;
    std::string query_name;
    std::string write_seed;
    std::string worker_boot_override;
    std::uint64_t claimed_epoch = 0;
    PublicationMode publication_mode = PublicationMode::AuthoritativeSnapshot;
    GrantMode requested_mode = GrantMode::AuthoritativeWrite;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&]() -> std::string {
            if (index + 1 >= argc) {
                return {};
            }
            return argv[++index];
        };
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        }
        if (argument == "--host") {
            options.host = next();
        } else if (argument == "--port") {
            options.port = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (argument == "--publisher") {
            const auto parsed = PublisherId::parse(next());
            if (!parsed.has_value()) {
                std::cerr << "invalid publisher identifier\n";
                return 2;
            }
            options.publisher = *parsed;
        } else if (argument == "--domain") {
            const auto parsed = TopologyDomainId::parse(next());
            if (!parsed.has_value()) {
                std::cerr << "invalid domain identifier\n";
                return 2;
            }
            domain = *parsed;
        } else if (argument == "--scenario") {
            scenario_name = next();
        } else if (argument == "--scale") {
            scale = static_cast<std::size_t>(std::stoul(next()));
        } else if (argument == "--tiers") {
            tiers = static_cast<std::size_t>(std::stoul(next()));
        } else if (argument == "--grant") {
            const auto mode = parse_mode(next());
            if (!mode.has_value()) {
                std::cerr << "invalid grant mode\n";
                return 2;
            }
            requested_mode = *mode;
        } else if (argument == "--mode") {
            const auto mode = parse_publication_mode(next());
            if (!mode.has_value()) {
                std::cerr << "invalid publication mode\n";
                return 2;
            }
            publication_mode = *mode;
        } else if (argument == "--publish-count") {
            publish_count = std::stoi(next());
        } else if (argument == "--write-seed") {
            write_seed = next();
        } else if (argument == "--worker-boot") {
            worker_boot_override = next();
        } else if (argument == "--claimed-epoch") {
            claimed_epoch = std::stoull(next());
        } else if (argument == "--token") {
            options.token = next();
        } else if (argument == "--process-label") {
            options.process_label = next();
        } else if (argument == "--observe") {
            options.observer = true;
        } else if (argument == "--query") {
            query_name = next();
        } else if (argument == "--hold-seconds") {
            hold_seconds = std::stod(next());
        } else if (argument == "--request-shutdown") {
            request_shutdown = true;
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            print_usage();
            return 2;
        }
    }

    if (!write_seed.empty()) {
        SyntheticOptions synthetic;
        synthetic.scenario = synthetic_scenario_from_string(scenario_name).value_or(
            SyntheticScenario::SingleSwitch);
        synthetic.scale = scale;
        synthetic.tiers = tiers;
        synthetic.domain = domain;
        synthetic.publisher = options.publisher;
        const SyntheticTopology topology = build_synthetic_topology(synthetic);
        const Status written = runtime::write_registry_seed(write_seed, topology.directory_entries);
        if (!outcome_is_success(written.outcome())) {
            std::cerr << "failed to write registry seed: " << written.one_line() << "\n";
            return 1;
        }
        std::cout << "registry_seed=" << write_seed << " records="
                  << topology.directory_entries.size() << "\n";
        return 0;
    }

    if (options.port == 0) {
        std::cerr << "--port is required\n";
        return 2;
    }

    if (worker_boot_override.empty()) {
        options.worker_boot = runtime::make_worker_boot_id(scenario_name);
    } else {
        const auto parsed = WorkerBootId::parse(worker_boot_override);
        if (!parsed.has_value()) {
            std::cerr << "invalid --worker-boot identifier\n";
            return 2;
        }
        options.worker_boot = *parsed;
    }
    options.claimed_epoch = CoordinatorEpoch{claimed_epoch};
    if (options.process_label.empty()) {
        options.process_label = "ftpublisher/" + options.publisher.value();
    }
    options.declared_evidence_type = PublicationType::Synthetic;
    if (!options.observer) {
        ScopeGrant grant;
        grant.domain = domain;
        grant.mode = requested_mode;
        options.requested_grants.push_back(grant);
    }

    runtime::PublisherClient client(std::move(options));
    WelcomeMessage welcome;
    const Status registered = client.connect_and_register(welcome);
    if (!outcome_is_success(registered.outcome())) {
        std::cerr << "registration failed: " << registered.one_line() << "\n";
        return 1;
    }
    std::cout << "publisher=" << client.publisher().to_string()
              << " worker_boot=" << client.worker_boot().to_string()
              << " coordinator_epoch=" << welcome.coordinator_epoch.to_string()
              << " topology_generation=" << welcome.topology_generation.to_string() << "\n";
    std::cout.flush();

    if (!query_name.empty()) {
        QueryResultMessage result;
        const Status status = client.query(query_name, "", result);
        std::cout << "query=" << query_name << " outcome=" << to_string(status.outcome()) << "\n"
                  << result.text << "\n";
        client.close();
        return outcome_is_success(status.outcome()) ? 0 : 1;
    }

    if (!client.welcome().accepted) {
        std::cerr << "coordinator rejected registration: " << welcome.reason.one_line() << "\n";
        client.close();
        return 1;
    }

    for (int iteration = 0; iteration < publish_count; ++iteration) {
        SyntheticOptions synthetic;
        synthetic.scenario = synthetic_scenario_from_string(scenario_name).value_or(
            SyntheticScenario::SingleSwitch);
        synthetic.scale = scale;
        synthetic.tiers = tiers;
        synthetic.domain = domain;
        synthetic.publisher = client.publisher();
        SyntheticTopology topology = build_synthetic_topology(synthetic);
        topology.publication.mode = publication_mode;
        topology.publication.evidence_generation = EvidenceGeneration{static_cast<std::uint64_t>(iteration) + 1};
        // A publication identifier identifies one publication. A fresh incarnation or a later
        // iteration therefore submits a new identifier; replaying an identical publication
        // identifier stays idempotent by design.
        topology.publication.id = PublicationId::from_trusted(
            "pub-" + client.publisher().value() + "-" + scenario_name + "-" +
            client.worker_boot().value() + "-" + std::to_string(iteration));

        PublishResultMessage result;
        const Status status = client.publish(topology.publication, result);
        std::cout << "publication=" << topology.publication.id.to_string()
                  << " generation=" << result.result.generation.to_string()
                  << " outcome=" << to_string(result.result.status.outcome())
                  << " nodes=" << topology.publication.nodes.size()
                  << " edges=" << topology.publication.edges.size() << "\n";
        if (status.outcome() != Outcome::Committed && status.outcome() != Outcome::Idempotent) {
            std::cout << "detail=" << status.one_line() << "\n";
        }
        std::cout.flush();
    }

    if (request_shutdown) {
        const Status status = client.request_shutdown("test", true);
        std::cout << "shutdown_requested=" << to_string(status.outcome()) << "\n";
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<long long>(hold_seconds * 1000.0));
    for (;;) {
        if (hold_seconds > 0.0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (!client.connected()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    client.close();
    return 0;
}
