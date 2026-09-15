// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// REAL host-local topology discovery.
//
// A host can prove its own adapters, interface identities and local attachment facts. It
// cannot prove the internal topology of a network it cannot see, and this adapter never
// invents such facts: everything it cannot prove is reported as unsupported.

#include "fabric_topology/host_discovery.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <optional>

#include "fabric_topology/ids.hpp"
#include "fabric_topology/runtime/socket.hpp"
#include "graph_state.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <iphlpapi.h>
#endif

namespace fabric_topology {

namespace {

[[nodiscard]] std::string sanitize_identifier(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 4);
    for (char c : text) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '.' || c == '_' || c == '-' || c == '~' || c == ':';
        out.push_back(ok ? c : '-');
    }
    if (out.empty()) {
        out = "unnamed";
    }
    if (!((out.front() >= '0' && out.front() <= '9') ||
          (out.front() >= 'a' && out.front() <= 'z') ||
          (out.front() >= 'A' && out.front() <= 'Z'))) {
        out.insert(out.begin(), 'x');
    }
    if (out.size() > kMaxIdentifierBytes) {
        out.resize(kMaxIdentifierBytes);
    }
    return out;
}

[[nodiscard]] bool interface_less(const HostInterfaceEvidence& a, const HostInterfaceEvidence& b) {
    return a.interface_guid < b.interface_guid;
}

#ifdef _WIN32

[[nodiscard]] std::string to_utf8(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(length - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

[[nodiscard]] std::string format_mac(const BYTE* address, ULONG length) {
    static constexpr char kDigits[] = "0123456789abcdef";
    if (address == nullptr || length == 0 || length > 32) {
        return {};
    }
    std::string out;
    out.reserve(static_cast<std::size_t>(length) * 3);
    for (ULONG i = 0; i < length; ++i) {
        if (i != 0) {
            out.push_back(':');
        }
        out.push_back(kDigits[(address[i] >> 4U) & 0x0FU]);
        out.push_back(kDigits[address[i] & 0x0FU]);
    }
    return out;
}

[[nodiscard]] std::string format_address(const sockaddr* address) {
    if (address == nullptr) {
        return {};
    }
    char text[INET6_ADDRSTRLEN] = {};
    if (address->sa_family == AF_INET) {
        const auto* inet = reinterpret_cast<const sockaddr_in*>(address);
        if (::inet_ntop(AF_INET, &inet->sin_addr, text, static_cast<socklen_t>(sizeof(text))) == nullptr) {
            return {};
        }
        return std::string(text);
    }
    if (address->sa_family == AF_INET6) {
        const auto* inet6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (::inet_ntop(AF_INET6, &inet6->sin6_addr, text, static_cast<socklen_t>(sizeof(text))) == nullptr) {
            return {};
        }
        std::string out(text);
        // Strip an IPv6 scope suffix: the scope identifier is not part of the address.
        const std::size_t percent = out.find('%');
        if (percent != std::string::npos) {
            out.resize(percent);
        }
        return out;
    }
    return {};
}

/// Map adapter GUIDs to PnP device instance ids through the network class registry key.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> enumerate_pnp_network_devices() {
    std::vector<std::pair<std::string, std::string>> result;
    HDEVINFO device_set = ::SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, nullptr, nullptr, DIGCF_PRESENT);
    if (device_set == INVALID_HANDLE_VALUE) {
        return result;
    }
    SP_DEVINFO_DATA device_info{};
    device_info.cbSize = sizeof(device_info);
    for (DWORD index = 0; ::SetupDiEnumDeviceInfo(device_set, index, &device_info); ++index) {
        HKEY key = ::SetupDiOpenDevRegKey(device_set, &device_info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) {
            continue;
        }
        wchar_t buffer[512] = {};
        DWORD buffer_bytes = sizeof(buffer);
        DWORD type = 0;
        const LONG status = ::RegQueryValueExW(key, L"NetCfgInstanceId", nullptr, &type,
                                               reinterpret_cast<LPBYTE>(buffer), &buffer_bytes);
        ::RegCloseKey(key);
        if (status != ERROR_SUCCESS || type != REG_SZ) {
            continue;
        }
        std::string instance = to_utf8(buffer);
        if (instance.empty()) {
            continue;
        }
        std::string pnp_id;
        wchar_t instance_buffer[1024] = {};
        DWORD instance_bytes = sizeof(instance_buffer);
        if (::SetupDiGetDeviceInstanceIdW(device_set, &device_info, instance_buffer,
                                          static_cast<DWORD>(std::size(instance_buffer)),
                                          &instance_bytes)) {
            pnp_id = to_utf8(instance_buffer);
        }
        result.emplace_back(std::move(instance), std::move(pnp_id));
    }
    ::SetupDiDestroyDeviceInfoList(device_set);
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] std::string host_name() {
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD length = static_cast<DWORD>(sizeof(buffer));
    if (::GetComputerNameA(buffer, &length)) {
        return std::string(buffer, static_cast<std::size_t>(length));
    }
    return "unknown-host";
}

