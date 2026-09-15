// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace fabric_topology {

// ---------------------------------------------------------------------------
// RecordWriter
// ---------------------------------------------------------------------------

void RecordWriter::u8(std::uint8_t value) { data_.push_back(static_cast<char>(value)); }

void RecordWriter::u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xFFU));
    u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void RecordWriter::u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void RecordWriter::u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void RecordWriter::blob(std::string_view bytes) {
    u32(static_cast<std::uint32_t>(bytes.size()));
    data_.append(bytes.data(), bytes.size());
}

void RecordWriter::optional_text(bool present, std::string_view text) {
    boolean(present);
    static_cast<void>(blob(present ? text : std::string_view{}));
}

// ---------------------------------------------------------------------------
// RecordReader
// ---------------------------------------------------------------------------

RecordReader::RecordReader(std::string_view data, const Limits& limits)
    : data_(data), limits_(&limits) {}

bool RecordReader::need(std::size_t bytes) {
    if (!ok_) {
        return false;
    }
    if (bytes > data_.size() - offset_) {
        return fail("codec.truncated");
    }
    return true;
}

bool RecordReader::fail(std::string message) {
    if (ok_) {
        ok_ = false;
        error_ = std::move(message);
    }
    return false;
}

std::uint8_t RecordReader::u8() {
    if (!need(1)) {
        return 0;
    }
    const auto value = static_cast<std::uint8_t>(data_[offset_]);
    ++offset_;
    return value;
}

std::uint16_t RecordReader::u16() {
    const std::uint16_t low = u8();
    const std::uint16_t high = u8();
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));
}

std::uint32_t RecordReader::u32() {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(u8()) << shift;
    }
    return value;
}

std::uint64_t RecordReader::u64() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(u8()) << shift;
    }
    return value;
}

bool RecordReader::boolean() { return u8() != 0; }

std::string RecordReader::blob(std::size_t max_bytes) {
    const std::uint32_t length = u32();
    if (!ok_) {
        return {};
    }
    if (length > max_bytes) {
        fail("codec.length_over_limit");
        return {};
    }
    if (!need(length)) {
        return {};
    }
    std::string value(data_.substr(offset_, length));
    offset_ += length;
    return value;
}

std::string RecordReader::text(std::size_t max_bytes) { return blob(max_bytes); }

std::string RecordReader::remaining_bytes() {
    std::string value(data_.substr(offset_));
    offset_ = data_.size();
    return value;
}

}  // namespace fabric_topology
