// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_RESULT_HPP
#define FABRIC_TOPOLOGY_RESULT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fabric_topology {

/// Structured, non-collapsing operation outcome. Distinct failure causes keep distinct
/// values: a caller can always tell malformed input from stale authority from a graph
/// invariant violation. Numeric values are stable and are used on the wire and on disk.
enum class Outcome : std::uint16_t {
    Ok = 0,
    Committed = 1,
    Idempotent = 2,
    StaleGeneration = 3,
    StaleEntityGeneration = 4,
    StaleAuthority = 5,
    StaleWorkerBoot = 6,
    StaleCoordinatorEpoch = 7,
    UnauthorizedScope = 8,
    UnknownEntity = 9,
    IncompatibleEntityClass = 10,
    DuplicateEdge = 11,
    RelationshipConflict = 12,
    CycleRejected = 13,
    DomainViolation = 14,
    InvalidEndpoint = 15,
    RevalidationRequired = 16,
    Retired = 17,
    Superseded = 18,
    MalformedRequest = 19,
    ResourceLimit = 20,
    StructuralInvariantViolation = 21,
    NotFound = 22,
    DuplicateIdentity = 23,
    PersistenceCorruption = 24,
    PersistenceIoFailure = 25,
    ProtocolViolation = 26,
    UnsupportedOperation = 27,
    Conflict = 28,
    InternalError = 29,
};

inline constexpr std::uint16_t kOutcomeCount = 30;

[[nodiscard]] const char* to_string(Outcome value) noexcept;
[[nodiscard]] std::optional<Outcome> outcome_from_code(std::uint16_t code) noexcept;
[[nodiscard]] std::optional<Outcome> outcome_from_string(std::string_view text) noexcept;
[[nodiscard]] bool outcome_is_success(Outcome value) noexcept;

/// Structured, deterministic explanation of a topology decision.
///
/// Fields are kept sorted by key and deduplicated, so rendering is byte-identical for
/// identical decisions regardless of the order in which the decision path added fields.
class Explanation {
public:
    struct Field {
        std::string key;
        std::string value;

        friend bool operator==(const Field& a, const Field& b) noexcept {
            return a.key == b.key && a.value == b.value;
        }
        friend bool operator<(const Field& a, const Field& b) noexcept { return a.key < b.key; }
    };

    static constexpr std::size_t kMaxFields = 32;
    static constexpr std::size_t kMaxFieldValueBytes = 1024;

    Explanation() = default;
    Explanation(Outcome outcome, std::string code);

    [[nodiscard]] Outcome outcome() const noexcept { return outcome_; }
    [[nodiscard]] const std::string& code() const noexcept { return code_; }
    [[nodiscard]] const std::vector<Field>& fields() const noexcept { return fields_; }

    Explanation& set_outcome(Outcome value) noexcept;
    Explanation& set_code(std::string code);

    /// Add or replace a field. Values are truncated to kMaxFieldValueBytes; the field list is
    /// capped at kMaxFields (keys beyond the cap are dropped deterministically by key order).
    Explanation& with(std::string key, std::string value);

    [[nodiscard]] bool has(std::string_view key) const noexcept;
    [[nodiscard]] const std::string* find(std::string_view key) const noexcept;

    /// Deterministic multi-line rendering: "outcome code" followed by sorted key=value lines.
    [[nodiscard]] std::string render() const;
    /// Deterministic single-line rendering suitable for logs and CLI output.
    [[nodiscard]] std::string one_line() const;

    friend bool operator==(const Explanation& a, const Explanation& b) noexcept {
        return a.outcome_ == b.outcome_ && a.code_ == b.code_ && a.fields_ == b.fields_;
    }

private:
    Outcome outcome_ = Outcome::Ok;
    std::string code_;
    std::vector<Field> fields_;
};

using Status = Explanation;

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_RESULT_HPP
