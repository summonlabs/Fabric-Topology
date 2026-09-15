// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Versioned, integrity-checked persistence with a bounded decoder and atomic replacement.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "engine_impl.hpp"
#include "fabric_topology/codec.hpp"
#include "fabric_topology/digest.hpp"
#include "fabric_topology/persistence.hpp"
#include "fabric_topology/topology.hpp"
#include "graph_state.hpp"
#include "validation.hpp"

namespace fabric_topology {

namespace {

constexpr char kStateMagic[8] = {'F', 'T', 'S', 'T', 'A', 'T', 'E', '1'};

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] Status decode_failure(const char* code, const std::string& detail) {
    Status status = make_status(Outcome::PersistenceCorruption, code);
    if (!detail.empty()) {
        status.with("detail", detail);
    }
    return status;
}

void encode_provenance(const Provenance& provenance, RecordWriter& writer) {
    writer.text(provenance.publisher.value());
    writer.text(provenance.worker_boot.value());
    writer.u64(provenance.coordinator_epoch.value());
    writer.u8(static_cast<std::uint8_t>(provenance.source));
    writer.u8(static_cast<std::uint8_t>(provenance.evidence_type));
    writer.u64(provenance.evidence_generation.value());
    writer.text(provenance.source_relationship_id);
    writer.text(provenance.publication.value());
    writer.u64(provenance.created_generation.value());
    writer.u64(provenance.last_validated_generation.value());
}

[[nodiscard]] bool decode_provenance(RecordReader& reader, Provenance& provenance) {
    provenance.publisher = PublisherId::from_trusted(reader.text(kMaxIdentifierBytes));
    provenance.worker_boot = WorkerBootId::from_trusted(reader.text(kMaxIdentifierBytes));
    provenance.coordinator_epoch = CoordinatorEpoch{reader.u64()};
    const std::uint8_t source = reader.u8();
    if (!reader.ok() || source >= kDiscoverySourceCount) {
        return reader.fail("provenance.source");
    }
    provenance.source = static_cast<DiscoverySource>(source);
    const std::uint8_t evidence_type = reader.u8();
    if (!reader.ok() || evidence_type >= kPublicationTypeCount) {
        return reader.fail("provenance.evidence_type");
    }
    provenance.evidence_type = static_cast<PublicationType>(evidence_type);
    provenance.evidence_generation = EvidenceGeneration{reader.u64()};
    provenance.source_relationship_id = reader.text(kMaxIdentifierBytes);
    provenance.publication = PublicationId::from_trusted(reader.text(kMaxIdentifierBytes));
    provenance.created_generation = TopologyGeneration{reader.u64()};
    provenance.last_validated_generation = TopologyGeneration{reader.u64()};
    return reader.ok();
}

void encode_metadata(const Metadata& metadata, RecordWriter& writer) {
    writer.u32(static_cast<std::uint32_t>(metadata.size()));
    for (const Metadata::Item& item : metadata.items()) {
        writer.text(item.first);
        writer.text(item.second);
    }
}

[[nodiscard]] bool decode_metadata(RecordReader& reader, Metadata& metadata, const Limits& limits) {
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
    return reader.ok();
}

void encode_node(const TopologyNode& node, RecordWriter& writer) {
    writer.text(node.id.value());
    writer.text(node.entity_id);
    writer.u8(static_cast<std::uint8_t>(node.entity_class));
    writer.u64(node.entity_generation.value());
    writer.u8(static_cast<std::uint8_t>(node.node_class));
    writer.u8(static_cast<std::uint8_t>(node.tier));
    writer.text(node.domain.value());
    writer.u64(node.generation.value());
    writer.u64(node.created_generation.value());
    writer.u64(node.last_validated_generation.value());
    writer.u8(static_cast<std::uint8_t>(node.lifecycle));
    encode_provenance(node.provenance, writer);
    encode_metadata(node.metadata, writer);
}

[[nodiscard]] bool decode_identifier(RecordReader& reader, std::string& out, std::size_t max_bytes);

[[nodiscard]] bool decode_node(RecordReader& reader, const Limits& limits, TopologyNode& node) {
    std::string id;
    if (!decode_identifier(reader, id, kMaxIdentifierBytes)) {
        return false;
    }
    node.id = TopologyNodeId::from_trusted(id);
    node.entity_id = reader.text(limits.max_identifier_bytes);
    const std::uint8_t entity_class = reader.u8();
    if (!reader.ok() || entity_class >= kEntityClassCount) {
        return reader.fail("node.entity_class");
    }
    if (!node.entity_id.empty() && !detail::is_valid_identifier(node.entity_id, limits.max_identifier_bytes)) {
        return reader.fail("node.entity_id_malformed");
    }
    node.entity_class = static_cast<EntityClass>(entity_class);
    node.entity_generation = EntityGeneration{reader.u64()};
    const std::uint8_t node_class = reader.u8();
    if (!reader.ok() || node_class >= kNodeClassCount) {
        return reader.fail("node.node_class");
    }
    node.node_class = static_cast<NodeClass>(node_class);
    const std::uint8_t tier = reader.u8();
    if (!reader.ok() || tier >= kTopologyTierCount) {
        return reader.fail("node.tier");
    }
    node.tier = static_cast<TopologyTier>(tier);
    node.domain = TopologyDomainId::from_trusted(reader.text(kMaxIdentifierBytes));
    node.generation = NodeGeneration{reader.u64()};
    node.created_generation = TopologyGeneration{reader.u64()};
    node.last_validated_generation = TopologyGeneration{reader.u64()};
    const std::uint8_t lifecycle = reader.u8();
    if (!reader.ok() || lifecycle >= kLifecycleStateCount) {
        return reader.fail("node.lifecycle");
    }
    node.lifecycle = static_cast<LifecycleState>(lifecycle);
    if (!decode_provenance(reader, node.provenance)) {
        return false;
    }
    return decode_metadata(reader, node.metadata, limits);
}

void encode_edge(const TopologyEdge& edge, RecordWriter& writer) {
    writer.text(edge.id.value());
    writer.text(edge.relationship_id.value());
    writer.u8(static_cast<std::uint8_t>(edge.relation));
    writer.text(edge.from.value());
    writer.text(edge.to.value());
    writer.u8(static_cast<std::uint8_t>(edge.layer));
    writer.text(edge.domain.value());
    writer.boolean(edge.cross_domain);
    writer.text(edge.secondary_domain.value());
    writer.u64(edge.generation.value());
    writer.u64(edge.created_generation.value());
    writer.u64(edge.last_validated_generation.value());
    writer.u64(edge.evidence_generation.value());
    writer.u64(edge.from_entity_generation.value());
    writer.u64(edge.to_entity_generation.value());
    writer.u8(static_cast<std::uint8_t>(edge.lifecycle));
    writer.optional_text(edge.supported_by.has_value(),
                         edge.supported_by.has_value() ? edge.supported_by->value() : std::string_view{});
    writer.optional_text(edge.superseded_by.has_value(),
                         edge.superseded_by.has_value() ? edge.superseded_by->value() : std::string_view{});
    writer.optional_text(edge.attachment.has_value(),
                         edge.attachment.has_value() ? edge.attachment->value() : std::string_view{});
    encode_provenance(edge.provenance, writer);
    encode_metadata(edge.metadata, writer);
}

[[nodiscard]] bool decode_edge(RecordReader& reader, const Limits& limits, TopologyEdge& edge) {
    std::string edge_identifier;
    std::string relationship_identifier;
    std::string from_identifier;
    std::string to_identifier;
    std::string domain_identifier;
    if (!decode_identifier(reader, edge_identifier, kMaxIdentifierBytes) ||
        !decode_identifier(reader, relationship_identifier, kMaxIdentifierBytes)) {
        return false;
    }
    edge.id = TopologyEdgeId::from_trusted(edge_identifier);
    edge.relationship_id = RelationshipId::from_trusted(relationship_identifier);
    const std::uint8_t relation = reader.u8();
    if (!reader.ok() || relation >= kRelationClassCount) {
        return reader.fail("edge.relation");
    }
    edge.relation = static_cast<RelationClass>(relation);
    if (!decode_identifier(reader, from_identifier, kMaxIdentifierBytes) ||
        !decode_identifier(reader, to_identifier, kMaxIdentifierBytes)) {
        return false;
    }
    edge.from = TopologyNodeId::from_trusted(from_identifier);
    edge.to = TopologyNodeId::from_trusted(to_identifier);
    const std::uint8_t layer = reader.u8();
    if (!reader.ok() || layer > static_cast<std::uint8_t>(TopologyLayer::Logical)) {
        return reader.fail("edge.layer");
    }
    edge.layer = static_cast<TopologyLayer>(layer);
    if (!decode_identifier(reader, domain_identifier, kMaxIdentifierBytes)) {
        return false;
    }
    edge.domain = TopologyDomainId::from_trusted(domain_identifier);
    edge.cross_domain = reader.boolean();
    if (!decode_identifier(reader, domain_identifier, kMaxIdentifierBytes)) {
        return false;
    }
    edge.secondary_domain = TopologyDomainId::from_trusted(domain_identifier);
    edge.generation = EdgeGeneration{reader.u64()};
    edge.created_generation = TopologyGeneration{reader.u64()};
    edge.last_validated_generation = TopologyGeneration{reader.u64()};
    edge.evidence_generation = EvidenceGeneration{reader.u64()};
    edge.from_entity_generation = EntityGeneration{reader.u64()};
    edge.to_entity_generation = EntityGeneration{reader.u64()};
    const std::uint8_t lifecycle = reader.u8();
    if (!reader.ok() || lifecycle >= kLifecycleStateCount) {
        return reader.fail("edge.lifecycle");
    }
    edge.lifecycle = static_cast<LifecycleState>(lifecycle);

    const bool has_support = reader.boolean();
    const std::string support = reader.text(kMaxIdentifierBytes);
    if (!reader.ok()) {
        return false;
    }
    if (has_support) {
        edge.supported_by = TopologyEdgeId::from_trusted(support);
    }
    const bool has_supersede = reader.boolean();
    const std::string supersede = reader.text(kMaxIdentifierBytes);
    if (!reader.ok()) {
        return false;
    }
    if (has_supersede) {
        edge.superseded_by = TopologyEdgeId::from_trusted(supersede);
    }
    const bool has_attachment = reader.boolean();
    const std::string attachment = reader.text(kMaxIdentifierBytes);
    if (!reader.ok()) {
        return false;
    }
    if (has_attachment) {
        edge.attachment = AttachmentId::from_trusted(attachment);
    }
    if (!decode_provenance(reader, edge.provenance)) {
        return false;
    }
    return decode_metadata(reader, edge.metadata, limits);
}

[[nodiscard]] bool decode_identifier(RecordReader& reader, std::string& out, std::size_t max_bytes) {
    out = reader.text(max_bytes);
    if (!reader.ok()) {
        return false;
    }
    if (!out.empty() && !detail::is_valid_identifier(out, max_bytes)) {
        return reader.fail("identifier.malformed");
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

namespace internal {

void encode_state_payload(const GraphState& state, const CoordinatorEpoch& epoch,
                          const std::vector<std::string>& fenced_boots, RecordWriter& writer) {
    writer.text("FABRIC-TOPOLOGY-STATE");
    writer.u32(kPersistenceFormatVersion);
    writer.u64(state.generation.value());
    writer.u64(state.snapshot_counter.value());
    writer.u64(epoch.value());

    const std::vector<TopologyNode> nodes = state.sorted_nodes(SnapshotScope::all());
    writer.u64(nodes.size());
    for (const TopologyNode& node : nodes) {
        encode_node(node, writer);
    }

    const std::vector<TopologyEdge> edges = state.sorted_edges(SnapshotScope::all());
    writer.u64(edges.size());
    for (const TopologyEdge& edge : edges) {
        encode_edge(edge, writer);
    }

    std::vector<TopologyDomainId> domain_ids;
    domain_ids.reserve(state.domain_definitions.size());
    for (const auto& entry : state.domain_definitions) {
        domain_ids.push_back(entry.first);
    }
    std::sort(domain_ids.begin(), domain_ids.end());
    writer.u64(domain_ids.size());
    for (const TopologyDomainId& id : domain_ids) {
        const DomainDefinition& definition = state.domain_definitions.at(id);
        writer.text(definition.id.value());
        writer.u8(static_cast<std::uint8_t>(definition.kind));
        writer.text(definition.anchor_entity_id);
        writer.u8(static_cast<std::uint8_t>(definition.anchor_entity_class));
        writer.text(definition.parent.value());
    }

    std::vector<CrossDomainRule> rules = state.cross_domain_rules;
    std::sort(rules.begin(), rules.end(), [](const CrossDomainRule& a, const CrossDomainRule& b) {
        if (a.from_domain != b.from_domain) {
            return a.from_domain < b.from_domain;
        }
        if (a.to_domain != b.to_domain) {
            return a.to_domain < b.to_domain;
        }
        return static_cast<std::uint8_t>(a.relation) < static_cast<std::uint8_t>(b.relation);
    });
    writer.u64(rules.size());
    for (const CrossDomainRule& rule : rules) {
        writer.text(rule.from_domain.value());
        writer.text(rule.to_domain.value());
        writer.u8(static_cast<std::uint8_t>(rule.relation));
        writer.boolean(rule.symmetric);
    }

    std::vector<std::string> boots = fenced_boots;
    std::sort(boots.begin(), boots.end());
    writer.u64(boots.size());
    for (const std::string& boot : boots) {
        writer.text(boot);
    }
}

Status decode_state_payload(std::string_view payload, const Limits& limits, GraphState& state,
                            CoordinatorEpoch& epoch, std::vector<std::string>& fenced_boots,
                            LoadReport& report) {
    RecordReader reader(payload, limits);
    const std::string tag = reader.text(64);
    if (!reader.ok() || tag != "FABRIC-TOPOLOGY-STATE") {
        return decode_failure("state.tag", reader.error());
    }
    const std::uint32_t version = reader.u32();
    if (!reader.ok()) {
        return decode_failure("state.version", reader.error());
    }
    if (version != kPersistenceFormatVersion) {
        Status status = make_status(Outcome::PersistenceCorruption, "state.version_unsupported");
        status.with("version", std::to_string(version));
        status.with("supported", std::to_string(kPersistenceFormatVersion));
        return status;
    }

    state.generation = TopologyGeneration{reader.u64()};
    state.snapshot_counter = SnapshotGeneration{reader.u64()};
    epoch = CoordinatorEpoch{reader.u64()};
    if (!reader.ok()) {
        return decode_failure("state.header", reader.error());
    }

    const std::uint64_t node_count = reader.u64();
    if (!reader.ok() || node_count > limits.max_nodes || node_count > limits.max_persistence_records) {
        return decode_failure("state.node_count", reader.error());
    }
    for (std::uint64_t i = 0; i < node_count; ++i) {
        TopologyNode node;
        if (!decode_node(reader, limits, node)) {
            return decode_failure("state.node", reader.error());
        }
        if (node.id.empty() || node.entity_id.empty()) {
            return decode_failure("state.node_identity", {});
        }
        if (!state.insert_node(node)) {
            return decode_failure("state.duplicate_node", node.id.to_string());
        }
    }

    const std::uint64_t edge_count = reader.u64();
    if (!reader.ok() || edge_count > limits.max_edges || edge_count > limits.max_persistence_records) {
        return decode_failure("state.edge_count", reader.error());
    }
    for (std::uint64_t i = 0; i < edge_count; ++i) {
        TopologyEdge edge;
        if (!decode_edge(reader, limits, edge)) {
            return decode_failure("state.edge", reader.error());
        }
        if (edge.id.empty() || edge.relationship_id.empty()) {
            return decode_failure("state.edge_identity", {});
        }
        if (state.find_edge(edge.id) != nullptr) {
            return decode_failure("state.duplicate_edge", edge.id.to_string());
        }
        if (state.find_edge_by_relationship(edge.relationship_id) != nullptr) {
            return decode_failure("state.duplicate_relationship", edge.relationship_id.to_string());
        }
        if (state.find_node(edge.from) == nullptr || state.find_node(edge.to) == nullptr) {
            return decode_failure("state.dangling_endpoint", edge.id.to_string());
        }
        if (!state.insert_edge(edge)) {
            return decode_failure("state.edge_insert", edge.id.to_string());
        }
    }

    const std::uint64_t domain_count = reader.u64();
    if (!reader.ok() || domain_count > limits.max_persistence_records) {
        return decode_failure("state.domain_count", reader.error());
    }
    for (std::uint64_t i = 0; i < domain_count; ++i) {
        DomainDefinition definition;
        std::string id;
        if (!decode_identifier(reader, id, kMaxIdentifierBytes)) {
            return decode_failure("state.domain_id", reader.error());
        }
        definition.id = TopologyDomainId::from_trusted(id);
        const std::uint8_t kind = reader.u8();
        if (!reader.ok() || kind >= kScopeKindCount) {
            return decode_failure("state.domain_kind", reader.error());
        }
        definition.kind = static_cast<ScopeKind>(kind);
        definition.anchor_entity_id = reader.text(limits.max_identifier_bytes);
        const std::uint8_t anchor_class = reader.u8();
        if (!reader.ok() || anchor_class >= kEntityClassCount) {
            return decode_failure("state.domain_anchor_class", reader.error());
        }
        definition.anchor_entity_class = static_cast<EntityClass>(anchor_class);
        std::string parent;
        if (!decode_identifier(reader, parent, kMaxIdentifierBytes)) {
            return decode_failure("state.domain_parent", reader.error());
        }
        definition.parent = TopologyDomainId::from_trusted(parent);
        if (definition.id.empty()) {
            return decode_failure("state.domain_identity", {});
        }
        if (state.domain_definitions.find(definition.id) != state.domain_definitions.end()) {
            return decode_failure("state.duplicate_domain", definition.id.to_string());
        }
        state.domain_definitions.emplace(definition.id, definition);
        state.domains[definition.id];
    }

    const std::uint64_t rule_count = reader.u64();
    if (!reader.ok() || rule_count > limits.max_persistence_records) {
        return decode_failure("state.rule_count", reader.error());
    }
    for (std::uint64_t i = 0; i < rule_count; ++i) {
        CrossDomainRule rule;
        std::string from_domain;
        std::string to_domain;
        if (!decode_identifier(reader, from_domain, kMaxIdentifierBytes) ||
            !decode_identifier(reader, to_domain, kMaxIdentifierBytes)) {
            return decode_failure("state.rule_domain", reader.error());
        }
        rule.from_domain = TopologyDomainId::from_trusted(from_domain);
        rule.to_domain = TopologyDomainId::from_trusted(to_domain);
        const std::uint8_t relation = reader.u8();
        if (!reader.ok() || relation == 0 || relation >= kRelationClassCount) {
            return decode_failure("state.rule_relation", reader.error());
        }
        rule.relation = static_cast<RelationClass>(relation);
        rule.symmetric = reader.boolean();
        if (!reader.ok()) {
            return decode_failure("state.rule", reader.error());
        }
        if (state.domain_definitions.find(rule.from_domain) == state.domain_definitions.end() ||
            state.domain_definitions.find(rule.to_domain) == state.domain_definitions.end()) {
            return decode_failure("state.rule_domain_unknown", {});
        }
        state.cross_domain_rules.push_back(rule);
    }

    const std::uint64_t boot_count = reader.u64();
    if (!reader.ok() || boot_count > limits.max_persistence_records) {
        return decode_failure("state.boot_count", reader.error());
    }
    for (std::uint64_t i = 0; i < boot_count; ++i) {
        std::string boot;
        if (!decode_identifier(reader, boot, kMaxIdentifierBytes)) {
            return decode_failure("state.boot", reader.error());
        }
        fenced_boots.push_back(boot);
    }

    if (!reader.at_end()) {
        return decode_failure("state.trailing_bytes", std::to_string(reader.remaining()));
    }
    report.nodes = state.nodes.size();
    report.edges = state.edges.size();
    return make_status(Outcome::Ok, "state.decoded");
}

}  // namespace internal

// ---------------------------------------------------------------------------
// Container helpers
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] std::string encode_container(const std::string& payload) {
    RecordWriter header(64);
    header.blob(std::string_view(kStateMagic, sizeof(kStateMagic)));
    header.u32(kPersistenceFormatVersion);
    header.u64(payload.size());
    header.u32(crc32(payload));
    const std::string digest = sha256_raw(payload);
    header.blob(digest);

    std::string container = header.take();
    container += payload;
    return container;
}

[[nodiscard]] Status read_file(const std::string& path, std::string& out) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        Status status = make_status(Outcome::PersistenceIoFailure, "persistence.stat_failed");
        status.with("path", path);
        status.with("detail", error.message());
        return status;
    }
    out.clear();
    out.resize(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        Status status = make_status(Outcome::PersistenceIoFailure, "persistence.open_failed");
        status.with("path", path);
        return status;
    }
    if (size != 0) {
        stream.read(out.data(), static_cast<std::streamsize>(out.size()));
        if (!stream) {
            Status status = make_status(Outcome::PersistenceIoFailure, "persistence.read_failed");
            status.with("path", path);
            return status;
        }
    }
    return make_status(Outcome::Ok, "persistence.read");
}