#endif  // _WIN32

}  // namespace

bool host_discovery_supported() noexcept {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::string host_discovery_capability_note() {
#ifdef _WIN32
    return "Windows host discovery reads adapter identities (GetAdaptersAddresses), interface "
           "GUIDs, MAC addresses, MTU, operational state, assigned unicast addresses, next-hop "
           "gateway addresses and PnP device instance identifiers (SetupAPI). It observes only "
           "the local host: remote switch internals, cable peer identity, patch-panel mapping, "
           "optical/InfiniBand/RoCE/NVLink fabric structure and multi-site topology are not "
           "discoverable from host-local information and are reported as unsupported.";
#else
    return "No host discovery backend is available for this platform in this build.";
#endif
}

HostTopologyEvidence discover_host_topology() {
    HostTopologyEvidence evidence;
    evidence.platform =
#ifdef _WIN32
        "windows";
#else
        "unsupported";
#endif

#ifdef _WIN32
    const Status networking = runtime::initialize_networking();
    if (networking.outcome() != Outcome::Ok) {
        evidence.unsupported.push_back("networking_unavailable");
    }
    evidence.host_name = host_name();

    ULONG size = 0;
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    if (::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        evidence.unsupported.push_back("adapter_enumeration_failed");
        return evidence;
    }
    std::vector<unsigned char> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size) != NO_ERROR) {
        evidence.unsupported.push_back("adapter_enumeration_failed");
        return evidence;
    }

    const std::vector<std::pair<std::string, std::string>> pnp_devices = enumerate_pnp_network_devices();

    for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        HostInterfaceEvidence interface;
        interface.interface_guid = sanitize_identifier(adapter->AdapterName);
        interface.friendly_name = to_utf8(adapter->FriendlyName);
        interface.description = to_utf8(adapter->Description);
        interface.mac_address = format_mac(adapter->PhysicalAddress, adapter->PhysicalAddressLength);
        interface.interface_type = adapter->IfType;
        interface.mtu = adapter->Mtu;
        interface.oper_up = adapter->OperStatus == IfOperStatusUp;
        interface.hardware = adapter->IfType == IF_TYPE_ETHERNET_CSMACD ||
                             adapter->IfType == IF_TYPE_IEEE80211 || adapter->IfType == IF_TYPE_IEEE80216_WMAN;

        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const std::string text = format_address(unicast->Address.lpSockaddr);
            if (text.empty()) {
                continue;
            }
            if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                interface.ipv4_addresses.push_back(text);
            } else {
                interface.ipv6_addresses.push_back(text);
            }
        }
        for (auto* gateway = adapter->FirstGatewayAddress; gateway != nullptr; gateway = gateway->Next) {
            const std::string text = format_address(gateway->Address.lpSockaddr);
            if (!text.empty() &&
                std::find(interface.gateway_addresses.begin(), interface.gateway_addresses.end(), text) ==
                    interface.gateway_addresses.end()) {
                interface.gateway_addresses.push_back(text);
            }
        }
        std::sort(interface.ipv4_addresses.begin(), interface.ipv4_addresses.end());
        std::sort(interface.ipv6_addresses.begin(), interface.ipv6_addresses.end());
        std::sort(interface.gateway_addresses.begin(), interface.gateway_addresses.end());

        const std::string raw_guid = adapter->AdapterName;
        for (const auto& entry : pnp_devices) {
            if (entry.first == raw_guid) {
                interface.pnp_device_id = entry.second;
                break;
            }
        }
        evidence.interfaces.push_back(std::move(interface));
    }
    std::sort(evidence.interfaces.begin(), evidence.interfaces.end(), interface_less);

    evidence.unsupported.push_back("remote_switch_internal_topology");
    evidence.unsupported.push_back("cable_peer_identity");
    evidence.unsupported.push_back("external_patch_panel_mapping");
    evidence.unsupported.push_back("optical_or_infiniband_fabric_structure");
    evidence.unsupported.push_back("multi_site_topology");
#else
    evidence.host_name = "unknown-host";
    evidence.unsupported.push_back("host_discovery_backend_unavailable");
#endif

    std::sort(evidence.unsupported.begin(), evidence.unsupported.end());
    return evidence;
}

