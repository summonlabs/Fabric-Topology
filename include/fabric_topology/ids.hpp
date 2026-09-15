// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_IDS_HPP
#define FABRIC_TOPOLOGY_IDS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace fabric_topology {

/// Bytes allowed in a canonical identifier body: [A-Za-z0-9] plus '.', '_', '-', '~', ':'.
/// The first byte must be alphanumeric. Nothing else is accepted, so identifier text is
/// always safe to embed in canonical encodings, digests, logs and file names.
inline constexpr std::size_t kMaxIdentifierBytes = 255;
inline constexpr std::size_t kMinIdentifierBytes = 1;

namespace detail {
[[nodiscard]] bool is_valid_identifier(std::string_view token, std::size_t max_bytes) noexcept;
}  // namespace detail

/// Strongly typed identifier. Each identity class gets its own tag, so identifiers of
/// different classes are distinct types and cannot be substituted for one another.
///
/// Ordering is byte-lexicographic over the canonical text, which makes every ordering in
/// this library deterministic and independent of insertion order.
template <class Tag>
class StrongId {
public:
    using tag_type = Tag;

    StrongId() = default;

    /// Parse a canonical identifier. Returns nullopt when the text is empty, too long, or
    /// contains a byte outside the canonical alphabet.
    [[nodiscard]] static std::optional<StrongId> parse(std::string_view token) noexcept {
        return parse(token, kMaxIdentifierBytes);
    }

    [[nodiscard]] static std::optional<StrongId> parse(std::string_view token,
                                                       std::size_t max_bytes) noexcept {
        if (!detail::is_valid_identifier(token, max_bytes)) {
            return std::nullopt;
        }
        StrongId result;
        result.value_.assign(token.data(), token.size());
        return result;
    }

    /// Build from text that has already been validated by the caller. The library only uses
    /// this internally, or when the text was produced by encode()/to_string().
    [[nodiscard]] static StrongId from_trusted(std::string token) {
        StrongId result;
        result.value_ = std::move(token);
        return result;
    }

    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    [[nodiscard]] std::string to_string() const { return value_; }

    friend bool operator==(const StrongId& a, const StrongId& b) noexcept { return a.value_ == b.value_; }
    friend bool operator!=(const StrongId& a, const StrongId& b) noexcept { return !(a == b); }
    friend bool operator<(const StrongId& a, const StrongId& b) noexcept { return a.value_ < b.value_; }
    friend bool operator>(const StrongId& a, const StrongId& b) noexcept { return b < a; }
    friend bool operator<=(const StrongId& a, const StrongId& b) noexcept { return !(b < a); }
    friend bool operator>=(const StrongId& a, const StrongId& b) noexcept { return !(a < b); }

    [[nodiscard]] std::size_t hash() const noexcept { return std::hash<std::string>{}(value_); }

private:
    std::string value_;
};

#define FABRIC_TOPOLOGY_DECLARE_ID(NAME)      \
    struct NAME##Tag {};                      \
    using NAME = StrongId<NAME##Tag>

FABRIC_TOPOLOGY_DECLARE_ID(TopologyId);
FABRIC_TOPOLOGY_DECLARE_ID(TopologyNodeId);
FABRIC_TOPOLOGY_DECLARE_ID(TopologyEdgeId);
FABRIC_TOPOLOGY_DECLARE_ID(TopologySnapshotId);
FABRIC_TOPOLOGY_DECLARE_ID(TopologyDomainId);
FABRIC_TOPOLOGY_DECLARE_ID(AttachmentId);
FABRIC_TOPOLOGY_DECLARE_ID(RelationshipId);
FABRIC_TOPOLOGY_DECLARE_ID(PublicationId);
FABRIC_TOPOLOGY_DECLARE_ID(PublisherId);
FABRIC_TOPOLOGY_DECLARE_ID(WorkerBootId);
FABRIC_TOPOLOGY_DECLARE_ID(MutationAttemptId);

#undef FABRIC_TOPOLOGY_DECLARE_ID

/// Monotonic counter with a distinct type per generation domain. Generation kinds are never
/// interchangeable: a topology generation cannot be passed where an evidence generation is
/// expected, and so on.
template <class Tag>
class Counter {
public:
    using value_type = std::uint64_t;

    constexpr Counter() noexcept = default;
    constexpr explicit Counter(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
    [[nodiscard]] constexpr bool can_advance() const noexcept {
        return value_ != (std::numeric_limits<std::uint64_t>::max)();
    }
    /// Advance by one. Callers must have checked can_advance(); the library checks before use.
    [[nodiscard]] constexpr Counter next() const noexcept { return Counter(value_ + 1U); }

    friend constexpr bool operator==(Counter a, Counter b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Counter a, Counter b) noexcept { return !(a == b); }
    friend constexpr auto operator<=>(Counter a, Counter b) noexcept { return a.value_ <=> b.value_; }

    /// Deterministic decimal rendering.
    [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

private:
    std::uint64_t value_ = 0;
};

#define FABRIC_TOPOLOGY_DECLARE_COUNTER(NAME) \
    struct NAME##Tag {};                      \
    using NAME = Counter<NAME##Tag>

FABRIC_TOPOLOGY_DECLARE_COUNTER(TopologyGeneration);
FABRIC_TOPOLOGY_DECLARE_COUNTER(NodeGeneration);
FABRIC_TOPOLOGY_DECLARE_COUNTER(EdgeGeneration);
FABRIC_TOPOLOGY_DECLARE_COUNTER(SnapshotGeneration);
FABRIC_TOPOLOGY_DECLARE_COUNTER(EvidenceGeneration);
FABRIC_TOPOLOGY_DECLARE_COUNTER(EntityGeneration);
FABRIC_TOPOLOGY_DECLARE_COUNTER(CoordinatorEpoch);

#undef FABRIC_TOPOLOGY_DECLARE_COUNTER

}  // namespace fabric_topology

namespace std {
template <class Tag>
struct hash<fabric_topology::StrongId<Tag>> {
    std::size_t operator()(const fabric_topology::StrongId<Tag>& value) const noexcept {
        return value.hash();
    }
};
}  // namespace std

#endif  // FABRIC_TOPOLOGY_IDS_HPP
