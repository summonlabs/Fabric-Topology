// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_RUNTIME_COORDINATOR_HPP
#define FABRIC_TOPOLOGY_RUNTIME_COORDINATOR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fabric_topology/result.hpp"
#include "fabric_topology/topology.hpp"

namespace fabric_topology::runtime {

struct CoordinatorOptions {
    std::string bind_address = "127.0.0.1";
    /// Zero selects an ephemeral port; the chosen port is reported by port().
    std::uint16_t port = 0;
    /// Engine options used only by the convenience constructor that owns engine creation.
    TopologyEngineOptions engine;
    /// Authority scopes the coordinator exposes. A publisher may only be granted a scope the
    /// coordinator has defined here.
    std::vector<DomainDefinition> domains;
    /// Delegation policy: the coordinator grants at most what it is configured to grant.
    /// A requested scope with no matching policy entry is granted GrantMode::None.
    std::vector<ScopeGrant> grant_policy;
    /// Optional shared secret. When non-empty, a publisher must present it in its Hello.
    std::string token;
    std::size_t max_sessions = 64;
    int poll_interval_ms = 50;
};

/// Multi-process topology coordinator.
///
/// One accept thread and one session thread per connected publisher. Publisher authority is
/// bound to the (publisher, worker boot, coordinator epoch) triple carried in the handshake.
/// When a session's socket dies, the controller fences that worker boot through the real
/// control path and demotes the relationships that incarnation asserted.
class TopologyCoordinator {
public:
    explicit TopologyCoordinator(CoordinatorOptions options);
    /// Adopt an already-opened engine (for example one recovered from a durable image).
    TopologyCoordinator(std::unique_ptr<TopologyEngine> engine, CoordinatorOptions options);
    ~TopologyCoordinator();

    TopologyCoordinator(const TopologyCoordinator&) = delete;
    TopologyCoordinator& operator=(const TopologyCoordinator&) = delete;

    [[nodiscard]] Status start();
    /// Stop accepting, signal every session, join every thread, then close the listener.
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] TopologyEngine& engine() noexcept { return *engine_; }
    [[nodiscard]] const TopologyEngine& engine() const noexcept { return *engine_; }
    [[nodiscard]] std::size_t session_count() const;
    /// Sessions whose peer is still connected and whose handler thread is still running.
    [[nodiscard]] std::size_t active_session_count() const;
    /// Number of worker incarnations fenced because their session died.
    [[nodiscard]] std::uint64_t fenced_workers() const noexcept;
    [[nodiscard]] std::vector<std::string> session_descriptions() const;
    [[nodiscard]] std::string statistics() const;
    [[nodiscard]] bool running() const noexcept;
    /// Apply the configured domains and delegation policy to the engine.
    [[nodiscard]] Status configure_authority();

private:
    struct Session;
    struct Listener;
    /// Highest grant mode the configured delegation policy allows for a scope.
    [[nodiscard]] GrantMode delegated_mode(const TopologyDomainId& domain) const;
    void accept_loop();
    void run_session(std::shared_ptr<Session> session);

    CoordinatorOptions options_;
    std::unique_ptr<TopologyEngine> engine_;
    std::shared_ptr<Listener> listener_;
    std::vector<std::shared_ptr<Session>> sessions_;
    std::atomic<bool> stop_{true};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> fenced_workers_{0};
    std::atomic<std::uint64_t> accepted_sessions_{0};
    std::atomic<std::uint64_t> rejected_sessions_{0};
    std::atomic<std::uint64_t> published_publications_{0};
    mutable std::mutex sessions_mutex_;
};

}  // namespace fabric_topology::runtime

#endif  // FABRIC_TOPOLOGY_RUNTIME_COORDINATOR_HPP
