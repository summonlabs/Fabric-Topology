// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Framed network protocol: bounded frame codec plus deterministic message encoding. Raw C++
// memory layouts are never transmitted; every field is written explicitly.

#include "fabric_topology/protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fabric_topology/digest.hpp"
#include "fabric_topology/metadata.hpp"

namespace fabric_topology {

namespace {

[[nodiscard]] Explanation protocol_error(const char* code, std::string detail) {
    Explanation explanation;
    explanation.set_outcome(Outcome::ProtocolViolation).set_code(code);
    if (!detail.empty()) {
        explanation.with("detail", std::move(detail));
    }
    return explanation;
}

void write_optional_id(RecordWriter& writer, bool present, const std::string& value) {
    writer.boolean(present);
    writer.text(present ? std::string_view(value) : std::string_view{});
}

[[nodiscard]] std::optional<std::string> read_optional_id(RecordReader& reader) {
    const bool present = reader.boolean();
    std::string value = reader.text(kMaxIdentifierBytes);
    if (!reader.ok() || !present) {
        return std::nullopt;
    }
    return value;
}

void encode_metadata_items(const Metadata& metadata, RecordWriter& writer) {
    writer.u32(static_cast<std::uint32_t>(metadata.size()));
    for (const Metadata::Item& item : metadata.items()) {
        writer.text(item.first);
        writer.text(item.second);
    }
}

[[nodiscard]] bool decode_metadata_items(RecordReader& reader, Metadata& metadata) {
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > Limits{}.max_metadata_entries) {
        return reader.fail("metadata.count");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::string key = reader.text(Limits{}.max_metadata_key_bytes);
        const std::string value = reader.text(Limits{}.max_metadata_value_bytes);
        if (!reader.ok()) {
            return false;
        }
        const Status status = metadata.set(key, value, Limits{});
        if (status.outcome() != Outcome::Ok) {
            return reader.fail("metadata.entry");
        }
    }
    return true;
}

void encode_diff_entry(const DiffEntry& entry, RecordWriter& writer) {
    writer.u8(static_cast<std::uint8_t>(entry.kind));
    writer.text(entry.key);
    writer.text(entry.node.value());
    writer.text(entry.edge.value());
    writer.text(entry.before);
    writer.text(entry.after);
}

[[nodiscard]] bool decode_diff_entry(RecordReader& reader, DiffEntry& entry) {
    const std::uint8_t kind = reader.u8();
    if (!reader.ok() || kind >= kDiffKindCount) {
        return reader.fail("diff.kind");
    }
    entry.kind = static_cast<DiffKind>(kind);
    entry.key = reader.text(Limits{}.max_string_bytes);
    entry.node = TopologyNodeId::from_trusted(reader.text(kMaxIdentifierBytes));
    entry.edge = TopologyEdgeId::from_trusted(reader.text(kMaxIdentifierBytes));
    entry.before = reader.text(Limits{}.max_string_bytes);
    entry.after = reader.text(Limits{}.max_string_bytes);
    return reader.ok();
}

}  // namespace

const char* to_string(MessageType value) noexcept {
    switch (value) {
        case MessageType::Invalid: return "INVALID";
        case MessageType::Hello: return "HELLO";
        case MessageType::Welcome: return "WELCOME";
        case MessageType::Goodbye: return "GOODBYE";
        case MessageType::Publish: return "PUBLISH";
        case MessageType::PublishResult: return "PUBLISH_RESULT";
        case MessageType::Heartbeat: return "HEARTBEAT";
        case MessageType::Fence: return "FENCE";
        case MessageType::Query: return "QUERY";
        case MessageType::QueryResult: return "QUERY_RESULT";
        case MessageType::ShutdownRequest: return "SHUTDOWN_REQUEST";
        case MessageType::ErrorReport: return "ERROR_REPORT";
    }
    return "INVALID";
}

std::optional<MessageType> message_type_from_code(std::uint16_t code) noexcept {
    if (code == 0 || code >= kMessageTypeCount) {
        return std::nullopt;
    }
    return static_cast<MessageType>(code);
}

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------

std::string encode_frame(const FrameHeader& header, std::string_view payload) {
    RecordWriter writer(kFrameHeaderBytes + payload.size());
    writer.blob(std::string_view(kProtocolMagic, sizeof(kProtocolMagic)));
    writer.u16(header.version);
    writer.u16(static_cast<std::uint16_t>(header.type));
    writer.u32(header.flags);
    writer.u64(header.sequence);
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u32(crc32(payload));
    std::string frame = writer.take();
    frame.append(payload.data(), payload.size());
    return frame;
}

