// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_LIMITS_HPP
#define FABRIC_TOPOLOGY_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace fabric_topology {

/// Explicit resource bounds applied to every externally supplied input: publications,
/// protocol frames, persistence containers and API requests. Nothing is allocated from an
/// untrusted declared count before the count is checked against these bounds.
struct Limits {
    // Identifier / string bounds
    std::size_t max_identifier_bytes = 255;
    std::size_t max_string_bytes = 1024;
    std::size_t max_metadata_entries = 32;
    std::size_t max_metadata_key_bytes = 64;
    std::size_t max_metadata_value_bytes = 256;
    std::size_t max_metadata_total_bytes = 8192;

    // Publication bounds
    std::size_t max_nodes_per_publication = 262144;
    std::size_t max_edges_per_publication = 1048576;
    std::size_t max_publication_diff_entries = 1048576;
    std::size_t max_queued_publications = 256;

    // Graph bounds
    std::size_t max_nodes = 4194304;
    std::size_t max_edges = 16777216;
    std::size_t max_degree = 1048576;
    std::size_t max_snapshot_edges = 8388608;

    // Traversal bounds
    std::size_t max_traversal_depth = 64;
    std::size_t max_traversal_visited = 1000000;

    // Persistence bounds
    std::uint64_t max_persistence_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    std::size_t max_persistence_records = 16777216;

    // Protocol bounds
    std::size_t max_frame_bytes = 4U * 1024U * 1024U;
    std::size_t max_frame_payload_bytes = 4U * 1024U * 1024U - 64U;
    std::size_t max_sessions = 1024;

    [[nodiscard]] static Limits defaults() noexcept { return Limits{}; }
};

/// Checked arithmetic helpers. Every externally influenced count is validated through these
/// before any allocation, so an oversized declared count can never drive memory growth.
[[nodiscard]] bool checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;
[[nodiscard]] bool checked_mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_LIMITS_HPP
