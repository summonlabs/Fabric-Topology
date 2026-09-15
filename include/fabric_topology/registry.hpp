// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_REGISTRY_HPP
#define FABRIC_TOPOLOGY_REGISTRY_HPP

#include <cstddef>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fabric_topology/ids.hpp"
#include "fabric_topology/result.hpp"
#include "fabric_topology/types.hpp"

namespace fabric_topology {

/// A canonical entity as published by Fabric Registry. Fabric Topology consumes this record;
/// it never creates, renames or re-keys canonical identity.
struct EntityRecord {
    std::string entity_id;
    EntityClass entity_class = EntityClass::Unknown;
    EntityGeneration generation;
    bool retired = false;
    /// True when Fabric Registry has superseded this identity (for example a replaced device).
    bool superseded = false;
    /// Canonical identifier of the successor identity, when superseded is true.
    std::string superseded_by;

    friend bool operator==(const EntityRecord&, const EntityRecord&) = default;
};

/// The dependency seam onto Fabric Registry. Implementations answer: does this canonical
/// identity exist, of which class, at which entity generation, and is it retired/superseded?
///
/// Fabric Topology uses this only to validate references. It is not, and must not become,
/// the owner of canonical identity.
class IEntityDirectory {
public:
    virtual ~IEntityDirectory() = default;

    [[nodiscard]] virtual std::optional<EntityRecord> lookup(std::string_view entity_id) const = 0;

    /// Optional enumeration, used by host/registry adapters. The default is empty.
    [[nodiscard]] virtual std::vector<EntityRecord> list(EntityClass /*entity_class*/) const { return {}; }
};

/// Reference adapter that keeps a canonical-identity table in memory. Consumers that own a
/// real Fabric Registry link supply their own IEntityDirectory; this adapter exists for
/// embedders, tests and the shipped tools, and it is thread-safe.
class InMemoryEntityDirectory final : public IEntityDirectory {
public:
    InMemoryEntityDirectory() = default;

    /// Insert a new canonical identity. Duplicate identifiers are rejected.
    Status add(EntityRecord record);

    /// Replace old_id with successor: the old record becomes superseded and points at the
    /// successor, the successor is inserted. This is the registry-side device replacement
    /// operation that Fabric Topology must fence against.
    Status supersede(std::string_view old_id, EntityRecord successor);

    /// Mark an identity retired. Retired identities stay resolvable so topology can explain
    /// why it refuses to reference them.
    Status retire(std::string_view entity_id);

    /// Advance an identity's generation in place (same identity, new incarnation).
    Status bump_generation(std::string_view entity_id, EntityGeneration generation);

    [[nodiscard]] std::optional<EntityRecord> lookup(std::string_view entity_id) const override;
    [[nodiscard]] std::vector<EntityRecord> list(EntityClass entity_class) const override;

    [[nodiscard]] std::size_t size() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, EntityRecord> records_;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_REGISTRY_HPP
