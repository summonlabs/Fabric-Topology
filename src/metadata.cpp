// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/metadata.hpp"

#include <algorithm>
#include <cstdint>

namespace fabric_topology {

namespace {

[[nodiscard]] bool has_control_bytes(std::string_view text) noexcept {
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20U || byte == 0x7FU) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool item_less(const Metadata::Item& a, const Metadata::Item& b) noexcept {
    return a.first < b.first;
}

}  // namespace

Status Metadata::validate(const std::vector<Item>& items, const Limits& limits) {
    Status status;
    if (items.size() > limits.max_metadata_entries) {
        status.set_outcome(Outcome::ResourceLimit).set_code("metadata.too_many_entries");
        status.with("entries", std::to_string(items.size()));
        status.with("limit", std::to_string(limits.max_metadata_entries));
        return status;
    }
    std::uint64_t total = 0;
    std::vector<Item> sorted(items.begin(), items.end());
    std::sort(sorted.begin(), sorted.end(), item_less);
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const Item& item = sorted[i];
        if (item.first.empty() || item.first.size() > limits.max_metadata_key_bytes) {
            status.set_outcome(Outcome::MalformedRequest).set_code("metadata.invalid_key");
            status.with("index", std::to_string(i));
            return status;
        }
        if (item.second.size() > limits.max_metadata_value_bytes) {
            status.set_outcome(Outcome::ResourceLimit).set_code("metadata.value_too_large");
            status.with("index", std::to_string(i));
            status.with("bytes", std::to_string(item.second.size()));
            status.with("limit", std::to_string(limits.max_metadata_value_bytes));
            return status;
        }
        if (has_control_bytes(item.first) || has_control_bytes(item.second)) {
            status.set_outcome(Outcome::MalformedRequest).set_code("metadata.control_byte");
            status.with("index", std::to_string(i));
            return status;
        }
        if (i > 0 && sorted[i - 1].first == item.first) {
            status.set_outcome(Outcome::DuplicateIdentity).set_code("metadata.duplicate_key");
            status.with("key", item.first);
            return status;
        }
        total += item.first.size();
        total += item.second.size();
    }
    if (total > limits.max_metadata_total_bytes) {
        status.set_outcome(Outcome::ResourceLimit).set_code("metadata.total_too_large");
        status.with("bytes", std::to_string(total));
        status.with("limit", std::to_string(limits.max_metadata_total_bytes));
        return status;
    }
    status.set_outcome(Outcome::Ok).set_code("metadata.ok");
    return status;
}

Status Metadata::set(std::string_view key, std::string_view value, const Limits& limits) {
    if (key.empty() || key.size() > limits.max_metadata_key_bytes) {
        Status status;
        status.set_outcome(Outcome::MalformedRequest).set_code("metadata.invalid_key");
        status.with("key_bytes", std::to_string(key.size()));
        status.with("limit", std::to_string(limits.max_metadata_key_bytes));
        return status;
    }
    if (value.size() > limits.max_metadata_value_bytes) {
        Status status;
        status.set_outcome(Outcome::ResourceLimit).set_code("metadata.value_too_large");
        status.with("key", std::string(key));
        status.with("bytes", std::to_string(value.size()));
        status.with("limit", std::to_string(limits.max_metadata_value_bytes));
        return status;
    }
    if (has_control_bytes(key) || has_control_bytes(value)) {
        Status status;
        status.set_outcome(Outcome::MalformedRequest).set_code("metadata.control_byte");
        status.with("key", std::string(key));
        return status;
    }

    const auto it = std::find_if(items_.begin(), items_.end(),
                                 [key](const Item& item) { return item.first == key; });
    if (it != items_.end()) {
        it->second.assign(value.data(), value.size());
    } else {
        if (items_.size() >= limits.max_metadata_entries) {
            Status status;
            status.set_outcome(Outcome::ResourceLimit).set_code("metadata.too_many_entries");
            status.with("limit", std::to_string(limits.max_metadata_entries));
            return status;
        }
        items_.emplace_back(std::string(key), std::string(value));
        std::sort(items_.begin(), items_.end(), item_less);
    }

    if (total_bytes() > limits.max_metadata_total_bytes) {
        if (it != items_.end()) {
            it->second.clear();
            items_.erase(it);
        } else {
            const auto added = std::find_if(items_.begin(), items_.end(),
                                            [key](const Item& item) { return item.first == key; });
            if (added != items_.end()) {
                items_.erase(added);
            }
        }
        Status status;
        status.set_outcome(Outcome::ResourceLimit).set_code("metadata.total_too_large");
        status.with("limit", std::to_string(limits.max_metadata_total_bytes));
        return status;
    }

    Status status;
    status.set_outcome(Outcome::Ok).set_code("metadata.ok");
    return status;
}

Status Metadata::erase(std::string_view key) {
    Status status;
    const auto it = std::find_if(items_.begin(), items_.end(),
                                 [key](const Item& item) { return item.first == key; });
    if (it == items_.end()) {
        status.set_outcome(Outcome::NotFound).set_code("metadata.key_absent");
        status.with("key", std::string(key));
        return status;
    }
    items_.erase(it);
    status.set_outcome(Outcome::Committed).set_code("metadata.key_erased");
    return status;
}

const std::string* Metadata::find(std::string_view key) const noexcept {
    for (const Item& item : items_) {
        if (item.first == key) {
            return &item.second;
        }
    }
    return nullptr;
}

std::size_t Metadata::total_bytes() const noexcept {
    std::size_t total = 0;
    for (const Item& item : items_) {
        total += item.first.size();
        total += item.second.size();
    }
    return total;
}

std::string Metadata::render() const {
    std::string out;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += items_[i].first;
        out += '=';
        out += items_[i].second;
    }
    return out;
}

}  // namespace fabric_topology
