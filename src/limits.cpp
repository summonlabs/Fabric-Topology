// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/limits.hpp"

#include <limits>

namespace fabric_topology {

bool checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a > (std::numeric_limits<std::uint64_t>::max)() - b) {
        return false;
    }
    out = a + b;
    return true;
}

bool checked_mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a != 0 && b > (std::numeric_limits<std::uint64_t>::max)() / a) {
        return false;
    }
    out = a * b;
    return true;
}

}  // namespace fabric_topology
