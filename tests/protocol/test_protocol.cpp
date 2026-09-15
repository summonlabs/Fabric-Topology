// Fabric Topology - framed protocol codec tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include <cstdint>
#include <string>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"

using namespace fabric_topology;

namespace {

std::string frame_of(MessageType type, const std::string& payload, std::uint64_t sequence = 1) {
    FrameHeader header;
    header.type = type;
    header.sequence = sequence;
    header.payload_bytes = static_cast<std::uint32_t>(payload.size());
    header.payload_crc32 = crc32(payload);
    return encode_frame(header, payload);
}

/// Encode a message into a named buffer. A RecordReader is a view, so the encoded bytes must
/// outlive it.
template <class T>
std::string encode(const T& message) {
    RecordWriter writer(256);
    encode_message(message, writer);
    return writer.take();
}

template <class T>
void check_round_trip(const T& message, const char* what) {
    const std::string encoded = encode(message);
    RecordReader reader(encoded, Limits{});
    T decoded{};
    if (!decode_message(reader, decoded)) {
        FT_FAIL(std::string("decode failed for ") + what + ": " + reader.error());
    }
    if (!(decoded == message)) {
        FT_FAIL(std::string("round trip mismatch for ") + what);
    }
    if (!reader.at_end()) {
        FT_FAIL(std::string("trailing bytes after ") + what);
    }
}

}  // namespace

FT_TEST(protocol, frame_round_trip) {
    const std::string payload = "hello world payload";
    const std::string frame = frame_of(MessageType::Query, payload, 42);
    FT_CHECK_EQ(frame.size(), kFrameHeaderBytes + payload.size());

    Limits limits;
    FrameHeader header;
    std::string decoded;
    std::size_t consumed = 0;
    std::string error;
    FT_CHECK_EQ(decode_frame(frame, limits, header, decoded, consumed, error), FrameStatus::Complete);
    FT_CHECK_EQ(consumed, frame.size());
    FT_CHECK_EQ(header.type, MessageType::Query);
    FT_CHECK_EQ(header.sequence, std::uint64_t{42});
    FT_CHECK_EQ(decoded, payload);
}

FT_TEST(protocol, partial_frame_is_incomplete_not_malformed) {
    const std::string frame = frame_of(MessageType::Heartbeat, std::string(500, 'x'));
    Limits limits;
    for (std::size_t length = 0; length < frame.size(); ++length) {
        FrameHeader header;
        std::string payload;
        std::size_t consumed = 0;
        std::string error;
        const FrameStatus status =
            decode_frame(frame.substr(0, length), limits, header, payload, consumed, error);
        FT_CHECK_EQ(status, FrameStatus::Incomplete);
    }
}

FT_TEST(protocol, bad_magic_is_malformed) {
    std::string frame = frame_of(MessageType::Query, "payload");
    frame[0] = 'X';
    Limits limits;
    FrameHeader header;
    std::string payload;
    std::size_t consumed = 0;
    std::string error;
    FT_CHECK_EQ(decode_frame(frame, limits, header, payload, consumed, error), FrameStatus::Malformed);
    FT_CHECK_EQ(error, std::string("frame.bad_magic"));
}

FT_TEST(protocol, bad_crc_is_malformed) {
    std::string frame = frame_of(MessageType::Query, "payload");
    frame.back() = static_cast<char>(frame.back() ^ 0x5A);
    Limits limits;
    FrameHeader header;
    std::string payload;
    std::size_t consumed = 0;
    std::string error;
    FT_CHECK_EQ(decode_frame(frame, limits, header, payload, consumed, error), FrameStatus::Malformed);
    FT_CHECK_EQ(error, std::string("frame.crc_mismatch"));
}

FT_TEST(protocol, unknown_message_type_is_rejected) {
    // Type field offset: 4-byte magic length prefix + 4-byte magic + 2-byte version.
    constexpr std::size_t kTypeOffset = 10;
    std::string frame = frame_of(MessageType::Query, "payload");
    frame[kTypeOffset] = static_cast<char>(0x7F);
    frame[kTypeOffset + 1] = 0;
    Limits limits;
    FrameHeader header;
    std::string payload;
    std::size_t consumed = 0;
    std::string error;
    FT_CHECK_EQ(decode_frame(frame, limits, header, payload, consumed, error), FrameStatus::Malformed);
    FT_CHECK_EQ(error, std::string("frame.unknown_message_type"));

    frame[kTypeOffset] = 0;
    frame[kTypeOffset + 1] = 0;
    FT_CHECK_EQ(decode_frame(frame, limits, header, payload, consumed, error), FrameStatus::Malformed);
}

