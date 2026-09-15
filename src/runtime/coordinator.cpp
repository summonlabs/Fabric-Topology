// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/runtime/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fabric_topology/codec.hpp"
#include "fabric_topology/protocol.hpp"
#include "fabric_topology/runtime/frame_transport.hpp"

namespace fabric_topology::runtime {

namespace {

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] std::string sanitize_identifier(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '.' || c == '_' || c == '-' || c == '~' || c == ':';
        out.push_back(ok ? c : '-');
    }
    if (out.empty()) {
        out = "unnamed";
    }
    if (!((out.front() >= '0' && out.front() <= '9') ||
          (out.front() >= 'a' && out.front() <= 'z') ||
          (out.front() >= 'A' && out.front() <= 'Z'))) {
        out.insert(out.begin(), 'x');
    }
    if (out.size() > kMaxIdentifierBytes) {
        out.resize(kMaxIdentifierBytes);
    }
    return out;
}

[[nodiscard]] Status send_error(FrameTransport& transport, Outcome outcome, const char* code,
                                const std::string& detail) {
    ErrorReportMessage message;
    message.error.set_outcome(outcome).set_code(code);
    if (!detail.empty()) {
        message.error.with("detail", detail);
    }
    RecordWriter writer(128);
    encode_message(message, writer);
    std::string error;
    static_cast<void>(transport.send(MessageType::ErrorReport, 0, writer.data(), error));
    return message.error;
}

}  // namespace

struct TopologyCoordinator::Listener {
    FrameTransport transport;
    std::thread thread;
    std::atomic<bool> stop{false};
};

struct TopologyCoordinator::Session {
    FrameTransport transport;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    std::string peer;
    PublisherId publisher;
    WorkerBootId worker_boot;
    std::string process_label;
    bool registered = false;
    bool observer = false;
    bool fenced = false;
    std::uint64_t frames = 0;
    std::uint64_t publications = 0;
    std::string end_reason;
};

TopologyCoordinator::TopologyCoordinator(CoordinatorOptions options)
    : options_(std::move(options)),
      engine_(std::make_unique<TopologyEngine>(options_.engine)),
      listener_(std::make_shared<Listener>()) {}

TopologyCoordinator::TopologyCoordinator(std::unique_ptr<TopologyEngine> engine, CoordinatorOptions options)
    : options_(std::move(options)),
      engine_(std::move(engine)),
      listener_(std::make_shared<Listener>()) {
    if (engine_ == nullptr) {
        engine_ = std::make_unique<TopologyEngine>(options_.engine);
    }
}

Status TopologyCoordinator::configure_authority() {
    for (const DomainDefinition& definition : options_.domains) {
        const Status status = engine_->define_domain(definition);
        if (status.outcome() != Outcome::Committed && status.outcome() != Outcome::Idempotent) {
            return status;
        }
    }
    Status status = make_status(Outcome::Ok, "coordinator.authority_configured");
    status.with("domains", std::to_string(options_.domains.size()));
    status.with("grant_rules", std::to_string(options_.grant_policy.size()));
    return status;
}

GrantMode TopologyCoordinator::delegated_mode(const TopologyDomainId& domain) const {
    GrantMode best = GrantMode::None;
    bool defined = false;
    for (const DomainDefinition& definition : options_.domains) {
        if (definition.id == domain) {
            defined = true;
            break;
        }
    }
    if (!defined) {
        return GrantMode::None;
    }
    for (const ScopeGrant& grant : options_.grant_policy) {
        if (grant.domain == domain && grant.mode > best) {
            best = grant.mode;
        }
    }
    return best;
}

TopologyCoordinator::~TopologyCoordinator() { stop(); }

std::uint16_t TopologyCoordinator::port() const noexcept {
    if (listener_ == nullptr) {
        return 0;
    }
    return listener_->transport.local_port();
}

bool TopologyCoordinator::running() const noexcept { return running_.load(); }

std::size_t TopologyCoordinator::session_count() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.size();
}

std::uint64_t TopologyCoordinator::fenced_workers() const noexcept { return fenced_workers_.load(); }

std::size_t TopologyCoordinator::active_session_count() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::size_t active = 0;
    for (const std::shared_ptr<Session>& session : sessions_) {
        if (!session->finished.load()) {
            ++active;
        }
    }
    return active;
}