FrameStatus decode_frame(std::string_view buffer, const Limits& limits, FrameHeader& header,
                         std::string& payload, std::size_t& consumed, std::string& error) {
    consumed = 0;
    payload.clear();
    error.clear();
    if (buffer.size() < kFrameHeaderBytes) {
        return FrameStatus::Incomplete;
    }
    RecordReader reader(buffer.substr(0, kFrameHeaderBytes), limits);
    const std::string magic = reader.blob(sizeof(kProtocolMagic));
    if (!reader.ok() || magic != std::string(kProtocolMagic, sizeof(kProtocolMagic))) {
        error = "frame.bad_magic";
        return FrameStatus::Malformed;
    }
    header.version = reader.u16();
    const std::uint16_t type_code = reader.u16();
    header.flags = reader.u32();
    header.sequence = reader.u64();
    header.payload_bytes = reader.u32();
    header.payload_crc32 = reader.u32();
    if (!reader.ok()) {
        error = "frame.header";
        return FrameStatus::Malformed;
    }
    if (header.version != kProtocolVersion) {
        error = "frame.version_unsupported";
        return FrameStatus::Malformed;
    }
    const std::optional<MessageType> type = message_type_from_code(type_code);
    if (!type.has_value()) {
        error = "frame.unknown_message_type";
        return FrameStatus::Malformed;
    }
    header.type = *type;
    if (header.payload_bytes > limits.max_frame_payload_bytes) {
        error = "frame.payload_too_large";
        return FrameStatus::Malformed;
    }
    const std::uint64_t total = static_cast<std::uint64_t>(kFrameHeaderBytes) + header.payload_bytes;
    if (static_cast<std::uint64_t>(buffer.size()) < total) {
        return FrameStatus::Incomplete;
    }
    const std::string_view body = buffer.substr(kFrameHeaderBytes, header.payload_bytes);
    if (crc32(body) != header.payload_crc32) {
        error = "frame.crc_mismatch";
        return FrameStatus::Malformed;
    }
    payload.assign(body.data(), body.size());
    consumed = static_cast<std::size_t>(total);
    return FrameStatus::Complete;
}

// ---------------------------------------------------------------------------
// Shared record codecs
// ---------------------------------------------------------------------------

void encode_metadata(const Metadata& metadata, RecordWriter& writer) {
    encode_metadata_items(metadata, writer);
}

bool decode_metadata(RecordReader& reader, Metadata& metadata, const Limits& limits) {
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > limits.max_metadata_entries) {
        return reader.fail("metadata.count");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::string key = reader.text(limits.max_metadata_key_bytes);
        const std::string value = reader.text(limits.max_metadata_value_bytes);
        if (!reader.ok()) {
            return false;
        }
        const Status status = metadata.set(key, value, limits);
        if (status.outcome() != Outcome::Ok) {
            return reader.fail("metadata.entry");
        }
    }
    return true;
}

void encode_explanation(const Explanation& explanation, RecordWriter& writer) {
    writer.u16(static_cast<std::uint16_t>(explanation.outcome()));
    writer.text(explanation.code());
    writer.u32(static_cast<std::uint32_t>(explanation.fields().size()));
    for (const Explanation::Field& field : explanation.fields()) {
        writer.text(field.key);
        writer.text(field.value);
    }
}

bool decode_explanation(RecordReader& reader, Explanation& explanation) {
    const std::uint16_t outcome = reader.u16();
    const std::string code = reader.text(128);
    if (!reader.ok()) {
        return false;
    }
    const std::optional<Outcome> parsed = outcome_from_code(outcome);
    if (!parsed.has_value()) {
        return reader.fail("explanation.outcome");
    }
    explanation = Explanation(*parsed, code);
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > Explanation::kMaxFields) {
        return reader.fail("explanation.field_count");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::string key = reader.text(64);
        const std::string value = reader.text(Explanation::kMaxFieldValueBytes);
        if (!reader.ok()) {
            return false;
        }
        explanation.with(key, value);
    }
    return reader.ok();
}

void encode_scope_grants(const std::vector<ScopeGrant>& grants, RecordWriter& writer) {
    writer.u32(static_cast<std::uint32_t>(grants.size()));
    for (const ScopeGrant& grant : grants) {
        writer.text(grant.domain.value());
        writer.u8(static_cast<std::uint8_t>(grant.mode));
    }
}

