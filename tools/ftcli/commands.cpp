// Fabric Topology inspection tool.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// The inspector is a thin consumer of the installed public API. No topology logic lives here.

#include "commands.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "fabric_topology/runtime/publisher_client.hpp"
#include "fabric_topology/runtime/registry_seed.hpp"

namespace ftcli {

namespace {

using namespace fabric_topology;

void print_usage(std::ostream& out) {
    out << "usage: ftcli <command> [options]\n"
           "\n"
           "Inspection of a durable topology image:\n"
           "  validate --state PATH\n"
           "  generation --state PATH\n"
           "  epoch --state PATH\n"
           "  digest --state PATH\n"
           "  statistics --state PATH\n"
           "  domains --state PATH\n"
           "  nodes --state PATH\n"
           "  edges --state PATH\n"
           "  node --state PATH --id NODE_ID\n"
           "  edge --state PATH --id EDGE_ID\n"
           "  entity --state PATH --id ENTITY_ID\n"
           "  neighbors --state PATH --id NODE_ID\n"
           "  ancestors --state PATH --id NODE_ID\n"
           "  descendants --state PATH --id NODE_ID\n"
           "  snapshot --state PATH\n"
           "  render --state PATH\n"
           "\n"
           "Other commands:\n"
           "  inspect-persistence --state PATH\n"
           "  synthetic --scenario NAME [--scale N] [--tiers N] --out PATH [--seed-out PATH]\n"
           "  discover-host [--out PATH] [--prefix NAME]\n"
           "  scenarios\n"
           "  query --host ADDRESS --port N --name QUERY [--argument ARG]\n"
           "  version\n";
}

struct Arguments {
    std::string command;
    std::string state;
    std::string id;
    std::string scenario;
    std::string out;
    std::string host = "127.0.0.1";
    std::string name;
    std::string argument;
    std::string prefix = "host";
    std::string seed_out;
    std::uint16_t port = 0;
    std::size_t scale = 0;
    std::size_t tiers = 0;
};

std::optional<Arguments> parse(const std::vector<std::string>& arguments, std::ostream& error) {
    if (arguments.empty()) {
        print_usage(error);
        return std::nullopt;
    }
    Arguments parsed;
    parsed.command = arguments.front();
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        const auto next = [&](std::string& target) {
            if (index + 1 >= arguments.size()) {
                error << "missing value for " << argument << "\n";
                return false;
            }
            target = arguments[++index];
            return true;
        };
        if (argument == "--state") {
            if (!next(parsed.state)) return std::nullopt;
        } else if (argument == "--id") {
            if (!next(parsed.id)) return std::nullopt;
        } else if (argument == "--scenario") {
            if (!next(parsed.scenario)) return std::nullopt;
        } else if (argument == "--out") {
            if (!next(parsed.out)) return std::nullopt;
        } else if (argument == "--host") {
            if (!next(parsed.host)) return std::nullopt;
        } else if (argument == "--name") {
            if (!next(parsed.name)) return std::nullopt;
        } else if (argument == "--argument") {
            if (!next(parsed.argument)) return std::nullopt;
        } else if (argument == "--seed-out") {
            if (!next(parsed.seed_out)) return std::nullopt;
        } else if (argument == "--prefix") {
            if (!next(parsed.prefix)) return std::nullopt;
        } else if (argument == "--port") {
            std::string value;
            if (!next(value)) return std::nullopt;
            parsed.port = static_cast<std::uint16_t>(std::stoi(value));
        } else if (argument == "--scale") {
            std::string value;
            if (!next(value)) return std::nullopt;
            parsed.scale = static_cast<std::size_t>(std::stoul(value));
        } else if (argument == "--tiers") {
            std::string value;
            if (!next(value)) return std::nullopt;
            parsed.tiers = static_cast<std::size_t>(std::stoul(value));
        } else {
            error << "unknown option: " << argument << "\n";
            return std::nullopt;
        }
    }
    return parsed;
}

struct LoadedEngine {
    std::unique_ptr<TopologyEngine> engine;
    LoadReport report;
};

std::optional<LoadedEngine> load_engine(const Arguments& arguments, std::ostream& error) {
    if (arguments.state.empty()) {
        error << "--state PATH is required\n";
        return std::nullopt;
    }
    LoadedEngine loaded;
    TopologyEngineOptions options;
    options.directory = std::make_shared<InMemoryEntityDirectory>();
    options.persistence_path = arguments.state;
    loaded.engine = TopologyEngine::open(options, loaded.report);
    if (loaded.report.status.outcome() != Outcome::Ok &&
        loaded.report.status.outcome() != Outcome::Committed) {
        error << "failed to load " << arguments.state << ": " << loaded.report.status.one_line()
              << "\n";
        return std::nullopt;
    }
    return loaded;
}

void print_report(const LoadReport& report, std::ostream& out) {
    out << "loaded=" << (report.loaded ? "true" : "false")
        << " generation=" << report.generation.to_string()
        << " coordinator_epoch=" << report.coordinator_epoch.to_string() << " nodes=" << report.nodes
        << " edges=" << report.edges
        << " revalidation_required_edges=" << report.revalidation_required_edges
        << " revalidation_required_nodes=" << report.revalidation_required_nodes << "\n";
}

int run_loaded(const Arguments& arguments, const LoadedEngine& loaded, std::ostream& out,
               std::ostream& error) {
    TopologyEngine& engine = *loaded.engine;
    const std::string& command = arguments.command;
    if (command == "validate") {
        out << engine.validate().render() << "\n";
        return 0;
    }
    if (command == "generation") {
        out << engine.generation().to_string() << "\n";
        return 0;
    }
    if (command == "epoch") {
        out << engine.coordinator_epoch().to_string() << "\n";
        return 0;
    }
    if (command == "digest") {
        out << engine.digest() << "\n";
        return 0;
    }
    if (command == "statistics") {
        out << engine.statistics() << "\n";
        return 0;
    }
    if (command == "domains") {
        const std::vector<DomainDefinition> domains = engine.domains();
        out << "domains=" << domains.size() << "\n";
        for (const DomainDefinition& definition : domains) {
            out << "  " << definition.id.to_string() << " kind=" << to_string(definition.kind);
            if (!definition.anchor_entity_id.empty()) {
                out << " anchor=" << definition.anchor_entity_id;
            }
            if (!definition.parent.empty()) {
                out << " parent=" << definition.parent.to_string();
            }
            out << "\n";
        }
        return 0;
    }
    if (command == "nodes") {
        const std::vector<TopologyNode> nodes = engine.all_nodes();
        out << "nodes=" << nodes.size() << "\n";
        for (const TopologyNode& node : nodes) {
            out << "  " << render_node(node) << "\n";
        }
        return 0;
    }
    if (command == "edges") {
        const std::vector<TopologyEdge> edges = engine.all_edges();
        out << "edges=" << edges.size() << "\n";
        for (const TopologyEdge& edge : edges) {
            out << "  " << render_edge(edge) << "\n";
        }
        return 0;
    }
    if (command == "node") {
        const auto parsed = TopologyNodeId::parse(arguments.id);
        if (!parsed.has_value()) {
            error << "invalid --id\n";
            return 2;
        }
        Explanation explanation;
        const Status status = engine.explain_node(*parsed, explanation);
        out << explanation.render() << "\n";
        return outcome_is_success(status.outcome()) ? 0 : 1;
    }
    if (command == "edge") {
        const auto parsed = TopologyEdgeId::parse(arguments.id);
        if (!parsed.has_value()) {
            error << "invalid --id\n";
            return 2;
        }
        Explanation explanation;
        const Status status = engine.explain_relationship(*parsed, explanation);
        out << explanation.render() << "\n";
        return outcome_is_success(status.outcome()) ? 0 : 1;
    }
    if (command == "entity") {
        Explanation explanation;
        const std::optional<TopologyNode> node = engine.node_for_entity(arguments.id);
        if (!node.has_value()) {
            out << "NOT_FOUND entity=" << arguments.id << "\n";
            return 1;
        }
        const Status status = engine.explain_node(node->id, explanation);
        out << explanation.render() << "\n";
        return outcome_is_success(status.outcome()) ? 0 : 1;
    }
    if (command == "neighbors" || command == "ancestors" || command == "descendants") {
        const auto parsed = TopologyNodeId::parse(arguments.id);
        if (!parsed.has_value()) {
            error << "invalid --id\n";
            return 2;
        }
        TraversalRequest request;
        request.origin = *parsed;
        request.max_depth = command == "neighbors" ? 1 : 64;
        request.max_visited = 10000;
        request.kind = command == "neighbors"   ? TraversalKind::Neighbors
                       : command == "ancestors" ? TraversalKind::Ancestors
                                                : TraversalKind::Descendants;
        out << engine.traverse(request).render() << "\n";
        return 0;
    }
    if (command == "snapshot") {
        out << engine.snapshot().render() << "\n";
        return 0;
    }
    if (command == "render") {
        out << engine.render_canonical() << "\n";
        return 0;
    }
    error << "unknown command: " << command << "\n";
    print_usage(error);
    return 2;
}

int write_state(TopologyEngine& engine, const std::string& path, std::ostream& error) {
    const Status saved = engine.save_to(path);
    if (saved.outcome() != Outcome::Ok) {
        error << "failed to write " << path << ": " << saved.one_line() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace

int run(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& error) {
    const std::optional<Arguments> parsed = parse(arguments, error);
    if (!parsed.has_value()) {
        return 2;
    }
    const Arguments& options = *parsed;

    if (options.command == "version") {
        out << product_banner() << "\n";
        return 0;
    }
    if (options.command == "scenarios") {
        for (const SyntheticScenario scenario : synthetic_scenarios()) {
            out << to_string(scenario) << "\n";
        }
        return 0;
    }
    if (options.command == "inspect-persistence") {
        if (options.state.empty()) {
            error << "--state PATH is required\n";
            return 2;
        }
        PersistenceHeaderInfo info;
        const Status status = inspect_persistence_file(options.state, info);
        out << status.render() << "\n";
        if (status.outcome() != Outcome::Ok) {
            return 1;
        }
        out << "format_version=" << info.format_version << " payload_bytes=" << info.payload_bytes
            << " payload_crc32=" << info.payload_crc32 << " payload_sha256=" << info.payload_sha256
            << " file_bytes=" << info.file_bytes << "\n";
        return 0;
    }
    if (options.command == "synthetic") {
        const auto scenario = synthetic_scenario_from_string(options.scenario);
        if (!scenario.has_value()) {
            error << "unknown scenario: " << options.scenario << "\n";
            return 2;
        }
        SyntheticOptions synthetic;
        synthetic.scenario = *scenario;
        synthetic.scale = options.scale;
        synthetic.tiers = options.tiers;
        synthetic.domain = TopologyDomainId::from_trusted("dom-synthetic");
        synthetic.publisher = PublisherId::from_trusted("pub-cli");
        const SyntheticTopology topology = build_synthetic_topology(synthetic);
        out << topology.render() << "\n";

        auto directory = std::make_shared<InMemoryEntityDirectory>();
        for (const EntityRecord& record : topology.directory_entries) {
            static_cast<void>(directory->add(record));
        }
        TopologyEngineOptions engine_options;
        engine_options.directory = directory;
        TopologyEngine engine(engine_options);
        DomainDefinition definition;
        definition.id = synthetic.domain;
        definition.kind = ScopeKind::AdministrativeDomain;
        static_cast<void>(engine.define_domain(definition));
        PublisherRegistration registration;
        registration.publisher = synthetic.publisher;
        registration.worker_boot = WorkerBootId::from_trusted("boot-cli");
        registration.coordinator_epoch = engine.coordinator_epoch();
        ScopeGrant grant;
        grant.domain = synthetic.domain;
        grant.mode = GrantMode::AuthoritativeWrite;
        registration.grants.push_back(grant);
        static_cast<void>(engine.register_publisher(registration));
        AuthorityContext context;
        context.coordinator_epoch = engine.coordinator_epoch();
        context.publisher = synthetic.publisher;
        context.worker_boot = WorkerBootId::from_trusted("boot-cli");
        const PublicationResult result = engine.publish(context, topology.publication);
        out << result.render() << "\n";
        if (!result.committed()) {
            return 1;
        }
        for (const Publication& follow_up : topology.secondary_publications) {
            static_cast<void>(engine.publish(context, follow_up));
        }
        if (!options.seed_out.empty()) {
            const Status written =
                runtime::write_registry_seed(options.seed_out, topology.directory_entries);
            if (!outcome_is_success(written.outcome())) {
                error << "failed to write registry seed: " << written.one_line() << "\n";
                return 1;
            }
        }
        if (options.out.empty()) {
            return 0;
        }
        return write_state(engine, options.out, error);
    }
    if (options.command == "discover-host") {
        if (!host_discovery_supported()) {
            error << "host discovery is not supported on this platform\n";
            return 77;
        }
        const HostTopologyEvidence evidence = discover_host_topology();
        out << "platform=" << evidence.platform << " host=" << evidence.host_name
            << " interfaces=" << evidence.interfaces.size() << "\n";
        for (const std::string& capability : evidence.unsupported) {
            out << "unsupported=" << capability << "\n";
        }
        for (const HostInterfaceEvidence& interface : evidence.interfaces) {
            out << "interface=" << interface.interface_guid << " name=\"" << interface.friendly_name
                << "\" mac=" << interface.mac_address << " mtu=" << interface.mtu
                << " up=" << (interface.oper_up ? "true" : "false")
                << " pnp=" << interface.pnp_device_id << " ipv4=";
            for (const std::string& address : interface.ipv4_addresses) {
                out << address << ',';
            }
            out << " gateways=";
            for (const std::string& address : interface.gateway_addresses) {
                out << address << ',';
            }
            out << "\n";
        }
        if (options.out.empty()) {
            return 0;
        }
        HostDiscoveryOptions discovery;
        discovery.domain = TopologyDomainId::from_trusted("dom-host");
        discovery.publisher = PublisherId::from_trusted("pub-cli");
        discovery.entity_prefix = options.prefix;
        const HostDiscoveryResult result = build_host_publication(evidence, discovery);

        auto directory = std::make_shared<InMemoryEntityDirectory>();
        for (const EntityRecord& record : result.directory_entries) {
            static_cast<void>(directory->add(record));
        }
        TopologyEngineOptions engine_options;
        engine_options.directory = directory;
        TopologyEngine engine(engine_options);
        DomainDefinition definition;
        definition.id = discovery.domain;
        definition.kind = ScopeKind::AdministrativeDomain;
        static_cast<void>(engine.define_domain(definition));
        PublisherRegistration registration;
        registration.publisher = discovery.publisher;
        registration.worker_boot = WorkerBootId::from_trusted("boot-cli");
        registration.coordinator_epoch = engine.coordinator_epoch();
        ScopeGrant grant;
        grant.domain = discovery.domain;
        grant.mode = GrantMode::AuthoritativeWrite;
        registration.grants.push_back(grant);
        static_cast<void>(engine.register_publisher(registration));
        AuthorityContext context;
        context.coordinator_epoch = engine.coordinator_epoch();
        context.publisher = discovery.publisher;
        context.worker_boot = WorkerBootId::from_trusted("boot-cli");
        const PublicationResult published = engine.publish(context, result.publication);
        out << published.render() << "\n";
        if (!published.committed()) {
            return 1;
        }
        return write_state(engine, options.out, error);
    }
    if (options.command == "query") {
        if (options.port == 0 || options.name.empty()) {
            error << "--port and --name are required\n";
            return 2;
        }
        runtime::PublisherClientOptions client_options;
        client_options.host = options.host;
        client_options.port = options.port;
        client_options.publisher = PublisherId::from_trusted("pub-cli-observer");
        client_options.worker_boot = runtime::make_worker_boot_id("ftcli");
        client_options.process_label = "ftcli";
        client_options.observer = true;
        runtime::PublisherClient client(std::move(client_options));
        WelcomeMessage welcome;
        const Status registered = client.connect_and_register(welcome);
        if (!outcome_is_success(registered.outcome())) {
            error << "query attach failed: " << registered.one_line() << "\n";
            return 1;
        }
        QueryResultMessage result;
        const Status status = client.query(options.name, options.argument, result);
        out << result.text << "\n";
        client.close();
        return outcome_is_success(status.outcome()) ? 0 : 1;
    }

    const std::optional<LoadedEngine> loaded = load_engine(options, error);
    if (!loaded.has_value()) {
        return 1;
    }
    print_report(loaded->report, out);
    return run_loaded(options, *loaded, out, error);
}

}  // namespace ftcli
