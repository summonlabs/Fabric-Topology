// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_DIGEST_HPP
#define FABRIC_TOPOLOGY_DIGEST_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace fabric_topology {

/// SHA-256 over the supplied bytes, rendered as 64 lowercase hex characters.
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

/// Raw 32-byte SHA-256 digest.
[[nodiscard]] std::string sha256_raw(std::string_view bytes);

/// CRC-32 (IEEE 802.3 polynomial, reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF).
[[nodiscard]] std::uint32_t crc32(std::string_view bytes) noexcept;

/// FNV-1a 64-bit hash, used for stable non-cryptographic bucket keys.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view bytes) noexcept;

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_DIGEST_HPP