std::vector<std::string> TopologyCoordinator::session_descriptions() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::vector<std::string> result;
    result.reserve(sessions_.size());
    for (const std::shared_ptr<Session>& session : sessions_) {
        std::string text = "peer=";
        text += session->peer;
        text += " publisher=";
        text += session->publisher.to_string();
        text += " boot=";
        text += session->worker_boot.to_string();
        text += " registered=";
        text += session->registered ? "true" : "false";
        text += " observer=";
        text += session->observer ? "true" : "false";
        text += " fenced=";
        text += session->fenced ? "true" : "false";
        text += " frames=";
        text += std::to_string(session->frames);
        text += " publications=";
        text += std::to_string(session->publications);
        text += " finished=";
        text += session->finished.load() ? "true" : "false";
        if (!session->end_reason.empty()) {
            text += " end_reason=";
            text += session->end_reason;
        }
        result.push_back(std::move(text));
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string TopologyCoordinator::statistics() const {
    std::string out;
    out.reserve(384);
    out += "port=";
    out += std::to_string(port());
    out += "\nbind=";
    out += options_.bind_address;
    out += "\nsessions=";
    out += std::to_string(session_count());
    out += "\naccepted_sessions=";
    out += std::to_string(accepted_sessions_.load());
    out += "\nrejected_sessions=";
    out += std::to_string(rejected_sessions_.load());
    out += "\nfenced_workers=";
    out += std::to_string(fenced_workers_.load());
    out += "\npublished_publications=";
    out += std::to_string(published_publications_.load());
    out += "\ncoordinator_epoch=";
    out += engine_->coordinator_epoch().to_string();
    out += "\nrunning=";
    out += running() ? "true" : "false";
    out += "\n";
    out += engine_->statistics();
    return out;
}

Status TopologyCoordinator::start() {
    if (running_.load()) {
        return make_status(Outcome::Idempotent, "coordinator.already_running");
    }
    const Status configured = configure_authority();
    if (configured.outcome() != Outcome::Ok) {
        return configured;
    }
    stop_.store(false);

    std::string error;
    if (!listener_->transport.listen(options_.bind_address, options_.port, error)) {
        Status status = make_status(Outcome::InternalError, "coordinator.listen_failed");
        status.with("detail", error);
        return status;
    }
    listener_->transport.set_poll_interval_ms(options_.poll_interval_ms);
    listener_->stop.store(false);
    running_.store(true);
    listener_->thread = std::thread([this] { accept_loop(); });

    Status status = make_status(Outcome::Committed, "coordinator.started");
    status.with("port", std::to_string(port()));
    status.with("coordinator_epoch", engine_->coordinator_epoch().to_string());
    status.with("topology_generation", engine_->generation().to_string());
    return status;
}

void TopologyCoordinator::accept_loop() {
    while (!stop_.load()) {
        std::string error;
        bool timed_out = false;
        std::optional<FrameTransport> client = listener_->transport.accept_client(error, timed_out);
        if (!client.has_value()) {
            if (timed_out) {
                continue;
            }
            if (!error.empty()) {
                break;
            }
            continue;
        }

        auto session = std::make_shared<Session>();
        session->transport = std::move(*client);
        session->transport.set_poll_interval_ms(options_.poll_interval_ms);
        session->peer = session->transport.peer_description();

        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            if (sessions_.size() >= options_.max_sessions) {
                ++rejected_sessions_;
                std::string send_error_text;
                static_cast<void>(session->transport.send(
                    MessageType::ErrorReport, 0,
                    [&] {
                        ErrorReportMessage message;
                        message.error.set_outcome(Outcome::ResourceLimit)
                            .set_code("coordinator.session_limit");
                        RecordWriter writer(64);
                        encode_message(message, writer);
                        return writer.take();
                    }(),
                    send_error_text));
                continue;
            }
            sessions_.push_back(session);
            ++accepted_sessions_;
            session->thread = std::thread([this, session] { run_session(session); });
        }
    }
}

