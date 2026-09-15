// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_HOST_DISCOVERY_HPP
#define FABRIC_TOPOLOGY_HOST_DISCOVERY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fabric_topology/publication.hpp"
#include "fabric_topology/registry.hpp"

namespace fabric_topology {

/// REAL, host-local attachment evidence.
///
/// A host can prove its own adapter identities, interface identities and local attachment
/// facts. It cannot prove the internal topology of an external network it cannot see, and
/// this adapter never invents such facts. Every field that could not be discovered is left
/// empty rather than guessed.
struct HostInterfaceEvidence {
    /// Stable identity for this host-scoped interface: the adapter GUID on Windows.
    std::string interface_guid;
    std::string friendly_name;
    std::string description;
    std::string mac_address;
    /// PnP device instance id (for example PCI\\VEN_8086&DEV_...). Empty when unavailable.
    std::string pnp_device_id;
    std::uint32_t interface_type = 0;   // IANA ifType
    std::uint32_t mtu = 0;
    std::vector<std::string> ipv4_addresses;
    std::vector<std::string> ipv6_addresses;
    std::vector<std::string> gateway_addresses;
    bool oper_up = false;
    bool hardware = false;
};

struct HostTopologyEvidence {
    std::string host_name;
    std::string host_entity_id;
    std::vector<HostInterfaceEvidence> interfaces;  // sorted by interface_guid
    /// Capabilities that are genuinely not discoverable from host-local information on this
    /// platform. Reported so callers never mistake absence for evidence of absence.
    std::vector<std::string> unsupported;
    /// OS/platform label used while collecting the evidence.
    std::string platform;
};

/// Result of a host discovery run: the raw evidence plus the publication and canonical
/// identities that express it.
struct HostDiscoveryResult {
    HostTopologyEvidence evidence;
    Publication publication;
    std::vector<EntityRecord> directory_entries;
};

struct HostDiscoveryOptions {
    TopologyDomainId domain;
    PublisherId publisher;
    std::string entity_prefix = "host";
};

/// Collect REAL host-local topology evidence. Never throws; returns unsupported capabilities
/// in the evidence rather than fabricating structure.
[[nodiscard]] HostTopologyEvidence discover_host_topology();

/// Convert host evidence into a SYNTHETIC-free, PublicationType::Real publication.
[[nodiscard]] HostDiscoveryResult build_host_publication(const HostTopologyEvidence& evidence,
                                                         const HostDiscoveryOptions& options);

/// True when this build/platform provides a real host discovery backend.
[[nodiscard]] bool host_discovery_supported() noexcept;

/// Human-readable description of what the host discovery backend can and cannot see.
[[nodiscard]] std::string host_discovery_capability_note();

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_HOST_DISCOVERY_HPP
