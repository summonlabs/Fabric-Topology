// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_PERSISTENCE_HPP
#define FABRIC_TOPOLOGY_PERSISTENCE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "fabric_topology/ids.hpp"
#include "fabric_topology/result.hpp"

namespace fabric_topology {

/// When durable topology is written.
enum class PersistenceMode : std::uint8_t {
    /// Only explicit save() calls touch the file. Default.
    Manual = 0,
    /// Every committed mutation rewrites the durable image transactionally. A failed write
    /// rolls the in-memory mutation back, so memory and disk never disagree.
    Immediate = 1,
};

[[nodiscard]] const char* to_string(PersistenceMode value) noexcept;

/// Persistence container layout version.
inline constexpr std::uint32_t kPersistenceFormatVersion = 1;
/// Encoded container header, all little-endian:
///   magic_blob(4 length prefix + 8 magic) | version(4) | payload_bytes(8) |
///   payload_crc32(4) | sha256_blob(4 length prefix + 32 digest)
inline constexpr std::size_t kPersistenceHeaderBytes = 4 + 8 + 4 + 8 + 4 + 4 + 32;

/// Header of a persistence container, readable without decoding the payload.
struct PersistenceHeaderInfo {
    std::uint32_t format_version = 0;
    std::uint64_t payload_bytes = 0;
    std::uint32_t payload_crc32 = 0;
    std::string payload_sha256;
    std::uint64_t file_bytes = 0;
};

/// Read and integrity-check only the container header and trailer. Used by the inspector and
/// by corruption tests. Never allocates the payload.
[[nodiscard]] Status inspect_persistence_file(const std::string& path, PersistenceHeaderInfo& out);

/// Result of conservatively recovering durable topology.
struct LoadReport {
    Explanation status;
    bool loaded = false;
    bool recovered = false;
    TopologyGeneration generation;
    CoordinatorEpoch coordinator_epoch;
    std::size_t nodes = 0;
    std::size_t edges = 0;
    std::size_t revalidation_required_edges = 0;
    std::size_t revalidation_required_nodes = 0;
    std::size_t retired_edges = 0;
    std::size_t superseded_edges = 0;

    [[nodiscard]] std::string render() const;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_PERSISTENCE_HPP