void TopologyCoordinator::run_session(std::shared_ptr<Session> session) {
    FrameTransport& transport = session->transport;

    // Handshake window: a peer that connects and never speaks must not hold a slot forever.
    const auto handshake_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    HelloMessage hello;
    bool handshake_ok = false;
    for (;;) {
        ReceivedFrame frame;
        std::string error;
        const ReceiveOutcome outcome = transport.receive(frame, error);
        if (outcome == ReceiveOutcome::Frame) {
            if (frame.header.type != MessageType::Hello) {
                static_cast<void>(send_error(transport, Outcome::ProtocolViolation, "coordinator.hello_expected",
                           to_string(frame.header.type)));
                session->end_reason = "hello_expected";
                break;
            }
            RecordReader reader(frame.payload, options_.engine.limits);
            if (!decode_message(reader, hello)) {
                static_cast<void>(send_error(transport, Outcome::ProtocolViolation, "coordinator.hello_malformed",
                           reader.error()));
                session->end_reason = "hello_malformed";
                break;
            }
            handshake_ok = true;
            break;
        }
        if (outcome == ReceiveOutcome::Idle) {
            if (stop_.load()) {
                session->end_reason = "coordinator_stopping";
                break;
            }
            if (std::chrono::steady_clock::now() >= handshake_deadline) {
                static_cast<void>(send_error(transport, Outcome::ProtocolViolation, "coordinator.handshake_timeout", {}));
                session->end_reason = "handshake_timeout";
                break;
            }
            continue;
        }
        session->end_reason = (outcome == ReceiveOutcome::Closed) ? "peer_closed" : "receive_error";
        break;
    }

    if (handshake_ok) {
        session->publisher = hello.publisher;
        session->worker_boot = hello.worker_boot;
        session->process_label = hello.process_label;
        session->observer = hello.observer;

        WelcomeMessage welcome;
        welcome.coordinator_epoch = engine_->coordinator_epoch();
        welcome.topology_generation = engine_->generation();
        welcome.session_id = sanitize_identifier(session->peer);

        const bool token_ok = options_.token.empty() || hello.token == options_.token;
        // A peer that claims an epoch other than the live one is a survivor of a previous
        // coordinator incarnation and is refused before any authority is considered.
        const bool epoch_ok = hello.coordinator_epoch.is_zero() ||
                              hello.coordinator_epoch == engine_->coordinator_epoch();
        if (!epoch_ok) {
            welcome.accepted = false;
            welcome.reason.set_outcome(Outcome::StaleCoordinatorEpoch)
                .set_code("coordinator.epoch_stale");
            welcome.reason.with("claimed_epoch", hello.coordinator_epoch.to_string());
            welcome.reason.with("current_epoch", engine_->coordinator_epoch().to_string());
            ++rejected_sessions_;
            session->end_reason = "epoch_stale";
        } else if (!token_ok) {
            welcome.accepted = false;
            welcome.reason.set_outcome(Outcome::StaleAuthority).set_code("coordinator.token_rejected");
            ++rejected_sessions_;
            session->end_reason = "token_rejected";
        } else if (session->observer) {
            welcome.accepted = true;
            welcome.reason.set_outcome(Outcome::Ok).set_code("coordinator.observer_attached");
        } else {
            PublisherRegistration registration;
            registration.publisher = hello.publisher;
            registration.worker_boot = hello.worker_boot;
            registration.coordinator_epoch = engine_->coordinator_epoch();
            // Delegation: a publisher receives at most what the coordinator policy allows.
            for (const ScopeGrant& requested : hello.requested_grants) {
                ScopeGrant granted;
                granted.domain = requested.domain;
                granted.mode = requested.mode < delegated_mode(requested.domain)
                                   ? requested.mode
                                   : delegated_mode(requested.domain);
                if (granted.mode != GrantMode::None) {
                    registration.grants.push_back(granted);
                }
            }
            registration.reason = hello.process_label;
            const Status registered = engine_->register_publisher(registration);
            welcome.accepted = outcome_is_success(registered.outcome());
            welcome.granted = hello.requested_grants;
            welcome.reason = registered;
            if (!welcome.accepted) {
                ++rejected_sessions_;
                session->end_reason = "registration_rejected";
            } else {
                session->registered = true;
            }
        }

        RecordWriter writer(256);
        encode_message(welcome, writer);
        std::string error;
        if (!transport.send(MessageType::Welcome, 0, writer.data(), error)) {
            session->end_reason = "welcome_send_failed";
        } else if (!welcome.accepted) {
            // Rejected peers are disconnected immediately.
        } else {
            for (;;) {
                ReceivedFrame frame;
                std::string receive_error;
                const ReceiveOutcome outcome = transport.receive(frame, receive_error);
                if (outcome == ReceiveOutcome::Idle) {
                    if (stop_.load()) {
                        session->end_reason = "coordinator_stopping";
                        break;
                    }
                    continue;
                }
                if (outcome == ReceiveOutcome::Closed) {
                    session->end_reason = "peer_closed";
                    break;
                }
                if (outcome == ReceiveOutcome::Error) {
                    session->end_reason = receive_error.empty() ? "receive_error" : receive_error;
                    break;
                }
                ++session->frames;
                const MessageType type = frame.header.type;
                RecordReader reader(frame.payload, options_.engine.limits);

                if (type == MessageType::Heartbeat) {
                    HeartbeatMessage request;
                    if (!decode_message(reader, request)) {
                        session->end_reason = "heartbeat_malformed";
                        break;
                    }
                    HeartbeatMessage reply = request;
                    reply.coordinator_epoch = engine_->coordinator_epoch();
                    RecordWriter heartbeat_writer(64);
                    encode_message(reply, heartbeat_writer);
                    std::string heartbeat_error;
                    if (!transport.send(MessageType::Heartbeat, 0, heartbeat_writer.data(), heartbeat_error)) {
                        session->end_reason = "heartbeat_send_failed";
                        break;
                    }
                    continue;
                }

                if (type == MessageType::Publish) {
                    PublishMessage request;
                    if (!decode_message(reader, request)) {
                        static_cast<void>(send_error(transport, Outcome::ProtocolViolation, "coordinator.publish_malformed",
                                   reader.error()));
                        session->end_reason = "publish_malformed";
                        break;
                    }
                    if (session->observer) {
                        static_cast<void>(send_error(transport, Outcome::UnauthorizedScope,
                                   "coordinator.observer_cannot_publish", {}));
                        session->end_reason = "observer_publish";
                        break;
                    }
                    // The coordinator binds authority to the session, never to what the client
                    // claims about itself.
                    AuthorityContext authority;
                    authority.coordinator_epoch = engine_->coordinator_epoch();
                    authority.publisher = session->publisher;
                    authority.worker_boot = session->worker_boot;
                    authority.evidence_generation = request.publication.evidence_generation;
                    authority.publication = request.publication.id;
                    authority.attempt = request.authority.attempt;

                    PublishResultMessage result;
                    result.result = engine_->publish(authority, request.publication);
                    ++published_publications_;
                    ++session->publications;

                    RecordWriter result_writer(1024);
                    encode_message(result, result_writer);
                    std::string result_error;
                    if (!transport.send(MessageType::PublishResult, 0, result_writer.data(), result_error)) {
                        session->end_reason = "publish_result_send_failed";
                        break;
                    }
                    continue;
                }

                if (type == MessageType::Query) {
                    QueryMessage request;
                    if (!decode_message(reader, request)) {
                        static_cast<void>(send_error(transport, Outcome::ProtocolViolation, "coordinator.query_malformed",
                                   reader.error()));
                        session->end_reason = "query_malformed";
                        break;
                    }
                    QueryResultMessage response;
                    response.status.set_outcome(Outcome::Ok).set_code("coordinator.query_ok");
                    if (request.query == "statistics") {
                        response.text = statistics();
                    } else if (request.query == "generation") {
                        response.text = engine_->generation().to_string();
                    } else if (request.query == "epoch") {
                        response.text = engine_->coordinator_epoch().to_string();
                    } else if (request.query == "digest") {
                        response.text = engine_->digest();
                    } else if (request.query == "validate") {
                        response.text = engine_->validate().render();
                    } else if (request.query == "sessions") {
                        const std::vector<std::string> descriptions = session_descriptions();
                        response.text = "sessions=" + std::to_string(descriptions.size()) +
                                        " active=" + std::to_string(active_session_count());
                        for (const std::string& text : descriptions) {
                            response.text += "\n  ";
                            response.text += text;
                        }
                    } else if (request.query == "publishers") {
                        const std::vector<PublisherState> publishers = engine_->publishers();
                        response.text = "publishers=" + std::to_string(publishers.size());
                        for (const PublisherState& state : publishers) {
                            response.text += "\n  ";
                            response.text += state.render();
                        }
                    } else if (request.query == "nodes") {
                        const std::vector<TopologyNode> nodes = engine_->all_nodes();
                        response.text = "nodes=" + std::to_string(nodes.size());
                        for (const TopologyNode& node : nodes) {
                            response.text += "\n  ";
                            response.text += render_node(node);
                        }
                    } else if (request.query == "edges") {
                        const std::vector<TopologyEdge> edges = engine_->all_edges();
                        response.text = "edges=" + std::to_string(edges.size());
                        for (const TopologyEdge& edge : edges) {
                            response.text += "\n  ";
                            response.text += render_edge(edge);
                            response.text += " publisher=";
                            response.text += edge.provenance.publisher.to_string();
                            response.text += " worker_boot=";
                            response.text += edge.provenance.worker_boot.to_string();
                        }
                    } else if (request.query == "edge") {
                        Explanation explanation;
                        const auto parsed = TopologyEdgeId::parse(request.argument);
                        if (!parsed.has_value()) {
                            response.status = make_status(Outcome::MalformedRequest,
                                                          "coordinator.query_argument");
                        } else {
                            response.status = engine_->explain_relationship(*parsed, explanation);
                            response.text = explanation.render();
                        }
                    } else if (request.query == "node") {
                        Explanation explanation;
                        const auto parsed = TopologyNodeId::parse(request.argument);
                        if (!parsed.has_value()) {
                            response.status = make_status(Outcome::MalformedRequest,
                                                          "coordinator.query_argument");
                        } else {
                            response.status = engine_->explain_node(*parsed, explanation);
                            response.text = explanation.render();
                        }
                    } else if (request.query == "render") {
                        response.text = engine_->render_canonical();
                    } else {
                        response.status = make_status(Outcome::NotFound, "coordinator.unknown_query");
                        response.status.with("query", request.query);
                    }
                    RecordWriter response_writer(4096);
                    encode_message(response, response_writer);
                    std::string response_error;
                    if (!transport.send(MessageType::QueryResult, 0, response_writer.data(), response_error)) {
                        session->end_reason = "query_result_send_failed";
                        break;
                    }
                    continue;
                }

                if (type == MessageType::ShutdownRequest) {
                    ShutdownRequestMessage request;
                    if (!decode_message(reader, request)) {
                        session->end_reason = "shutdown_malformed";
                        break;
                    }
                    GoodbyeMessage goodbye;
                    goodbye.reason = "coordinator_shutting_down";
                    RecordWriter goodbye_writer(64);
                    encode_message(goodbye, goodbye_writer);
                    std::string goodbye_error;
                    static_cast<void>(
                        transport.send(MessageType::Goodbye, 0, goodbye_writer.data(), goodbye_error));
                    // The stop flag is raised here, but the join happens on the owning thread:
                    // a session thread never tears down the coordinator it runs inside.
                    stop_.store(true);
                    listener_->transport.request_stop();
                    session->end_reason = "shutdown_requested";
                    break;
                }

                if (type == MessageType::Goodbye) {
                    GoodbyeMessage goodbye;
                    static_cast<void>(decode_message(reader, goodbye));
                    session->end_reason = "client_goodbye";
                    break;
                }

                static_cast<void>(send_error(transport, Outcome::ProtocolViolation, "coordinator.unexpected_message",
                           to_string(type)));
                session->end_reason = "unexpected_message";
                break;
            }
        }
    }

    // Worker incarnation fencing through the real control path: whichever way the session
    // ended, this worker boot can never mutate topology again.
    if (session->registered && !session->observer) {
        const std::size_t demoted = engine_->fence_boot(session->worker_boot, "session_terminated", true);
        static_cast<void>(demoted);
        const Status fenced = engine_->fence_publisher(session->publisher, session->worker_boot,
                                                       "session_terminated");
        if (outcome_is_success(fenced.outcome()) || fenced.outcome() == Outcome::StaleWorkerBoot) {
            session->fenced = true;
            fenced_workers_.fetch_add(1);
        }
    }

    transport.request_stop();
    transport.close();
    session->finished.store(true);
}

void TopologyCoordinator::stop() {
    if (stop_.exchange(true)) {
        running_.store(false);
        return;
    }
    running_.store(false);
    if (listener_ != nullptr) {
        listener_->stop.store(true);
        listener_->transport.request_stop();
    }

    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions = sessions_;
    }
    for (const std::shared_ptr<Session>& session : sessions) {
        session->stop.store(true);
        session->transport.request_stop();
    }

    if (listener_ != nullptr && listener_->thread.joinable()) {
        listener_->thread.join();
    }
    const std::thread::id self = std::this_thread::get_id();
    for (const std::shared_ptr<Session>& session : sessions) {
        if (!session->thread.joinable()) {
            continue;
        }
        if (session->thread.get_id() == self) {
            // Defensive: a session thread must never join itself. This path is unreachable in
            // the shipped design because stop() is only called from the owning thread, but the
            // guard makes a self-join impossible rather than merely unlikely.
            continue;
        }
        session->thread.join();
    }
    if (listener_ != nullptr) {
        listener_->transport.close();
    }
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }
}

}  // namespace fabric_topology::runtime