[[nodiscard]] Status write_file_atomic(const std::string& path, const std::string& bytes) {
    std::error_code error;
    const std::filesystem::path target(path);
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            Status status = make_status(Outcome::PersistenceIoFailure, "persistence.open_failed");
            status.with("path", temporary.string());
            return status;
        }
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            Status status = make_status(Outcome::PersistenceIoFailure, "persistence.write_failed");
            status.with("path", temporary.string());
            stream.close();
            std::filesystem::remove(temporary, error);
            return status;
        }
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        Status status = make_status(Outcome::PersistenceIoFailure, "persistence.rename_failed");
        status.with("path", path);
        status.with("detail", error.message());
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return status;
    }
    return make_status(Outcome::Ok, "persistence.ok");
}

}  // namespace

Status inspect_persistence_file(const std::string& path, PersistenceHeaderInfo& out) {
    std::string bytes;
    Status read = read_file(path, bytes);
    if (read.outcome() != Outcome::Ok) {
        return read;
    }
    if (bytes.size() < kPersistenceHeaderBytes) {
        return decode_failure("container.truncated_header", std::to_string(bytes.size()));
    }
    RecordReader reader(bytes, Limits{});
    const std::string magic = reader.blob(sizeof(kStateMagic));
    if (!reader.ok() || magic != std::string(kStateMagic, sizeof(kStateMagic))) {
        return decode_failure("container.bad_magic", {});
    }
    out.format_version = reader.u32();
    out.payload_bytes = reader.u64();
    out.payload_crc32 = reader.u32();
    const std::string digest = reader.blob(64);
    if (!reader.ok()) {
        return decode_failure("container.header", reader.error());
    }
    const std::string raw_digest = digest;
    out.payload_sha256 = [&digest] {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(digest.size() * 2);
        for (char c : digest) {
            const auto value = static_cast<unsigned char>(c);
            hex.push_back(kDigits[(value >> 4U) & 0x0FU]);
            hex.push_back(kDigits[value & 0x0FU]);
        }
        return hex;
    }();
    out.file_bytes = bytes.size();
    if (out.format_version != kPersistenceFormatVersion) {
        Status status = make_status(Outcome::PersistenceCorruption, "container.version_unsupported");
        status.with("version", std::to_string(out.format_version));
        return status;
    }
    if (out.payload_bytes > Limits{}.max_persistence_bytes) {
        return decode_failure("container.payload_too_large", std::to_string(out.payload_bytes));
    }
    if (out.payload_bytes != bytes.size() - kPersistenceHeaderBytes) {
        return decode_failure("container.length_mismatch",
                              std::to_string(out.payload_bytes) + " vs " +
                                  std::to_string(bytes.size() - kPersistenceHeaderBytes));
    }
    const std::string_view payload(bytes.data() + kPersistenceHeaderBytes,
                                   bytes.size() - kPersistenceHeaderBytes);
    if (crc32(payload) != out.payload_crc32) {
        return decode_failure("container.crc_mismatch", {});
    }
    if (sha256_raw(payload) != raw_digest) {
        return decode_failure("container.sha256_mismatch", {});
    }
    return make_status(Outcome::Ok, "container.ok");
}

