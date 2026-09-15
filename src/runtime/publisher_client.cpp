// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/runtime/publisher_client.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "fabric_topology/codec.hpp"
#include "fabric_topology/digest.hpp"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fabric_topology::runtime {

namespace {

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] std::string hex64(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        out[15 - i] = kDigits[(value >> (4U * i)) & 0x0FU];
    }
    return out;
}

[[nodiscard]] std::uint64_t process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace

WorkerBootId make_worker_boot_id(std::string_view label) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    std::string seed = "boot|";
    seed.append(label.data(), label.size());
    seed += '|';
    seed += std::to_string(process_id());
    seed += '|';
    seed += std::to_string(ticks);
    const std::uint64_t mixed = fnv1a64(seed);
    std::string value = "boot_";
    value += hex64(mixed);
    value += hex64(ticks ^ (process_id() << 17U));
    return WorkerBootId::from_trusted(std::move(value));
}

PublisherClient::PublisherClient(PublisherClientOptions options)
    : options_(std::move(options)), transport_(options_.limits) {}

PublisherClient::~PublisherClient() { close(); }

bool PublisherClient::send_frame(MessageType type, std::string_view payload, std::string& error) {
    return transport_.send(type, 0, payload, error);
}

ReceiveOutcome PublisherClient::await(MessageType expected, ReceivedFrame& frame, std::string& error) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.response_deadline_ms);
    for (;;) {
        ReceivedFrame received;
        const ReceiveOutcome outcome = transport_.receive(received, error);
        if (outcome == ReceiveOutcome::Frame) {
            if (received.header.type == expected) {
                frame = std::move(received);
                return ReceiveOutcome::Frame;
            }
            if (received.header.type == MessageType::ErrorReport) {
                RecordReader reader(received.payload, options_.limits);
                ErrorReportMessage report;
                if (decode_message(reader, report)) {
                    error = report.error.one_line();
                } else {
                    error = "protocol.error_report";
                }
                return ReceiveOutcome::Error;
            }
            error = std::string("protocol.unexpected_message:") +
                    to_string(received.header.type);
            return ReceiveOutcome::Error;
        }
        if (outcome == ReceiveOutcome::Idle) {
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "protocol.response_deadline_exceeded";
                return ReceiveOutcome::Error;
            }
            continue;
        }
        if (outcome == ReceiveOutcome::Closed) {
            error = "protocol.connection_closed";
            return ReceiveOutcome::Closed;
        }
        return ReceiveOutcome::Error;
    }
}

Status PublisherClient::connect_and_register(WelcomeMessage& welcome) {
    std::string error;
    if (!transport_.connect_to(options_.host, options_.port, error)) {
        Status status = make_status(Outcome::PersistenceIoFailure, "publisher.connect_failed");
        status.with("detail", error);
        status.with("host", options_.host);
        status.with("port", std::to_string(options_.port));
        return status;
    }

    HelloMessage hello;
    hello.publisher = options_.publisher;
    hello.worker_boot = options_.worker_boot;
    hello.coordinator_epoch = options_.claimed_epoch;
    hello.process_label = options_.process_label;
    hello.observer = options_.observer;
    hello.token = options_.token;
    hello.declared_evidence_type = options_.declared_evidence_type;
    hello.requested_grants = options_.requested_grants;

    RecordWriter writer(256);
    encode_message(hello, writer);
    if (!send_frame(MessageType::Hello, writer.data(), error)) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.hello_send_failed");
        status.with("detail", error);
        return status;
    }

    ReceivedFrame frame;
    if (await(MessageType::Welcome, frame, error) != ReceiveOutcome::Frame) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.welcome_failed");
        status.with("detail", error);
        return status;
    }
    RecordReader reader(frame.payload, options_.limits);
    if (!decode_message(reader, welcome)) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.welcome_malformed");
        status.with("detail", reader.error());
        return status;
    }
    welcome_ = welcome;
    if (!welcome.accepted) {
        Status status = make_status(Outcome::StaleAuthority, "publisher.registration_rejected");
        status.with("reason", welcome.reason.one_line());
        return status;
    }
    registered_ = true;
    Status status = make_status(Outcome::Committed, "publisher.registered");
    status.with("coordinator_epoch", welcome.coordinator_epoch.to_string());
    status.with("topology_generation", welcome.topology_generation.to_string());
    status.with("session", welcome.session_id);
    return status;
}