HostDiscoveryResult build_host_publication(const HostTopologyEvidence& evidence,
                                           const HostDiscoveryOptions& options) {
    HostDiscoveryResult result;
    result.evidence = evidence;

    const std::string prefix = options.entity_prefix.empty() ? std::string("host") : options.entity_prefix;
    const std::string host_key = sanitize_identifier(evidence.host_name.empty() ? "unknown-host"
                                                                               : evidence.host_name);
    result.evidence.host_entity_id = prefix + "-" + host_key;

    Publication& publication = result.publication;
    publication.id = PublicationId::from_trusted(prefix + "-publication");
    publication.domain = options.domain;
    publication.mode = PublicationMode::AuthoritativeSnapshot;
    publication.type = PublicationType::Real;
    publication.source = DiscoverySource::HostDiscovery;
    publication.evidence_generation = EvidenceGeneration{1};
    publication.note = "host-local attachment evidence discovered on " + evidence.platform;

    auto add_entity = [&result](const std::string& entity_id, EntityClass entity_class) {
        EntityRecord record;
        record.entity_id = entity_id;
        record.entity_class = entity_class;
        record.generation = EntityGeneration{1};
        result.directory_entries.push_back(std::move(record));
    };

    auto add_node = [&](const std::string& entity_id, NodeClass node_class, TopologyTier tier) {
        add_entity(entity_id, node_class_entity_class(node_class));
        ObservedNode node;
        node.entity_id = entity_id;
        node.node_class = node_class;
        node.tier = tier;
        node.domain = options.domain;
        node.entity_generation = EntityGeneration{1};
        node.node_id = internal::derive_node_id(options.domain, entity_id);
        publication.nodes.push_back(std::move(node));
        return publication.nodes.back().node_id;
    };

    auto add_edge = [&](RelationClass relation, const TopologyNodeId& from, const TopologyNodeId& to,
                        std::optional<TopologyLayer> layer, const std::string& evidence_class) {
        ObservedEdge edge;
        edge.relation = relation;
        edge.from = from;
        edge.to = to;
        edge.layer = layer;
        edge.domain = options.domain;
        edge.evidence_generation = EvidenceGeneration{1};
        edge.source_relationship_id = evidence_class;
        edge.relationship_id = RelationshipId::from_trusted(
            internal::derive_relationship_id(make_edge_key(relation, from, to, options.domain)));
        publication.edges.push_back(std::move(edge));
    };

    std::size_t index = 0;
    for (const HostInterfaceEvidence& interface : evidence.interfaces) {
        ++index;
        const std::string suffix = interface.interface_guid.empty()
                                       ? std::to_string(index)
                                       : interface.interface_guid;
        const std::string nic_entity = prefix + "-nic-" + suffix;
        const std::string port_entity = prefix + "-port-" + suffix;
        const TopologyNodeId nic = add_node(nic_entity, NodeClass::Nic, TopologyTier::Endpoint);
        const TopologyNodeId port =
            add_node(port_entity, NodeClass::PhysicalPort, TopologyTier::Endpoint);
        add_edge(RelationClass::PresentsEndpoint, nic, port,
                 std::optional<TopologyLayer>{TopologyLayer::Physical}, "adapter_presents_port");

        std::size_t address_index = 0;
        for (const std::string& address : interface.ipv4_addresses) {
            ++address_index;
            const std::string address_entity =
                prefix + "-ip-" + suffix + "-" + std::to_string(address_index);
            const TopologyNodeId endpoint =
                add_node(address_entity, NodeClass::LogicalEndpoint, TopologyTier::Endpoint);
            add_edge(RelationClass::PresentsEndpoint, port, endpoint,
                     std::optional<TopologyLayer>{TopologyLayer::Physical}, "interface_address:" + address);
        }
        for (const std::string& address : interface.ipv6_addresses) {
            ++address_index;
            const std::string address_entity =
                prefix + "-ip6-" + suffix + "-" + std::to_string(address_index);
            const TopologyNodeId endpoint =
                add_node(address_entity, NodeClass::LogicalEndpoint, TopologyTier::Endpoint);
            add_edge(RelationClass::PresentsEndpoint, port, endpoint,
                     std::optional<TopologyLayer>{TopologyLayer::Physical}, "interface_address:" + address);
        }
        for (const std::string& gateway : interface.gateway_addresses) {
            const std::string gateway_entity = prefix + "-next-hop-" + sanitize_identifier(gateway);
            const TopologyNodeId next_hop =
                add_node(gateway_entity, NodeClass::FabricEndpoint, TopologyTier::Gateway);
            add_edge(RelationClass::ConnectedTo, port, next_hop, std::nullopt,
                     "host_route_next_hop:" + gateway);
        }
    }

    return result;
}

}  // namespace fabric_topology
