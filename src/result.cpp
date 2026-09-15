// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/result.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace fabric_topology {

namespace {

struct OutcomeName {
    Outcome outcome;
    const char* name;
};

constexpr std::array<OutcomeName, kOutcomeCount> kOutcomeNames{{
    {Outcome::Ok, "OK"},
    {Outcome::Committed, "COMMITTED"},
    {Outcome::Idempotent, "IDEMPOTENT"},
    {Outcome::StaleGeneration, "STALE_GENERATION"},
    {Outcome::StaleEntityGeneration, "STALE_ENTITY_GENERATION"},
    {Outcome::StaleAuthority, "STALE_AUTHORITY"},
    {Outcome::StaleWorkerBoot, "STALE_WORKER_BOOT"},
    {Outcome::StaleCoordinatorEpoch, "STALE_COORDINATOR_EPOCH"},
    {Outcome::UnauthorizedScope, "UNAUTHORIZED_SCOPE"},
    {Outcome::UnknownEntity, "UNKNOWN_ENTITY"},
    {Outcome::IncompatibleEntityClass, "INCOMPATIBLE_ENTITY_CLASS"},
    {Outcome::DuplicateEdge, "DUPLICATE_EDGE"},
    {Outcome::RelationshipConflict, "RELATIONSHIP_CONFLICT"},
    {Outcome::CycleRejected, "CYCLE_REJECTED"},
    {Outcome::DomainViolation, "DOMAIN_VIOLATION"},
    {Outcome::InvalidEndpoint, "INVALID_ENDPOINT"},
    {Outcome::RevalidationRequired, "REVALIDATION_REQUIRED"},
    {Outcome::Retired, "RETIRED"},
    {Outcome::Superseded, "SUPERSEDED"},
    {Outcome::MalformedRequest, "MALFORMED_REQUEST"},
    {Outcome::ResourceLimit, "RESOURCE_LIMIT"},
    {Outcome::StructuralInvariantViolation, "STRUCTURAL_INVARIANT_VIOLATION"},
    {Outcome::NotFound, "NOT_FOUND"},
    {Outcome::DuplicateIdentity, "DUPLICATE_IDENTITY"},
    {Outcome::PersistenceCorruption, "PERSISTENCE_CORRUPTION"},
    {Outcome::PersistenceIoFailure, "PERSISTENCE_IO_FAILURE"},
    {Outcome::ProtocolViolation, "PROTOCOL_VIOLATION"},
    {Outcome::UnsupportedOperation, "UNSUPPORTED_OPERATION"},
    {Outcome::Conflict, "CONFLICT"},
    {Outcome::InternalError, "INTERNAL_ERROR"},
}};

[[nodiscard]] bool field_less(const Explanation::Field& a, const Explanation::Field& b) noexcept {
    return a.key < b.key;
}

}  // namespace

const char* to_string(Outcome value) noexcept {
    for (const OutcomeName& entry : kOutcomeNames) {
        if (entry.outcome == value) {
            return entry.name;
        }
    }
    return "UNKNOWN_OUTCOME";
}

std::optional<Outcome> outcome_from_code(std::uint16_t code) noexcept {
    if (code >= kOutcomeCount) {
        return std::nullopt;
    }
    return static_cast<Outcome>(code);
}

std::optional<Outcome> outcome_from_string(std::string_view text) noexcept {
    for (const OutcomeName& entry : kOutcomeNames) {
        if (text == entry.name) {
            return entry.outcome;
        }
    }
    return std::nullopt;
}

bool outcome_is_success(Outcome value) noexcept {
    return value == Outcome::Ok || value == Outcome::Committed || value == Outcome::Idempotent;
}

Explanation::Explanation(Outcome outcome, std::string code) : outcome_(outcome), code_(std::move(code)) {}

Explanation& Explanation::set_outcome(Outcome value) noexcept {
    outcome_ = value;
    return *this;
}

Explanation& Explanation::set_code(std::string code) {
    code_ = std::move(code);
    return *this;
}

Explanation& Explanation::with(std::string key, std::string value) {
    if (value.size() > kMaxFieldValueBytes) {
        value.resize(kMaxFieldValueBytes);
    }
    const auto it = std::find_if(fields_.begin(), fields_.end(),
                                 [&key](const Field& field) { return field.key == key; });
    if (it != fields_.end()) {
        it->value = std::move(value);
        return *this;
    }
    fields_.push_back(Field{std::move(key), std::move(value)});
    std::sort(fields_.begin(), fields_.end(), field_less);
    while (fields_.size() > kMaxFields) {
        fields_.pop_back();
    }
    return *this;
}

bool Explanation::has(std::string_view key) const noexcept { return find(key) != nullptr; }

const std::string* Explanation::find(std::string_view key) const noexcept {
    for (const Field& field : fields_) {
        if (field.key == key) {
            return &field.value;
        }
    }
    return nullptr;
}

std::string Explanation::render() const {
    std::string out;
    out.reserve(64 + fields_.size() * 32);
    out += to_string(outcome_);
    if (!code_.empty()) {
        out += ' ';
        out += code_;
    }
    for (const Field& field : fields_) {
        out += '\n';
        out += "  ";
        out += field.key;
        out += '=';
        out += field.value;
    }
    return out;
}

std::string Explanation::one_line() const {
    std::string out;
    out.reserve(48 + fields_.size() * 24);
    out += to_string(outcome_);
    if (!code_.empty()) {
        out += " [";
        out += code_;
        out += ']';
    }
    for (const Field& field : fields_) {
        out += ' ';
        out += field.key;
        out += '=';
        out += field.value;
    }
    return out;
}

}  // namespace fabric_topology