FT_TEST(protocol, unsupported_version_is_rejected) {
    constexpr std::size_t kVersionOffset = 8;
    std::string frame = frame_of(MessageType::Query, "payload");
    frame[kVersionOffset] = static_cast<char>(kProtocolVersion + 1);
    Limits limits;
    FrameHeader header;
    std::string payload;
    std::size_t consumed = 0;
    std::string error;
    FT_CHECK_EQ(decode_frame(frame, limits, header, payload, consumed, error), FrameStatus::Malformed);
    FT_CHECK_EQ(error, std::string("frame.version_unsupported"));
}

FT_TEST(protocol, oversized_declared_payload_is_rejected_without_allocating) {
    std::string frame = frame_of(MessageType::Query, "payload");
    // Declare a payload far beyond the frame limit.
    constexpr std::size_t kPayloadLengthOffset = 24;
    const std::uint32_t huge = 0xFFFFFF00U;
    for (unsigned i = 0; i < 4; ++i) {
        frame[kPayloadLengthOffset + i] = static_cast<char>((huge >> (8U * i)) & 0xFFU);
    }
    Limits limits;
    FrameHeader header;
    std::string payload;
    std::size_t consumed = 0;
    std::string error;
    FT_CHECK_EQ(decode_frame(frame, limits, header, payload, consumed, error), FrameStatus::Malformed);
    FT_CHECK_EQ(error, std::string("frame.payload_too_large"));
}

FT_TEST(protocol, trailing_bytes_after_a_frame_are_preserved_for_the_next) {
    const std::string first = frame_of(MessageType::Heartbeat, "one", 1);
    const std::string second = frame_of(MessageType::Query, "two", 2);
    const std::string stream = first + second;
    Limits limits;
    FrameHeader header;
    std::string payload;
    std::size_t consumed = 0;
    std::string error;
    FT_CHECK_EQ(decode_frame(stream, limits, header, payload, consumed, error), FrameStatus::Complete);
    FT_CHECK_EQ(consumed, first.size());
    FT_CHECK_EQ(decode_frame(std::string_view(stream).substr(consumed), limits, header, payload,
                             consumed, error),
                FrameStatus::Complete);
    FT_CHECK_EQ(header.sequence, std::uint64_t{2});
}

FT_TEST(protocol, explanation_round_trip) {
    Explanation explanation(Outcome::CycleRejected, "relationship.cycle");
    explanation.with("from", "n-a");
    explanation.with("to", "n-b");
    RecordWriter writer(128);
    encode_explanation(explanation, writer);
    const std::string encoded = writer.take();
    RecordReader reader(encoded, Limits{});
    Explanation decoded;
    FT_CHECK(decode_explanation(reader, decoded));
    FT_CHECK_EQ(decoded.render(), explanation.render());
    FT_CHECK_EQ(decoded, explanation);
    FT_CHECK(reader.at_end());
}

FT_TEST(protocol, malformed_explanation_is_rejected) {
    RecordWriter writer(32);
    writer.u16(9999);
    writer.text("bad");
    const std::string encoded = writer.take();
    RecordReader reader(encoded, Limits{});
    Explanation decoded;
    FT_CHECK(!decode_explanation(reader, decoded));
    FT_CHECK_EQ(reader.error(), std::string("explanation.outcome"));
}

FT_TEST(protocol, publication_round_trip) {
    Publication publication;
    publication.id = PublicationId::from_trusted("pub-wire");
    publication.domain = TopologyDomainId::from_trusted("dom-wire");
    publication.mode = PublicationMode::AuthoritativeSnapshot;
    publication.type = PublicationType::Synthetic;
    publication.source = DiscoverySource::SyntheticGenerator;
    publication.expected_generation = TopologyGeneration{7};
    publication.evidence_generation = EvidenceGeneration{3};
    publication.note = "wire test";
    publication.declared_absent_relationships.push_back(RelationshipId::from_trusted("rel-absent"));

    ObservedNode node;
    node.node_id = TopologyNodeId::from_trusted("n-wire");
    node.entity_id = "ent-wire";
    node.node_class = NodeClass::Switch;
    node.tier = TopologyTier::Leaf;
    node.domain = publication.domain;
    node.entity_generation = EntityGeneration{2};
    static_cast<void>(node.metadata.set("rack", "r1", Limits{}));
    publication.nodes.push_back(node);

    ObservedEdge edge;
    edge.edge_id = TopologyEdgeId::from_trusted("e-wire");
    edge.relationship_id = RelationshipId::from_trusted("rel-wire");
    edge.relation = RelationClass::MemberOf;
    edge.from = node.node_id;
    edge.to = node.node_id;
    edge.layer = TopologyLayer::Logical;
    edge.domain = publication.domain;
    edge.evidence_generation = EvidenceGeneration{4};
    edge.attachment = AttachmentId::from_trusted("att-wire");
    edge.exclusive_attachment = true;
    publication.edges.push_back(edge);

    RecordWriter writer(512);
    encode_publication(publication, writer);
    const std::string encoded = writer.take();
    RecordReader reader(encoded, Limits{});
    Publication decoded;
    FT_CHECK(decode_publication(reader, decoded));
    FT_CHECK(reader.at_end());
    FT_CHECK_EQ(decoded.id, publication.id);
    FT_CHECK_EQ(decoded.mode, publication.mode);
    FT_CHECK_EQ(decoded.type, publication.type);
    FT_CHECK_EQ(decoded.expected_generation, publication.expected_generation);
    FT_CHECK_EQ(decoded.nodes.size(), std::size_t{1});
    FT_CHECK_EQ(decoded.nodes.front().entity_id, node.entity_id);
    FT_CHECK_EQ(decoded.nodes.front().metadata.items(), node.metadata.items());
    FT_CHECK_EQ(decoded.edges.size(), std::size_t{1});
    FT_CHECK_EQ(decoded.edges.front().attachment, edge.attachment);
    FT_CHECK(decoded.edges.front().exclusive_attachment);
}

