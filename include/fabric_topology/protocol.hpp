// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_PROTOCOL_HPP
#define FABRIC_TOPOLOGY_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_topology/authority.hpp"
#include "fabric_topology/codec.hpp"
#include "fabric_topology/limits.hpp"
#include "fabric_topology/publication.hpp"
#include "fabric_topology/result.hpp"

namespace fabric_topology {

/// Wire protocol identifier. Frames carry it so a mismatched peer is rejected immediately.
inline constexpr char kProtocolMagic[4] = {'F', 'T', 'W', 'P'};
inline constexpr std::uint16_t kProtocolVersion = 1;
/// Encoded header layout, all little-endian:
///   magic_blob(4 length prefix + 4 magic) | version(2) | type(2) | flags(4) |
///   sequence(8) | payload_bytes(4) | payload_crc32(4)
inline constexpr std::size_t kFrameHeaderBytes = 4 + 4 + 2 + 2 + 4 + 8 + 4 + 4;

enum class MessageType : std::uint16_t {
    Invalid = 0,
    Hello = 1,
    Welcome = 2,
    Goodbye = 3,
    Publish = 4,
    PublishResult = 5,
    Heartbeat = 6,
    Fence = 7,
    Query = 8,
    QueryResult = 9,
    ShutdownRequest = 10,
    ErrorReport = 11,
};

inline constexpr std::uint16_t kMessageTypeCount = 12;

[[nodiscard]] const char* to_string(MessageType value) noexcept;
[[nodiscard]] std::optional<MessageType> message_type_from_code(std::uint16_t code) noexcept;

enum class FrameStatus : std::uint8_t {
    Complete = 0,
    Incomplete = 1,
    Malformed = 2,
};

struct FrameHeader {
    std::uint16_t version = kProtocolVersion;
    MessageType type = MessageType::Invalid;
    std::uint32_t flags = 0;
    std::uint64_t sequence = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t payload_crc32 = 0;
};

/// Encode one frame. Payload must not exceed limits.max_frame_payload_bytes.
[[nodiscard]] std::string encode_frame(const FrameHeader& header, std::string_view payload);

/// Decode one frame from a stream buffer. On FrameStatus::Complete, consumed receives the
/// total frame bytes. On Incomplete, consumed is the number of bytes that can be discarded
/// (zero). On Malformed, the caller must close the session.
[[nodiscard]] FrameStatus decode_frame(std::string_view buffer, const Limits& limits, FrameHeader& header,
                                       std::string& payload, std::size_t& consumed, std::string& error);

// ---------------------------------------------------------------------------
// Messages. Every message is encoded with RecordWriter and decoded with the bounded
// RecordReader; raw C++ memory layouts are never transmitted.
// ---------------------------------------------------------------------------

struct HelloMessage {
    PublisherId publisher;
    WorkerBootId worker_boot;
    CoordinatorEpoch coordinator_epoch;
    std::string process_label;
    bool observer = false;
    std::vector<ScopeGrant> requested_grants;
    std::string token;
    PublicationType declared_evidence_type = PublicationType::Unsupported;

    friend bool operator==(const HelloMessage&, const HelloMessage&) = default;
};

struct WelcomeMessage {
    bool accepted = false;
    CoordinatorEpoch coordinator_epoch;
    TopologyGeneration topology_generation;
    std::vector<ScopeGrant> granted;
    std::string session_id;
    Explanation reason;

    friend bool operator==(const WelcomeMessage&, const WelcomeMessage&) = default;
};

struct PublishMessage {
    AuthorityContext authority;
    Publication publication;

    friend bool operator==(const PublishMessage&, const PublishMessage&) = default;
};

struct PublishResultMessage {
    PublicationResult result;

    friend bool operator==(const PublishResultMessage&, const PublishResultMessage&) = default;
};

struct HeartbeatMessage {
    CoordinatorEpoch coordinator_epoch;
    WorkerBootId worker_boot;
    std::uint64_t counter = 0;

    friend bool operator==(const HeartbeatMessage&, const HeartbeatMessage&) = default;
};

struct FenceMessage {
    WorkerBootId worker_boot;
    std::string reason;

    friend bool operator==(const FenceMessage&, const FenceMessage&) = default;
};

struct QueryMessage {
    std::string query;
    std::string argument;

    friend bool operator==(const QueryMessage&, const QueryMessage&) = default;
};

struct QueryResultMessage {
    Explanation status;
    std::string text;

    friend bool operator==(const QueryResultMessage&, const QueryResultMessage&) = default;
};

struct ShutdownRequestMessage {
    std::string reason;
    bool graceful = true;

    friend bool operator==(const ShutdownRequestMessage&, const ShutdownRequestMessage&) = default;
};

struct ErrorReportMessage {
    Explanation error;

    friend bool operator==(const ErrorReportMessage&, const ErrorReportMessage&) = default;
};

struct GoodbyeMessage {
    std::string reason;

    friend bool operator==(const GoodbyeMessage&, const GoodbyeMessage&) = default;
};

/// Message codecs. decode returns false and records a stable message on the reader on any
/// malformed, truncated, oversized or unknown-enum input.
void encode_message(const HelloMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, HelloMessage& message);

void encode_message(const WelcomeMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, WelcomeMessage& message);

void encode_message(const PublishMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, PublishMessage& message);

void encode_message(const PublishResultMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, PublishResultMessage& message);

void encode_message(const HeartbeatMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, HeartbeatMessage& message);

void encode_message(const FenceMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, FenceMessage& message);

void encode_message(const QueryMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, QueryMessage& message);

void encode_message(const QueryResultMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, QueryResultMessage& message);

void encode_message(const ShutdownRequestMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, ShutdownRequestMessage& message);

void encode_message(const ErrorReportMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, ErrorReportMessage& message);

void encode_message(const GoodbyeMessage& message, RecordWriter& writer);
[[nodiscard]] bool decode_message(RecordReader& reader, GoodbyeMessage& message);

// ---------------------------------------------------------------------------
// Shared record codecs used by both messages and persistence.
// ---------------------------------------------------------------------------
void encode_publication(const Publication& publication, RecordWriter& writer);
[[nodiscard]] bool decode_publication(RecordReader& reader, Publication& publication);

void encode_authority(const AuthorityContext& authority, RecordWriter& writer);
[[nodiscard]] bool decode_authority(RecordReader& reader, AuthorityContext& authority);

void encode_explanation(const Explanation& explanation, RecordWriter& writer);
[[nodiscard]] bool decode_explanation(RecordReader& reader, Explanation& explanation);

void encode_scope_grants(const std::vector<ScopeGrant>& grants, RecordWriter& writer);
[[nodiscard]] bool decode_scope_grants(RecordReader& reader, std::vector<ScopeGrant>& grants);

void encode_metadata(const Metadata& metadata, RecordWriter& writer);
[[nodiscard]] bool decode_metadata(RecordReader& reader, Metadata& metadata, const Limits& limits);

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_PROTOCOL_HPP
