// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/version.hpp"

namespace fabric_topology {

std::string version_string() { return std::string(kVersionString); }

std::string product_banner() {
    std::string banner;
    banner.reserve(64);
    banner += kProductName;
    banner += ' ';
    banner += kVersionString;
    banner += " (";
    banner += kVendorName;
    banner += ')';
    return banner;
}

}  // namespace fabric_topology
