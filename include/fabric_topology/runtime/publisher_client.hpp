// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_RUNTIME_PUBLISHER_CLIENT_HPP
#define FABRIC_TOPOLOGY_RUNTIME_PUBLISHER_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fabric_topology/authority.hpp"
#include "fabric_topology/limits.hpp"
#include "fabric_topology/publication.hpp"
#include "fabric_topology/result.hpp"
#include "fabric_topology/runtime/frame_transport.hpp"

namespace fabric_topology::runtime {

/// Produce a fresh worker boot identifier for this process incarnation. A restarted worker
/// receives a different value; the previous value stays stale forever.
[[nodiscard]] WorkerBootId make_worker_boot_id(std::string_view label);

struct PublisherClientOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    PublisherId publisher;
    WorkerBootId worker_boot;
    std::string process_label;
    std::vector<ScopeGrant> requested_grants;
    std::string token;
    PublicationType declared_evidence_type = PublicationType::Unsupported;
    /// Coordinator epoch the peer claims. Zero means "unset"; a non-zero value that does not
    /// match the live coordinator is refused as a survivor of a previous incarnation.
    CoordinatorEpoch claimed_epoch;
    bool observer = false;
    Limits limits{};
    /// Bounded wait for a response to a request. This is a request/response I/O deadline for
    /// a network protocol, not a test watchdog: an unresponsive coordinator must not hang a
    /// publisher forever.
    int response_deadline_ms = 30000;
};

/// Client side of the publication protocol. One client owns one connection.
class PublisherClient {
public:
    explicit PublisherClient(PublisherClientOptions options);
    ~PublisherClient();

    PublisherClient(const PublisherClient&) = delete;
    PublisherClient& operator=(const PublisherClient&) = delete;

    [[nodiscard]] Status connect_and_register(WelcomeMessage& welcome);
    [[nodiscard]] Status publish(const Publication& publication, PublishResultMessage& result);
    [[nodiscard]] Status heartbeat(HeartbeatMessage& acknowledgement);
    [[nodiscard]] Status query(const std::string& query, const std::string& argument,
                               QueryResultMessage& result);
    /// Ask the coordinator to shut down. Used by the lifecycle tests.
    [[nodiscard]] Status request_shutdown(const std::string& reason, bool graceful);

    void close();
    [[nodiscard]] bool connected() const noexcept { return transport_.valid() && registered_; }
    [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept { return welcome_.coordinator_epoch; }
    [[nodiscard]] TopologyGeneration topology_generation() const noexcept {
        return welcome_.topology_generation;
    }
    [[nodiscard]] const WelcomeMessage& welcome() const noexcept { return welcome_; }
    [[nodiscard]] const WorkerBootId& worker_boot() const noexcept { return options_.worker_boot; }
    [[nodiscard]] const PublisherId& publisher() const noexcept { return options_.publisher; }

private:
    [[nodiscard]] bool send_frame(MessageType type, std::string_view payload, std::string& error);
    [[nodiscard]] ReceiveOutcome await(MessageType expected, ReceivedFrame& frame, std::string& error);

    PublisherClientOptions options_;
    FrameTransport transport_;
    WelcomeMessage welcome_;
    bool registered_ = false;
    std::uint64_t heartbeat_counter_ = 0;
};

}  // namespace fabric_topology::runtime

#endif  // FABRIC_TOPOLOGY_RUNTIME_PUBLISHER_CLIENT_HPP
