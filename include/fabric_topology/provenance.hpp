// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_PROVENANCE_HPP
#define FABRIC_TOPOLOGY_PROVENANCE_HPP

#include <string>

#include "fabric_topology/ids.hpp"
#include "fabric_topology/types.hpp"

namespace fabric_topology {

/// Provenance of one material topology fact. Inspectable field by field; never collapsed
/// into a single opaque source string.
struct Provenance {
    /// Publisher that first asserted the fact. Empty for facts derived from a local
    /// operator/registry declaration rather than a distributed publisher.
    PublisherId publisher;
    /// Worker incarnation that asserted the fact. A restarted publisher gets a fresh boot
    /// id; the old one stays stale forever.
    WorkerBootId worker_boot;
    /// Coordinator epoch in force when the fact was asserted.
    CoordinatorEpoch coordinator_epoch;
    /// How the fact was learned.
    DiscoverySource source = DiscoverySource::Unknown;
    /// Truthfulness classification of the underlying evidence.
    PublicationType evidence_type = PublicationType::Unsupported;
    /// Evidence stream position supplied by the publisher.
    EvidenceGeneration evidence_generation;
    /// Publisher-local identifier of the observation (for example a controller link id).
    std::string source_relationship_id;
    /// Publication that carried the fact.
    PublicationId publication;
    /// Topology generation that created the fact.
    TopologyGeneration created_generation;
    /// Topology generation of the last authoritative validation of the fact.
    TopologyGeneration last_validated_generation;

    friend bool operator==(const Provenance&, const Provenance&) = default;
};

[[nodiscard]] std::string render_provenance(const Provenance& provenance);

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_PROVENANCE_HPP