// ---------------------------------------------------------------------------
// Engine persistence
// ---------------------------------------------------------------------------

Status TopologyEngine::Impl::persist_locked() const {
    if (options.persistence_path.empty()) {
        return make_status(Outcome::Ok, "persistence.disabled");
    }
    return persist_to_locked(options.persistence_path);
}

Status TopologyEngine::Impl::persist_to_locked(const std::string& path) const {
    RecordWriter writer(1U << 16);
    internal::encode_state_payload(graph, coordinator_epoch,
                                   std::vector<std::string>(fenced_boots.begin(), fenced_boots.end()),
                                   writer);
    const std::string container = encode_container(writer.data());
    std::lock_guard<std::mutex> lock(persistence_mutex);
    return write_file_atomic(path, container);
}

Status TopologyEngine::save() const {
    std::unique_lock state_lock(impl_->state_mutex);
    return impl_->persist_locked();
}

Status TopologyEngine::save_to(const std::string& path) const {
    std::unique_lock state_lock(impl_->state_mutex);
    return impl_->persist_to_locked(path);
}

LoadReport TopologyEngine::load_from(const std::string& path) {
    LoadReport report;
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        report.status = make_status(Outcome::Ok, "persistence.absent");
        return report;
    }
    std::string bytes;
    const Status read = read_file(path, bytes);
    if (read.outcome() != Outcome::Ok) {
        report.status = read;
        return report;
    }
    if (bytes.size() < kPersistenceHeaderBytes) {
        report.status = decode_failure("container.truncated_header", std::to_string(bytes.size()));
        return report;
    }

    RecordReader reader(bytes, impl_->limits);
    const std::string magic = reader.blob(sizeof(kStateMagic));
    if (!reader.ok() || magic != std::string(kStateMagic, sizeof(kStateMagic))) {
        report.status = decode_failure("container.bad_magic", {});
        return report;
    }
    const std::uint32_t version = reader.u32();
    const std::uint64_t payload_bytes = reader.u64();
    const std::uint32_t payload_crc = reader.u32();
    const std::string payload_digest = reader.blob(64);
    if (!reader.ok()) {
        report.status = decode_failure("container.header", reader.error());
        return report;
    }
    if (version != kPersistenceFormatVersion) {
        Status status = make_status(Outcome::PersistenceCorruption, "container.version_unsupported");
        status.with("version", std::to_string(version));
        report.status = status;
        return report;
    }
    if (payload_bytes > impl_->limits.max_persistence_bytes) {
        report.status = decode_failure("container.payload_too_large", std::to_string(payload_bytes));
        return report;
    }
    if (payload_bytes != bytes.size() - kPersistenceHeaderBytes) {
        report.status = decode_failure("container.length_mismatch", std::to_string(payload_bytes));
        return report;
    }
    const std::string_view payload(bytes.data() + kPersistenceHeaderBytes,
                                   bytes.size() - kPersistenceHeaderBytes);
    if (crc32(payload) != payload_crc) {
        report.status = decode_failure("container.crc_mismatch", {});
        return report;
    }
    if (sha256_raw(payload) != payload_digest) {
        report.status = decode_failure("container.sha256_mismatch", {});
        return report;
    }

    internal::GraphState recovered(impl_->limits);
    CoordinatorEpoch stored_epoch;
    std::vector<std::string> fenced;
    report.status = internal::decode_state_payload(payload, impl_->limits, recovered, stored_epoch,
                                                   fenced, report);
    if (report.status.outcome() != Outcome::Ok) {
        return report;
    }

    // Containment and dependency acyclicity encoded on disk must not be trusted.
    const ValidationReport validation =
        internal::validate_graph(recovered, impl_->directory.get(), SnapshotScope::all());
    if (!validation.valid) {
        Status status = make_status(Outcome::PersistenceCorruption, "state.invariant_violation");
        status.with("issues", std::to_string(validation.issues.size()));
        if (!validation.issues.empty()) {
            status.with("first_issue", validation.issues.front().code);
        }
        report.status = status;
        return report;
    }

    std::unique_lock authority_lock(impl_->authority_mutex);
    std::unique_lock state_lock(impl_->state_mutex);

    // Conservative recovery: durable structure survives, live authority does not.
    impl_->publishers.clear();
    impl_->attempts = internal::AttemptLedger{};
    impl_->fenced_boots.clear();
    for (const std::string& boot : fenced) {
        if (detail::is_valid_identifier(boot, kMaxIdentifierBytes)) {
            impl_->fenced_boots.insert(boot);
        }
    }

    // The coordinator epoch advances on every fresh start, so traffic from the previous
    // incarnation can never mutate topology again.
    CoordinatorEpoch next_epoch = stored_epoch;
    if (next_epoch.can_advance()) {
        next_epoch = next_epoch.next();
    }
    if (next_epoch < impl_->options.initial_coordinator_epoch) {
        next_epoch = impl_->options.initial_coordinator_epoch;
    }

    std::size_t demoted_edges = 0;
    std::vector<TopologyEdgeId> recovered_edge_ids;
    recovered_edge_ids.reserve(recovered.edges.size());
    for (const auto& entry : recovered.edges) {
        recovered_edge_ids.push_back(entry.first);
    }
    std::sort(recovered_edge_ids.begin(), recovered_edge_ids.end());
    for (const TopologyEdgeId& edge_id : recovered_edge_ids) {
        const TopologyEdge* stored = recovered.find_edge(edge_id);
        if (stored == nullptr) {
            continue;
        }
        if (stored->provenance.publisher.empty()) {
            continue;
        }
        if (stored->lifecycle == LifecycleState::Retired) {
            ++report.retired_edges;
            continue;
        }
        if (stored->lifecycle == LifecycleState::Superseded) {
            ++report.superseded_edges;
            continue;
        }
        if (stored->lifecycle == LifecycleState::RevalidationRequired) {
            continue;
        }
        static_cast<void>(recovered.mutate_edge(edge_id, [](TopologyEdge& edge) {
            edge.lifecycle = LifecycleState::RevalidationRequired;
            if (edge.generation.can_advance()) {
                edge.generation = edge.generation.next();
            }
        }));
        ++demoted_edges;
    }
    // A recovered relationship never silently inherits a newer registry incarnation.
    std::vector<TopologyNodeId> recovered_node_ids;
    recovered_node_ids.reserve(recovered.nodes.size());
    for (const auto& entry : recovered.nodes) {
        recovered_node_ids.push_back(entry.first);
    }
    std::sort(recovered_node_ids.begin(), recovered_node_ids.end());
    for (const TopologyNodeId& node_id : recovered_node_ids) {
        const TopologyNode* stored = recovered.find_node(node_id);
        if (stored == nullptr || impl_->directory == nullptr) {
            continue;
        }
        const auto record = impl_->directory->lookup(stored->entity_id);
        if (record.has_value() && !record->retired && !record->superseded &&
            record->generation == stored->entity_generation) {
            continue;
        }
        static_cast<void>(recovered.mutate_node(node_id, [](TopologyNode& node) {
            node.lifecycle = LifecycleState::RevalidationRequired;
            if (node.generation.can_advance()) {
                node.generation = node.generation.next();
            }
        }));
        ++report.revalidation_required_nodes;
    }

    impl_->graph = std::move(recovered);
    impl_->coordinator_epoch = next_epoch;
    impl_->snapshots.clear();

    report.loaded = true;
    report.recovered = true;
    report.generation = impl_->graph.generation;
    report.coordinator_epoch = impl_->coordinator_epoch;
    report.revalidation_required_edges = demoted_edges;
    report.status = make_status(Outcome::Committed, "persistence.recovered");
    report.status.with("generation", report.generation.to_string());
    report.status.with("coordinator_epoch", report.coordinator_epoch.to_string());
    report.status.with("nodes", std::to_string(report.nodes));
    report.status.with("edges", std::to_string(report.edges));
    return report;
}

std::string LoadReport::render() const {
    std::string out;
    out.reserve(192);
    out += "loaded=";
    out += loaded ? "true" : "false";
    out += " recovered=";
    out += recovered ? "true" : "false";
    out += " generation=";
    out += generation.to_string();
    out += " coordinator_epoch=";
    out += coordinator_epoch.to_string();
    out += " nodes=";
    out += std::to_string(nodes);
    out += " edges=";
    out += std::to_string(edges);
    out += " revalidation_required_edges=";
    out += std::to_string(revalidation_required_edges);
    out += " revalidation_required_nodes=";
    out += std::to_string(revalidation_required_nodes);
    out += '\n';
    out += status.render();
    return out;
}

const char* to_string(PersistenceMode value) noexcept {
    return value == PersistenceMode::Immediate ? "IMMEDIATE" : "MANUAL";
}

}  // namespace fabric_topology
