// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_CODEC_HPP
#define FABRIC_TOPOLOGY_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fabric_topology/limits.hpp"

namespace fabric_topology {

/// Deterministic little-endian writer. The same encoding is used for canonical digest
/// input, persistence payloads and protocol payloads, so independent encoders of identical
/// state produce identical bytes. Raw C++ memory layouts are never written.
class RecordWriter {
public:
    explicit RecordWriter(std::size_t reserve = 256) { data_.reserve(reserve); }

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void boolean(bool value) { u8(value ? std::uint8_t{1} : std::uint8_t{0}); }
    /// u32 little-endian length followed by the raw bytes.
    void blob(std::string_view bytes);
    void text(std::string_view text) { blob(text); }
    void optional_text(bool present, std::string_view text);

    [[nodiscard]] const std::string& data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] std::string take() { return std::move(data_); }

private:
    std::string data_;
};

/// Bounded, fully checked reader. Every length prefix is validated against the configured
/// limits and against the bytes actually remaining before anything is allocated.
class RecordReader {
public:
    RecordReader(std::string_view data, const Limits& limits);

    /// A reader is a view: it never owns the bytes it decodes. Binding it to a temporary
    /// string would leave a dangling view, so that mistake is a compile error rather than a
    /// silent lifetime bug.
    RecordReader(std::string&& data, const Limits& limits) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    bool boolean();
    std::string blob(std::size_t max_bytes);
    std::string text(std::size_t max_bytes);
    std::string remaining_bytes();

    /// Record a decode failure with a stable message. Returns false so callers can write
    /// `return reader.fail(...)` in value-returning decoders.
    bool fail(std::string message);

private:
    [[nodiscard]] bool need(std::size_t bytes);

    std::string_view data_;
    std::size_t offset_ = 0;
    bool ok_ = true;
    const Limits* limits_ = nullptr;
    std::string error_;
};

}  // namespace fabric_topology

#endif  // FABRIC_TOPOLOGY_CODEC_HPP
