// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/ids.hpp"

namespace fabric_topology::detail {

namespace {

[[nodiscard]] constexpr bool is_alphanumeric(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool is_body_char(char c) noexcept {
    return is_alphanumeric(c) || c == '.' || c == '_' || c == '-' || c == '~' || c == ':';
}

}  // namespace

bool is_valid_identifier(std::string_view token, std::size_t max_bytes) noexcept {
    if (token.size() < kMinIdentifierBytes || token.size() > max_bytes) {
        return false;
    }
    if (!is_alphanumeric(token.front())) {
        return false;
    }
    for (char c : token) {
        if (!is_body_char(c)) {
            return false;
        }
    }
    return true;
}

}  // namespace fabric_topology::detail
