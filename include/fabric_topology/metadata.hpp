// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_METADATA_HPP
#define FABRIC_TOPOLOGY_METADATA_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fabric_topology/limits.hpp"
#include "fabric_topology/result.hpp"

namespace fabric_topology {

/// Bounded, sorted, deterministic key/value metadata attached to nodes and relationships.
/// Not an opaque source string: provenance is modelled separately and explicitly.
class Metadata {
public:
    using Item = std::pair<std::string, std::string>;

    Metadata() = default;

    /// Insert or replace one key. Rejects keys and values that exceed the configured bounds
    /// or contain control bytes / newlines, which would break deterministic rendering.
    Status set(std::string_view key, std::string_view value, const Limits& limits = Limits{});

    Status erase(std::string_view key);

    [[nodiscard]] const std::string* find(std::string_view key) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] const std::vector<Item>& items() const noexcept { return items_; }

    /// Total encoded size of keys and values.
    [[nodiscard]] std::size_t total_bytes() const noexcept;

    /// Validate a whole item list against the limits without constructing a Metadata.
    [[nodiscard]] static Status validate(const std::vector<Item>& items, const Limits& limits);

    [[nodiscard]] std::string render() const;

    friend bool operator==(const Metadata& a, const Metadata& b) noexcept { return a.items_ == b.items_; }

private:
    std::vector<Item> items_;  // kept sorted by key
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_METADATA_HPP