FT_TEST(protocol, malformed_publication_is_rejected) {
    RecordWriter writer(64);
    writer.text("pub-x");
    writer.text("dom-x");
    writer.u8(200);  // invalid scope kind
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    const std::string encoded = writer.take();
    RecordReader reader(encoded, Limits{});
    Publication publication;
    FT_CHECK(!decode_publication(reader, publication));
    FT_CHECK_EQ(reader.error(), std::string("publication.enum"));
}

FT_TEST(protocol, absurd_declared_counts_are_rejected) {
    RecordWriter writer(128);
    writer.text("pub-x");
    writer.text("dom-x");
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    writer.u64(0);
    writer.u64(0);
    writer.text("");
    writer.u32(0);            // declared absences
    writer.u32(0xFFFFFFFFU);  // absurd node count
    const std::string encoded = writer.take();
    RecordReader reader(encoded, Limits{});
    Publication publication;
    FT_CHECK(!decode_publication(reader, publication));
    FT_CHECK_EQ(reader.error(), std::string("publication.node_count"));
}

FT_TEST(protocol, message_round_trips) {
    HelloMessage hello;
    hello.publisher = PublisherId::from_trusted("pub-wire");
    hello.worker_boot = WorkerBootId::from_trusted("boot-wire");
    hello.coordinator_epoch = CoordinatorEpoch{5};
    hello.process_label = "unit-test";
    hello.token = "token";
    ScopeGrant grant;
    grant.domain = TopologyDomainId::from_trusted("dom-wire");
    grant.mode = GrantMode::AuthoritativeWrite;
    hello.requested_grants.push_back(grant);
    check_round_trip(hello, "hello");
    {
        WelcomeMessage welcome;
        welcome.accepted = true;
        welcome.coordinator_epoch = CoordinatorEpoch{6};
        welcome.topology_generation = TopologyGeneration{12};
        welcome.session_id = "session-1";
        welcome.reason.set_outcome(Outcome::Ok).set_code("ok");
        welcome.granted = hello.requested_grants;
        check_round_trip(welcome, "welcome");
    }
    {
        HeartbeatMessage heartbeat;
        heartbeat.coordinator_epoch = CoordinatorEpoch{2};
        heartbeat.worker_boot = hello.worker_boot;
        heartbeat.counter = 9;
        check_round_trip(heartbeat, "heartbeat");
    }
    {
        FenceMessage fence;
        fence.worker_boot = hello.worker_boot;
        fence.reason = "worker died";
        check_round_trip(fence, "fence");
    }
    {
        QueryResultMessage result;
        result.status.set_outcome(Outcome::Ok).set_code("coordinator.query_ok");
        result.text = "nodes=0";
        check_round_trip(result, "query result");
    }
    {
        ErrorReportMessage report;
        report.error.set_outcome(Outcome::ProtocolViolation).set_code("bad");
        check_round_trip(report, "error report");
    }
}

FT_TEST(protocol, truncated_message_payload_is_rejected) {
    QueryMessage query;
    query.query = "statistics";
    query.argument = "none";
    const std::string encoded = encode(query);
    for (std::size_t length = 0; length < encoded.size(); ++length) {
        RecordReader reader(std::string_view(encoded).substr(0, length), Limits{});
        QueryMessage decoded;
        FT_CHECK(!decode_message(reader, decoded));
    }
}