Status PublisherClient::publish(const Publication& publication, PublishResultMessage& result) {
    if (!registered_) {
        return make_status(Outcome::StaleAuthority, "publisher.not_registered");
    }
    AuthorityContext authority;
    authority.coordinator_epoch = welcome_.coordinator_epoch;
    authority.publisher = options_.publisher;
    authority.worker_boot = options_.worker_boot;
    authority.evidence_generation = publication.evidence_generation;
    authority.publication = publication.id;

    PublishMessage message;
    message.authority = authority;
    message.publication = publication;

    RecordWriter writer(4096);
    encode_message(message, writer);
    std::string error;
    if (!send_frame(MessageType::Publish, writer.data(), error)) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.publish_send_failed");
        status.with("detail", error);
        return status;
    }
    ReceivedFrame frame;
    if (await(MessageType::PublishResult, frame, error) != ReceiveOutcome::Frame) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.publish_result_failed");
        status.with("detail", error);
        return status;
    }
    RecordReader reader(frame.payload, options_.limits);
    if (!decode_message(reader, result)) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.publish_result_malformed");
        status.with("detail", reader.error());
        return status;
    }
    return result.result.status;
}

Status PublisherClient::heartbeat(HeartbeatMessage& acknowledgement) {
    if (!registered_) {
        return make_status(Outcome::StaleAuthority, "publisher.not_registered");
    }
    HeartbeatMessage message;
    message.coordinator_epoch = welcome_.coordinator_epoch;
    message.worker_boot = options_.worker_boot;
    message.counter = ++heartbeat_counter_;

    RecordWriter writer(64);
    encode_message(message, writer);
    std::string error;
    if (!send_frame(MessageType::Heartbeat, writer.data(), error)) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.heartbeat_send_failed");
        status.with("detail", error);
        return status;
    }
    ReceivedFrame frame;
    if (await(MessageType::Heartbeat, frame, error) != ReceiveOutcome::Frame) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.heartbeat_failed");
        status.with("detail", error);
        return status;
    }
    RecordReader reader(frame.payload, options_.limits);
    if (!decode_message(reader, acknowledgement)) {
        return make_status(Outcome::ProtocolViolation, "publisher.heartbeat_malformed");
    }
    return make_status(Outcome::Committed, "publisher.heartbeat_ok");
}

Status PublisherClient::query(const std::string& query, const std::string& argument,
                              QueryResultMessage& result) {
    QueryMessage message;
    message.query = query;
    message.argument = argument;
    RecordWriter writer(128);
    encode_message(message, writer);
    std::string error;
    if (!send_frame(MessageType::Query, writer.data(), error)) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.query_send_failed");
        status.with("detail", error);
        return status;
    }
    ReceivedFrame frame;
    if (await(MessageType::QueryResult, frame, error) != ReceiveOutcome::Frame) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.query_failed");
        status.with("detail", error);
        return status;
    }
    RecordReader reader(frame.payload, options_.limits);
    if (!decode_message(reader, result)) {
        return make_status(Outcome::ProtocolViolation, "publisher.query_result_malformed");
    }
    return result.status;
}

Status PublisherClient::request_shutdown(const std::string& reason, bool graceful) {
    ShutdownRequestMessage message;
    message.reason = reason;
    message.graceful = graceful;
    RecordWriter writer(64);
    encode_message(message, writer);
    std::string error;
    if (!send_frame(MessageType::ShutdownRequest, writer.data(), error)) {
        Status status = make_status(Outcome::ProtocolViolation, "publisher.shutdown_send_failed");
        status.with("detail", error);
        return status;
    }
    return make_status(Outcome::Committed, "publisher.shutdown_requested");
}

void PublisherClient::close() {
    if (transport_.valid()) {
        GoodbyeMessage goodbye;
        goodbye.reason = "client_closing";
        RecordWriter writer(64);
        encode_message(goodbye, writer);
        std::string error;
        static_cast<void>(transport_.send(MessageType::Goodbye, 0, writer.data(), error));
    }
    registered_ = false;
    transport_.close();
}

}  // namespace fabric_topology::runtime
