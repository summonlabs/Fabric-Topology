// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// Reference adapter over Fabric Registry identities. Fabric Topology consumes this
// dependency; it never owns canonical identity.

#include "fabric_topology/registry.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

namespace fabric_topology {

namespace {

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] bool record_less(const EntityRecord& a, const EntityRecord& b) {
    return a.entity_id < b.entity_id;
}

}  // namespace

Status InMemoryEntityDirectory::add(EntityRecord record) {
    if (record.entity_id.empty() ||
        !detail::is_valid_identifier(record.entity_id, kMaxIdentifierBytes)) {
        Status status = make_status(Outcome::MalformedRequest, "directory.invalid_identifier");
        status.with("entity_id", record.entity_id);
        return status;
    }
    if (record.entity_class == EntityClass::Unknown) {
        Status status = make_status(Outcome::MalformedRequest, "directory.unknown_class");
        status.with("entity_id", record.entity_id);
        return status;
    }
    if (record.generation.is_zero()) {
        Status status = make_status(Outcome::MalformedRequest, "directory.zero_generation");
        status.with("entity_id", record.entity_id);
        return status;
    }

    std::unique_lock lock(mutex_);
    if (records_.find(record.entity_id) != records_.end()) {
        Status status = make_status(Outcome::DuplicateIdentity, "directory.duplicate_identity");
        status.with("entity_id", record.entity_id);
        return status;
    }
    const std::string id = record.entity_id;
    records_.emplace(id, std::move(record));
    Status status = make_status(Outcome::Committed, "directory.added");
    status.with("entity_id", id);
    return status;
}

Status InMemoryEntityDirectory::supersede(std::string_view old_id, EntityRecord successor) {
    const std::string old_key(old_id);
    std::unique_lock lock(mutex_);
    const auto it = records_.find(old_key);
    if (it == records_.end()) {
        Status status = make_status(Outcome::UnknownEntity, "directory.predecessor_unknown");
        status.with("entity_id", old_key);
        return status;
    }
    if (successor.entity_id.empty() ||
        !detail::is_valid_identifier(successor.entity_id, kMaxIdentifierBytes)) {
        return make_status(Outcome::MalformedRequest, "directory.invalid_successor");
    }
    if (successor.entity_class == EntityClass::Unknown || successor.generation.is_zero()) {
        return make_status(Outcome::MalformedRequest, "directory.invalid_successor");
    }
    if (records_.find(successor.entity_id) != records_.end()) {
        Status status = make_status(Outcome::DuplicateIdentity, "directory.successor_exists");
        status.with("entity_id", successor.entity_id);
        return status;
    }
    it->second.superseded = true;
    it->second.superseded_by = successor.entity_id;
    const std::string successor_id = successor.entity_id;
    records_.emplace(successor_id, std::move(successor));
    Status status = make_status(Outcome::Committed, "directory.superseded");
    status.with("superseded", old_key);
    status.with("successor", successor_id);
    return status;
}

Status InMemoryEntityDirectory::retire(std::string_view entity_id) {
    const std::string key(entity_id);
    std::unique_lock lock(mutex_);
    const auto it = records_.find(key);
    if (it == records_.end()) {
        Status status = make_status(Outcome::UnknownEntity, "directory.unknown");
        status.with("entity_id", key);
        return status;
    }
    if (it->second.retired) {
        return make_status(Outcome::Idempotent, "directory.already_retired");
    }
    it->second.retired = true;
    Status status = make_status(Outcome::Committed, "directory.retired");
    status.with("entity_id", key);
    return status;
}

Status InMemoryEntityDirectory::bump_generation(std::string_view entity_id,
                                                EntityGeneration generation) {
    const std::string key(entity_id);
    std::unique_lock lock(mutex_);
    const auto it = records_.find(key);
    if (it == records_.end()) {
        Status status = make_status(Outcome::UnknownEntity, "directory.unknown");
        status.with("entity_id", key);
        return status;
    }
    if (generation == it->second.generation) {
        return make_status(Outcome::Idempotent, "directory.generation_unchanged");
    }
    if (generation < it->second.generation) {
        Status status = make_status(Outcome::StaleEntityGeneration, "directory.generation_rollback");
        status.with("entity_id", key);
        status.with("current", it->second.generation.to_string());
        status.with("requested", generation.to_string());
        return status;
    }
    it->second.generation = generation;
    Status status = make_status(Outcome::Committed, "directory.generation_advanced");
    status.with("entity_id", key);
    status.with("generation", generation.to_string());
    return status;
}

std::optional<EntityRecord> InMemoryEntityDirectory::lookup(std::string_view entity_id) const {
    std::shared_lock lock(mutex_);
    const auto it = records_.find(std::string(entity_id));
    if (it == records_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<EntityRecord> InMemoryEntityDirectory::list(EntityClass entity_class) const {
    std::shared_lock lock(mutex_);
    std::vector<EntityRecord> result;
    for (const auto& entry : records_) {
        if (entry.second.entity_class == entity_class) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(), record_less);
    return result;
}

std::size_t InMemoryEntityDirectory::size() const {
    std::shared_lock lock(mutex_);
    return records_.size();
}

}  // namespace fabric_topology