bool decode_scope_grants(RecordReader& reader, std::vector<ScopeGrant>& grants) {
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > 4096U) {
        return reader.fail("grants.count");
    }
    grants.clear();
    grants.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ScopeGrant grant;
        grant.domain = TopologyDomainId::from_trusted(reader.text(kMaxIdentifierBytes));
        const std::uint8_t mode = reader.u8();
        if (!reader.ok()) {
            return false;
        }
        if (mode >= kGrantModeCount || grant.domain.empty()) {
            return reader.fail("grants.entry");
        }
        grant.mode = static_cast<GrantMode>(mode);
        grants.push_back(std::move(grant));
    }
    return reader.ok();
}

void encode_authority(const AuthorityContext& authority, RecordWriter& writer) {
    writer.u64(authority.coordinator_epoch.value());
    writer.text(authority.publisher.value());
    writer.text(authority.worker_boot.value());
    writer.u64(authority.evidence_generation.value());
    writer.text(authority.attempt.value());
    writer.text(authority.publication.value());
}

bool decode_authority(RecordReader& reader, AuthorityContext& authority) {
    authority.coordinator_epoch = CoordinatorEpoch{reader.u64()};
    authority.publisher = PublisherId::from_trusted(reader.text(kMaxIdentifierBytes));
    authority.worker_boot = WorkerBootId::from_trusted(reader.text(kMaxIdentifierBytes));
    authority.evidence_generation = EvidenceGeneration{reader.u64()};
    authority.attempt = MutationAttemptId::from_trusted(reader.text(kMaxIdentifierBytes));
    authority.publication = PublicationId::from_trusted(reader.text(kMaxIdentifierBytes));
    return reader.ok();
}

void encode_publication(const Publication& publication, RecordWriter& writer) {
    writer.text(publication.id.value());
    writer.text(publication.domain.value());
    writer.u8(static_cast<std::uint8_t>(publication.scope_kind));
    writer.u8(static_cast<std::uint8_t>(publication.mode));
    writer.u8(static_cast<std::uint8_t>(publication.type));
    writer.u8(static_cast<std::uint8_t>(publication.source));
    writer.u64(publication.expected_generation.value());
    writer.u64(publication.evidence_generation.value());
    writer.text(publication.note);

    writer.u32(static_cast<std::uint32_t>(publication.declared_absent_relationships.size()));
    for (const RelationshipId& id : publication.declared_absent_relationships) {
        writer.text(id.value());
    }

    writer.u32(static_cast<std::uint32_t>(publication.nodes.size()));
    for (const ObservedNode& node : publication.nodes) {
        writer.text(node.node_id.value());
        writer.text(node.entity_id);
        writer.u8(static_cast<std::uint8_t>(node.node_class));
        writer.u8(static_cast<std::uint8_t>(node.tier));
        writer.text(node.domain.value());
        writer.u64(node.entity_generation.value());
        writer.boolean(node.present);
        encode_metadata_items(node.metadata, writer);
    }

    writer.u32(static_cast<std::uint32_t>(publication.edges.size()));
    for (const ObservedEdge& edge : publication.edges) {
        write_optional_id(writer, edge.edge_id.has_value(),
                          edge.edge_id.has_value() ? edge.edge_id->value() : std::string{});
        writer.text(edge.relationship_id.value());
        writer.u8(static_cast<std::uint8_t>(edge.relation));
        writer.text(edge.from.value());
        writer.text(edge.to.value());
        writer.boolean(edge.layer.has_value());
        writer.u8(edge.layer.has_value() ? static_cast<std::uint8_t>(*edge.layer) : 0U);
        writer.text(edge.domain.value());
        writer.u64(edge.evidence_generation.value());
        writer.text(edge.source_relationship_id);
        write_optional_id(writer, edge.supported_by.has_value(),
                          edge.supported_by.has_value() ? edge.supported_by->value() : std::string{});
        write_optional_id(writer, edge.attachment.has_value(),
                          edge.attachment.has_value() ? edge.attachment->value() : std::string{});
        writer.boolean(edge.exclusive_attachment);
        writer.boolean(edge.present);
        encode_metadata_items(edge.metadata, writer);
    }
}

