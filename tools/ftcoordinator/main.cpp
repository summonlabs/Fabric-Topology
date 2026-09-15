// Fabric Topology coordinator process.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "fabric_topology/runtime/coordinator.hpp"
#include "fabric_topology/runtime/registry_seed.hpp"

namespace {

using namespace fabric_topology;

void print_usage() {
    std::cout << "usage: ftcoordinator [options]\n"
                 "  --bind ADDRESS            bind address (default 127.0.0.1)\n"
                 "  --port N                  listen port, 0 selects an ephemeral port (default 0)\n"
                 "  --port-file PATH          write the chosen port to PATH once listening\n"
                 "  --state PATH              durable topology image (enables recovery)\n"
                 "  --domain ID               define an administrative authority scope (repeatable)\n"
                 "  --grant DOMAIN=MODE       delegation policy; MODE is none|read|incremental|authoritative\n"
                 "  --registry-seed PATH      load canonical Fabric Registry identities from PATH (repeatable)\n"
                 "  --token TOKEN             shared secret required from publishers\n"
                 "  --max-sessions N          concurrent publisher sessions (default 64)\n"
                 "  --exit-after-seconds N    stop automatically after N seconds (0 waits indefinitely)\n";
}

std::optional<GrantMode> parse_mode(const std::string& text) {
    if (text == "none") return GrantMode::None;
    if (text == "read") return GrantMode::Read;
    if (text == "incremental") return GrantMode::IncrementalWrite;
    if (text == "authoritative") return GrantMode::AuthoritativeWrite;
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    runtime::CoordinatorOptions options;
    auto registry = std::make_shared<InMemoryEntityDirectory>();
    options.engine.directory = registry;
    options.engine.verify_indexes_on_mutation = false;
    std::string port_file;
    std::string state_path;
    std::vector<std::string> registry_seeds;
    double exit_after_seconds = 0.0;

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
        if (argument == "--bind") {
            options.bind_address = next();
        } else if (argument == "--port") {
            options.port = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (argument == "--port-file") {
            port_file = next();
        } else if (argument == "--state") {
            state_path = next();
        } else if (argument == "--registry-seed") {
            registry_seeds.push_back(next());
        } else if (argument == "--token") {
            options.token = next();
        } else if (argument == "--max-sessions") {
            options.max_sessions = static_cast<std::size_t>(std::stoul(next()));
        } else if (argument == "--exit-after-seconds") {
            exit_after_seconds = std::stod(next());
        } else if (argument == "--domain") {
            DomainDefinition definition;
            const std::string id = next();
            const auto parsed = TopologyDomainId::parse(id);
            if (!parsed.has_value()) {
                std::cerr << "invalid domain identifier: " << id << "\n";
                return 2;
            }
            definition.id = *parsed;
            definition.kind = ScopeKind::AdministrativeDomain;
            options.domains.push_back(definition);
        } else if (argument == "--grant") {
            const std::string value = next();
            const std::size_t separator = value.find('=');
            if (separator == std::string::npos) {
                std::cerr << "invalid grant (expected DOMAIN=MODE): " << value << "\n";
                return 2;
            }
            const auto domain = TopologyDomainId::parse(value.substr(0, separator));
            const auto mode = parse_mode(value.substr(separator + 1));
            if (!domain.has_value() || !mode.has_value()) {
                std::cerr << "invalid grant: " << value << "\n";
                return 2;
            }
            ScopeGrant grant;
            grant.domain = *domain;
            grant.mode = *mode;
            options.grant_policy.push_back(grant);
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            print_usage();
            return 2;
        }
    }

    if (!state_path.empty()) {
        options.engine.persistence_path = state_path;
        options.engine.persistence_mode = PersistenceMode::Immediate;
    }

    for (const std::string& seed : registry_seeds) {
        const Status loaded = runtime::load_registry_seed(seed, *registry);
        if (outcome_is_success(loaded.outcome())) {
            std::cout << "registry_seed=" << seed << " records="
                      << (loaded.find("records") != nullptr ? *loaded.find("records") : "0") << "\n";
        } else {
            std::cerr << "failed to load registry seed " << seed << ": " << loaded.one_line() << "\n";
            return 1;
        }
    }

    LoadReport report;
    std::unique_ptr<TopologyEngine> engine = TopologyEngine::open(options.engine, report);
    if (report.loaded) {
        std::cout << "recovered=true generation=" << report.generation.to_string()
                  << " coordinator_epoch=" << report.coordinator_epoch.to_string()
                  << " nodes=" << report.nodes << " edges=" << report.edges
                  << " revalidation_required_edges=" << report.revalidation_required_edges << "\n";
    }

    runtime::TopologyCoordinator coordinator(std::move(engine), options);
    const Status started = coordinator.start();
    if (started.outcome() != Outcome::Committed) {
        std::cerr << "failed to start coordinator: " << started.one_line() << "\n";
        return 1;
    }

    if (!port_file.empty()) {
        std::ofstream stream(port_file, std::ios::trunc);
        stream << coordinator.port() << "\n";
        stream.flush();
    }
    std::cout << "coordinator listening on " << options.bind_address << ":" << coordinator.port()
              << " coordinator_epoch=" << coordinator.engine().coordinator_epoch().to_string()
              << " topology_generation=" << coordinator.engine().generation().to_string() << "\n";
    std::cout.flush();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<long long>(exit_after_seconds * 1000.0));
    while (coordinator.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (exit_after_seconds > 0.0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }

    coordinator.stop();
    if (!state_path.empty()) {
        const Status saved = coordinator.engine().save();
        if (saved.outcome() != Outcome::Ok) {
            std::cerr << "final save failed: " << saved.one_line() << "\n";
            return 1;
        }
    }
    std::cout << "coordinator stopped sessions=" << coordinator.session_count()
              << " fenced_workers=" << coordinator.fenced_workers() << "\n";
    return 0;
}
