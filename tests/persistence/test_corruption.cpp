// Fabric Topology - persistence corruption and truncation tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Every case below writes a deliberately hostile container and requires the decoder to reject
// it safely: no crash, no partial authoritative state, a precise outcome.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "test_framework.hpp"
#include "topology_fixture.hpp"

using namespace fabric_topology;

namespace {

struct TempDirectory {
    std::filesystem::path path;
    explicit TempDirectory(const std::string& name) {
        path = std::filesystem::temp_directory_path() / ("fabric_topology_corrupt_" + name);
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    [[nodiscard]] std::string file(const std::string& name) const {
        return (path / name).string();
    }
};

constexpr char kMagic[8] = {'F', 'T', 'S', 'T', 'A', 'T', 'E', '1'};

std::string wrap(const std::string& payload, std::uint32_t version = kPersistenceFormatVersion,
                 bool break_crc = false, bool break_sha = false) {
    RecordWriter header(64);
    header.blob(std::string_view(kMagic, sizeof(kMagic)));
    header.u32(version);
    header.u64(payload.size());
    header.u32(break_crc ? (crc32(payload) ^ 0x1U) : crc32(payload));
    std::string digest = sha256_raw(payload);
    if (break_sha) {
        digest[0] = static_cast<char>(digest[0] ^ 0x7F);
    }
    header.blob(digest);
    std::string container = header.take();
    container += payload;
    return container;
}

void write_file(const std::string& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Build a valid state payload, then hand it to the mutator for deliberate damage.
struct StateBuilder {
    RecordWriter writer{1024};

    StateBuilder() {
        writer.text("FABRIC-TOPOLOGY-STATE");
        writer.u32(kPersistenceFormatVersion);
        writer.u64(1);  // topology generation
        writer.u64(0);  // snapshot counter
        writer.u64(3);  // coordinator epoch
    }

    void node(const std::string& id, const std::string& entity_id, std::uint8_t entity_class,
              std::uint8_t node_class, const std::string& domain) {
        writer.text(id);
        writer.text(entity_id);
        writer.u8(entity_class);
        writer.u64(1);  // entity generation
        writer.u8(node_class);
        writer.u8(0);   // tier
        writer.text(domain);
        writer.u64(1);  // node generation
        writer.u64(1);  // created generation
        writer.u64(1);  // last validated generation
        writer.u8(1);   // lifecycle Current
        writer.text("");  // publisher
        writer.text("");  // worker boot
        writer.u64(0);    // coordinator epoch
        writer.u8(0);     // discovery source
        writer.u8(0);     // evidence type
        writer.u64(0);    // evidence generation
        writer.text("");  // source relationship id
        writer.text("");  // publication
        writer.u64(1);
        writer.u64(1);
        writer.u32(0);  // metadata
    }

    void edge(const std::string& id, const std::string& relationship, std::uint8_t relation,
              const std::string& from, const std::string& to, const std::string& domain) {
        writer.text(id);
        writer.text(relationship);
        writer.u8(relation);
        writer.text(from);
        writer.text(to);
        writer.u8(0);  // layer physical
        writer.text(domain);
        writer.boolean(false);  // cross domain
        writer.text(domain);
        writer.u64(1);
        writer.u64(1);
        writer.u64(1);
        writer.u64(1);
        writer.u64(1);
        writer.u64(1);
        writer.u8(1);  // lifecycle current
        writer.boolean(false);
        writer.text("");
        writer.boolean(false);
        writer.text("");
        writer.boolean(false);
        writer.text("");
        writer.text("");
        writer.text("");
        writer.u64(0);
        writer.u8(0);
        writer.u8(0);
        writer.u64(0);
        writer.text("");
        writer.text("");
        writer.u64(1);
        writer.u64(1);
        writer.u32(0);
    }

    void domain(const std::string& id) {
        writer.text(id);
        writer.u8(0);
        writer.text("");
        writer.u8(0);
        writer.text("");
    }
};

void expect_corrupt(const std::string& path, const char* code) {
    auto directory = std::make_shared<InMemoryEntityDirectory>();
    TopologyEngineOptions options;
    options.directory = directory;
    options.persistence_path = path;
    LoadReport report;
    const std::unique_ptr<TopologyEngine> engine = TopologyEngine::open(options, report);
    FT_CHECK(!report.loaded);
    FT_CHECK_EQ(report.status.outcome(), Outcome::PersistenceCorruption);
    if (code != nullptr) {
        FT_CHECK_EQ(report.status.code(), std::string(code));
    }
    FT_CHECK_EQ(engine->node_count(), std::size_t{0});
    FT_CHECK_EQ(engine->edge_count(), std::size_t{0});
}

}  // namespace

FT_TEST(corruption, empty_file) {
    const TempDirectory temp("empty");
    const std::string path = temp.file("state.ftstate");
    write_file(path, "");
    expect_corrupt(path, "container.truncated_header");
}

FT_TEST(corruption, truncated_header) {
    const TempDirectory temp("trunc_header");
    const std::string path = temp.file("state.ftstate");
    std::string container = wrap("payload");
    container.resize(kPersistenceHeaderBytes - 1);
    write_file(path, container);
    expect_corrupt(path, "container.truncated_header");
}

FT_TEST(corruption, truncated_payload) {
    const TempDirectory temp("trunc_payload");
    const std::string path = temp.file("state.ftstate");
    std::string container = wrap(std::string(200, 'a'));
    container.resize(container.size() - 50);
    write_file(path, container);
    expect_corrupt(path, "container.length_mismatch");
}

FT_TEST(corruption, bad_magic) {
    const TempDirectory temp("bad_magic");
    const std::string path = temp.file("state.ftstate");
    std::string container = wrap("payload");
    container[0] = 'Z';
    write_file(path, container);
    expect_corrupt(path, "container.bad_magic");
}

FT_TEST(corruption, unsupported_version) {
    const TempDirectory temp("bad_version");
    const std::string path = temp.file("state.ftstate");
    write_file(path, wrap("payload", kPersistenceFormatVersion + 1));
    expect_corrupt(path, "container.version_unsupported");
}

FT_TEST(corruption, wrong_crc) {
    const TempDirectory temp("bad_crc");
    const std::string path = temp.file("state.ftstate");
    write_file(path, wrap("payload", kPersistenceFormatVersion, true, false));
    expect_corrupt(path, "container.crc_mismatch");
}

FT_TEST(corruption, wrong_sha256) {
    const TempDirectory temp("bad_sha");
    const std::string path = temp.file("state.ftstate");
    write_file(path, wrap("payload", kPersistenceFormatVersion, true, true));
    expect_corrupt(path, nullptr);
}

FT_TEST(corruption, valid_container_bad_state_tag) {
    const TempDirectory temp("bad_tag");
    const std::string path = temp.file("state.ftstate");
    RecordWriter writer(64);
    writer.text("NOT-A-STATE");
    write_file(path, wrap(writer.take()));
    expect_corrupt(path, "state.tag");
}

FT_TEST(corruption, unsupported_state_version) {
    const TempDirectory temp("state_version");
    const std::string path = temp.file("state.ftstate");
    RecordWriter writer(64);
    writer.text("FABRIC-TOPOLOGY-STATE");
    writer.u32(kPersistenceFormatVersion + 5);
    write_file(path, wrap(writer.take()));
    expect_corrupt(path, "state.version_unsupported");
}

FT_TEST(corruption, absurd_node_count) {
    const TempDirectory temp("absurd_nodes");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(0xFFFFFFFFFFFFFFFFULL);
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.node_count");
}

FT_TEST(corruption, absurd_edge_count) {
    const TempDirectory temp("absurd_edges");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(0);
    builder.writer.u64(0xFFFFFFFFFFFFFFFFULL);
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.edge_count");
}

FT_TEST(corruption, truncated_node_record) {
    const TempDirectory temp("trunc_node");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(1);
    builder.node("n-1", "ent-1", 4, 4, "dom-1");
    std::string payload = builder.writer.take();
    payload.resize(payload.size() - 6);
    write_file(path, wrap(payload));
    expect_corrupt(path, "state.node");
}

FT_TEST(corruption, unknown_enum_in_node) {
    const TempDirectory temp("bad_enum");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(1);
    builder.node("n-1", "ent-1", 250, 4, "dom-1");
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.node");
}

FT_TEST(corruption, malformed_identifier_in_node) {
    const TempDirectory temp("bad_id");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(1);
    builder.node("bad id with spaces", "ent-1", 4, 4, "dom-1");
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.node");
}

FT_TEST(corruption, duplicate_node) {
    const TempDirectory temp("dup_node");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(2);
    builder.node("n-1", "ent-1", 4, 4, "dom-1");
    builder.node("n-1", "ent-1", 4, 4, "dom-1");
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.duplicate_node");
}

FT_TEST(corruption, duplicate_edge) {
    const TempDirectory temp("dup_edge");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(2);
    builder.node("n-1", "ent-1", 4, 4, "dom-1");
    builder.node("n-2", "ent-2", 4, 4, "dom-1");
    builder.writer.u64(2);
    builder.edge("e-1", "r-1", 1, "n-1", "n-2", "dom-1");
    builder.edge("e-1", "r-2", 1, "n-1", "n-2", "dom-1");
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.duplicate_edge");
}

FT_TEST(corruption, dangling_endpoint) {
    const TempDirectory temp("dangling");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(1);
    builder.node("n-1", "ent-1", 4, 4, "dom-1");
    builder.writer.u64(1);
    builder.edge("e-1", "r-1", 1, "n-1", "n-missing", "dom-1");
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.dangling_endpoint");
}

FT_TEST(corruption, containment_cycle_encoded_on_disk) {
    const TempDirectory temp("cycle");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(3);
    for (const char* id : {"n-1", "n-2", "n-3"}) {
        builder.node(id, std::string("ent-") + id, 4, 4, "dom-1");
    }
    builder.writer.u64(3);
    // CONTAINS (3) is acyclic: n-1 -> n-2 -> n-3 -> n-1 must be refused even from disk.
    builder.edge("e-1", "r-1", 3, "n-1", "n-2", "dom-1");
    builder.edge("e-2", "r-2", 3, "n-2", "n-3", "dom-1");
    builder.edge("e-3", "r-3", 3, "n-3", "n-1", "dom-1");
    builder.writer.u64(0);  // domains
    builder.writer.u64(0);  // cross-domain rules
    builder.writer.u64(0);  // fenced worker boots
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.invariant_violation");
}

FT_TEST(corruption, trailing_bytes_are_rejected) {
    const TempDirectory temp("trailing");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(0);
    builder.writer.u64(0);
    builder.writer.u64(0);
    builder.writer.u64(0);
    builder.writer.u64(0);
    builder.writer.u8(0xFF);
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.trailing_bytes");
}

FT_TEST(corruption, unknown_domain_reference_in_rule) {
    const TempDirectory temp("bad_rule");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(0);
    builder.writer.u64(0);
    builder.writer.u64(1);
    builder.domain("dom-1");
    builder.writer.u64(1);
    builder.writer.text("dom-1");
    builder.writer.text("dom-missing");
    builder.writer.u8(1);
    builder.writer.boolean(true);
    builder.writer.u64(0);
    write_file(path, wrap(builder.writer.data()));
    expect_corrupt(path, "state.rule_domain_unknown");
}

FT_TEST(corruption, mutated_state_files_never_partially_load) {
    const TempDirectory temp("partial");
    const std::string path = temp.file("state.ftstate");
    StateBuilder builder;
    builder.writer.u64(3);
    for (const char* id : {"n-1", "n-2", "n-3"}) {
        builder.node(id, std::string("ent-") + id, 4, 4, "dom-1");
    }
    builder.writer.u64(0);
    std::string payload = builder.writer.take();

    // Every possible single-byte truncation must be rejected without leaving state behind.
    for (std::size_t length = kPersistenceHeaderBytes; length < payload.size(); length += 7) {
        write_file(path, wrap(payload.substr(0, length)));
        auto directory = std::make_shared<InMemoryEntityDirectory>();
        TopologyEngineOptions options;
        options.directory = directory;
        options.persistence_path = path;
        LoadReport report;
        const std::unique_ptr<TopologyEngine> engine = TopologyEngine::open(options, report);
        FT_CHECK(!report.loaded);
        FT_CHECK_EQ(engine->node_count(), std::size_t{0});
    }
}