bool decode_publication(RecordReader& reader, Publication& publication) {
    const Limits limits;
    publication.id = PublicationId::from_trusted(reader.text(kMaxIdentifierBytes));
    publication.domain = TopologyDomainId::from_trusted(reader.text(kMaxIdentifierBytes));
    const std::uint8_t scope_kind = reader.u8();
    const std::uint8_t mode = reader.u8();
    const std::uint8_t type = reader.u8();
    const std::uint8_t source = reader.u8();
    if (!reader.ok() || scope_kind >= kScopeKindCount || mode >= kPublicationModeCount ||
        type >= kPublicationTypeCount || source >= kDiscoverySourceCount) {
        return reader.fail("publication.enum");
    }
    publication.scope_kind = static_cast<ScopeKind>(scope_kind);
    publication.mode = static_cast<PublicationMode>(mode);
    publication.type = static_cast<PublicationType>(type);
    publication.source = static_cast<DiscoverySource>(source);
    publication.expected_generation = TopologyGeneration{reader.u64()};
    publication.evidence_generation = EvidenceGeneration{reader.u64()};
    publication.note = reader.text(limits.max_string_bytes);
    if (!reader.ok()) {
        return false;
    }

    const std::uint32_t absence_count = reader.u32();
    if (!reader.ok() || absence_count > limits.max_edges_per_publication) {
        return reader.fail("publication.absence_count");
    }
    for (std::uint32_t i = 0; i < absence_count; ++i) {
        publication.declared_absent_relationships.push_back(
            RelationshipId::from_trusted(reader.text(kMaxIdentifierBytes)));
    }
    if (!reader.ok()) {
        return false;
    }

    const std::uint32_t node_count = reader.u32();
    if (!reader.ok() || node_count > limits.max_nodes_per_publication) {
        return reader.fail("publication.node_count");
    }
    publication.nodes.reserve(node_count);
    for (std::uint32_t i = 0; i < node_count; ++i) {
        ObservedNode node;
        node.node_id = TopologyNodeId::from_trusted(reader.text(kMaxIdentifierBytes));
        node.entity_id = reader.text(limits.max_identifier_bytes);
        const std::uint8_t node_class = reader.u8();
        const std::uint8_t tier = reader.u8();
        if (!reader.ok() || node_class >= kNodeClassCount || tier >= kTopologyTierCount) {
            return reader.fail("publication.node_enum");
        }
        node.node_class = static_cast<NodeClass>(node_class);
        node.tier = static_cast<TopologyTier>(tier);
        node.domain = TopologyDomainId::from_trusted(reader.text(kMaxIdentifierBytes));
        node.entity_generation = EntityGeneration{reader.u64()};
        node.present = reader.boolean();
        if (!decode_metadata(reader, node.metadata, limits)) {
            return false;
        }
        publication.nodes.push_back(std::move(node));
    }

    const std::uint32_t edge_count = reader.u32();
    if (!reader.ok() || edge_count > limits.max_edges_per_publication) {
        return reader.fail("publication.edge_count");
    }
    publication.edges.reserve(edge_count);
    for (std::uint32_t i = 0; i < edge_count; ++i) {
        ObservedEdge edge;
        const std::optional<std::string> edge_id = read_optional_id(reader);
        if (edge_id.has_value()) {
            if (!detail::is_valid_identifier(*edge_id, kMaxIdentifierBytes)) {
                return reader.fail("publication.edge_id");
            }
            edge.edge_id = TopologyEdgeId::from_trusted(*edge_id);
        }
        edge.relationship_id = RelationshipId::from_trusted(reader.text(kMaxIdentifierBytes));
        const std::uint8_t relation = reader.u8();
        if (!reader.ok() || relation >= kRelationClassCount) {
            return reader.fail("publication.edge_relation");
        }
        edge.relation = static_cast<RelationClass>(relation);
        edge.from = TopologyNodeId::from_trusted(reader.text(kMaxIdentifierBytes));
        edge.to = TopologyNodeId::from_trusted(reader.text(kMaxIdentifierBytes));
        const bool has_layer = reader.boolean();
        const std::uint8_t layer = reader.u8();
        if (!reader.ok() || layer > static_cast<std::uint8_t>(TopologyLayer::Logical)) {
            return reader.fail("publication.edge_layer");
        }
        if (has_layer) {
            edge.layer = static_cast<TopologyLayer>(layer);
        }
        edge.domain = TopologyDomainId::from_trusted(reader.text(kMaxIdentifierBytes));
        edge.evidence_generation = EvidenceGeneration{reader.u64()};
        edge.source_relationship_id = reader.text(kMaxIdentifierBytes);
        const std::optional<std::string> supported_by = read_optional_id(reader);
        if (supported_by.has_value()) {
            if (!detail::is_valid_identifier(*supported_by, kMaxIdentifierBytes)) {
                return reader.fail("publication.edge_supported_by");
            }
            edge.supported_by = TopologyEdgeId::from_trusted(*supported_by);
        }
        const std::optional<std::string> attachment = read_optional_id(reader);
        if (attachment.has_value()) {
            if (!detail::is_valid_identifier(*attachment, kMaxIdentifierBytes)) {
                return reader.fail("publication.edge_attachment");
            }
            edge.attachment = AttachmentId::from_trusted(*attachment);
        }
        edge.exclusive_attachment = reader.boolean();
        edge.present = reader.boolean();
        if (!decode_metadata(reader, edge.metadata, limits)) {
            return false;
        }
        publication.edges.push_back(std::move(edge));
    }

    return reader.ok();
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

void encode_message(const HelloMessage& message, RecordWriter& writer) {
    writer.text(message.publisher.value());
    writer.text(message.worker_boot.value());
    writer.u64(message.coordinator_epoch.value());
    writer.text(message.process_label);
    writer.boolean(message.observer);
    writer.u8(static_cast<std::uint8_t>(message.declared_evidence_type));
    writer.text(message.token);
    encode_scope_grants(message.requested_grants, writer);
}

bool decode_message(RecordReader& reader, HelloMessage& message) {
    const Limits limits;
    message.publisher = PublisherId::from_trusted(reader.text(kMaxIdentifierBytes));
    message.worker_boot = WorkerBootId::from_trusted(reader.text(kMaxIdentifierBytes));
    message.coordinator_epoch = CoordinatorEpoch{reader.u64()};
    message.process_label = reader.text(limits.max_string_bytes);
    message.observer = reader.boolean();
    const std::uint8_t evidence_type = reader.u8();
    if (!reader.ok() || evidence_type >= kPublicationTypeCount) {
        return reader.fail("hello.evidence_type");
    }
    message.declared_evidence_type = static_cast<PublicationType>(evidence_type);
    message.token = reader.text(limits.max_string_bytes);
    if (!reader.ok()) {
        return false;
    }
    if (message.publisher.empty() || message.worker_boot.empty()) {
        return reader.fail("hello.identity");
    }
    return decode_scope_grants(reader, message.requested_grants);
}

void encode_message(const WelcomeMessage& message, RecordWriter& writer) {
    writer.boolean(message.accepted);
    writer.u64(message.coordinator_epoch.value());
    writer.u64(message.topology_generation.value());
    writer.text(message.session_id);
    encode_explanation(message.reason, writer);
    encode_scope_grants(message.granted, writer);
}

bool decode_message(RecordReader& reader, WelcomeMessage& message) {
    message.accepted = reader.boolean();
    message.coordinator_epoch = CoordinatorEpoch{reader.u64()};
    message.topology_generation = TopologyGeneration{reader.u64()};
    message.session_id = reader.text(kMaxIdentifierBytes);
    if (!reader.ok()) {
        return false;
    }
    if (!decode_explanation(reader, message.reason)) {
        return false;
    }
    return decode_scope_grants(reader, message.granted);
}

void encode_message(const PublishMessage& message, RecordWriter& writer) {
    encode_authority(message.authority, writer);
    encode_publication(message.publication, writer);
}

bool decode_message(RecordReader& reader, PublishMessage& message) {
    if (!decode_authority(reader, message.authority)) {
        return false;
    }
    return decode_publication(reader, message.publication);
}

void encode_message(const PublishResultMessage& message, RecordWriter& writer) {
    encode_explanation(message.result.status, writer);
    writer.u64(message.result.generation.value());
    writer.u64(message.result.nodes_added);
    writer.u64(message.result.nodes_updated);
    writer.u64(message.result.nodes_removed);
    writer.u64(message.result.nodes_unchanged);
    writer.u64(message.result.edges_added);
    writer.u64(message.result.edges_updated);
    writer.u64(message.result.edges_removed);
    writer.u64(message.result.edges_unchanged);
    writer.boolean(message.result.generation_advanced);
    writer.text(message.result.diff.digest);
    writer.u64(message.result.diff.from_generation.value());
    writer.u64(message.result.diff.to_generation.value());
    writer.u32(static_cast<std::uint32_t>(message.result.diff.entries.size()));
    for (const DiffEntry& entry : message.result.diff.entries) {
        encode_diff_entry(entry, writer);
    }
}

bool decode_message(RecordReader& reader, PublishResultMessage& message) {
    const Limits limits;
    if (!decode_explanation(reader, message.result.status)) {
        return false;
    }
    message.result.generation = TopologyGeneration{reader.u64()};
    message.result.nodes_added = static_cast<std::size_t>(reader.u64());
    message.result.nodes_updated = static_cast<std::size_t>(reader.u64());
    message.result.nodes_removed = static_cast<std::size_t>(reader.u64());
    message.result.nodes_unchanged = static_cast<std::size_t>(reader.u64());
    message.result.edges_added = static_cast<std::size_t>(reader.u64());
    message.result.edges_updated = static_cast<std::size_t>(reader.u64());
    message.result.edges_removed = static_cast<std::size_t>(reader.u64());
    message.result.edges_unchanged = static_cast<std::size_t>(reader.u64());
    message.result.generation_advanced = reader.boolean();
    message.result.diff.digest = reader.text(128);
    message.result.diff.from_generation = TopologyGeneration{reader.u64()};
    message.result.diff.to_generation = TopologyGeneration{reader.u64()};
    const std::uint32_t entries = reader.u32();
    if (!reader.ok() || entries > limits.max_publication_diff_entries) {
        return reader.fail("publish_result.diff_entries");
    }
    message.result.diff.entries.reserve(entries);
    for (std::uint32_t i = 0; i < entries; ++i) {
        DiffEntry entry;
        if (!decode_diff_entry(reader, entry)) {
            return false;
        }
        message.result.diff.entries.push_back(std::move(entry));
    }
    return reader.ok();
}

void encode_message(const HeartbeatMessage& message, RecordWriter& writer) {
    writer.u64(message.coordinator_epoch.value());
    writer.text(message.worker_boot.value());
    writer.u64(message.counter);
}

bool decode_message(RecordReader& reader, HeartbeatMessage& message) {
    message.coordinator_epoch = CoordinatorEpoch{reader.u64()};
    message.worker_boot = WorkerBootId::from_trusted(reader.text(kMaxIdentifierBytes));
    message.counter = reader.u64();
    return reader.ok();
}

void encode_message(const FenceMessage& message, RecordWriter& writer) {
    writer.text(message.worker_boot.value());
    writer.text(message.reason);
}

bool decode_message(RecordReader& reader, FenceMessage& message) {
    message.worker_boot = WorkerBootId::from_trusted(reader.text(kMaxIdentifierBytes));
    message.reason = reader.text(Limits{}.max_string_bytes);
    return reader.ok();
}

void encode_message(const QueryMessage& message, RecordWriter& writer) {
    writer.text(message.query);
    writer.text(message.argument);
}

bool decode_message(RecordReader& reader, QueryMessage& message) {
    message.query = reader.text(64);
    message.argument = reader.text(Limits{}.max_identifier_bytes);
    return reader.ok();
}

void encode_message(const QueryResultMessage& message, RecordWriter& writer) {
    encode_explanation(message.status, writer);
    writer.text(message.text);
}

bool decode_message(RecordReader& reader, QueryResultMessage& message) {
    if (!decode_explanation(reader, message.status)) {
        return false;
    }
    message.text = reader.text(Limits{}.max_string_bytes * 4U);
    return reader.ok();
}

void encode_message(const ShutdownRequestMessage& message, RecordWriter& writer) {
    writer.text(message.reason);
    writer.boolean(message.graceful);
}

bool decode_message(RecordReader& reader, ShutdownRequestMessage& message) {
    message.reason = reader.text(Limits{}.max_string_bytes);
    message.graceful = reader.boolean();
    return reader.ok();
}

void encode_message(const ErrorReportMessage& message, RecordWriter& writer) {
    encode_explanation(message.error, writer);
}

bool decode_message(RecordReader& reader, ErrorReportMessage& message) {
    return decode_explanation(reader, message.error);
}

void encode_message(const GoodbyeMessage& message, RecordWriter& writer) {
    writer.text(message.reason);
}

bool decode_message(RecordReader& reader, GoodbyeMessage& message) {
    message.reason = reader.text(Limits{}.max_string_bytes);
    return reader.ok();
}

}  // namespace fabric_topology
