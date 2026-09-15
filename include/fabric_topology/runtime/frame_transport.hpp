// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_RUNTIME_FRAME_TRANSPORT_HPP
#define FABRIC_TOPOLOGY_RUNTIME_FRAME_TRANSPORT_HPP

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fabric_topology/limits.hpp"
#include "fabric_topology/protocol.hpp"
#include "fabric_topology/runtime/socket.hpp"

namespace fabric_topology::runtime {

struct ReceivedFrame {
    FrameHeader header;
    std::string payload;
};

enum class ReceiveOutcome {
    /// One complete, integrity-checked frame was decoded.
    Frame = 0,
    /// No frame arrived within the poll interval. The caller decides whether to continue.
    Idle = 1,
    /// The peer closed the connection, or request_stop() was called.
    Closed = 2,
    /// The peer sent something that is not a valid frame, or the socket failed.
    Error = 3,
};

/// Framed, integrity-checked, bounded TCP transport.
///
/// Shutdown design: a receive never blocks indefinitely. The socket is put into a poll loop
/// with a short interval and an explicit stop flag, so a blocking receive always returns
/// within the poll interval after stop is requested. The implementation never relies on
/// shutdown() interrupting a blocked recv, and never joins a thread from itself.
class FrameTransport {
public:
    explicit FrameTransport(Limits limits = Limits{});
    ~FrameTransport();

    FrameTransport(const FrameTransport&) = delete;
    FrameTransport& operator=(const FrameTransport&) = delete;
    FrameTransport(FrameTransport&& other) noexcept;
    FrameTransport& operator=(FrameTransport&& other) noexcept;

    [[nodiscard]] bool listen(const std::string& bind_address, std::uint16_t port, std::string& error);
    [[nodiscard]] bool connect_to(const std::string& host, std::uint16_t port, std::string& error);
    /// Accept one client. Returns nullopt on poll timeout (timed_out set) or on error.
    [[nodiscard]] std::optional<FrameTransport> accept_client(std::string& error, bool& timed_out);

    [[nodiscard]] bool send(MessageType type, std::uint32_t flags, std::string_view payload,
                            std::string& error);

    [[nodiscard]] ReceiveOutcome receive(ReceivedFrame& frame, std::string& error);

    void request_stop() noexcept;
    void close() noexcept;

    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
    [[nodiscard]] std::uint16_t local_port() const noexcept { return local_port_; }
    [[nodiscard]] const std::string& peer_description() const noexcept { return peer_; }
    void set_peer_description(std::string text) { peer_ = std::move(text); }
    void set_poll_interval_ms(int milliseconds) noexcept { poll_interval_ms_ = milliseconds; }
    [[nodiscard]] int poll_interval_ms() const noexcept { return poll_interval_ms_; }
    [[nodiscard]] std::uint64_t frames_sent() const noexcept { return send_sequence_; }
    [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_received_; }

private:
    /// Poll the socket until it is readable, the poll interval elapses, or stop is requested.
    [[nodiscard]] ReceiveOutcome pump(std::string& error, bool& readable);

    Limits limits_;
    Socket socket_;
    std::string buffer_;
    std::string peer_;
    std::atomic<bool> stop_{false};
    int poll_interval_ms_ = 50;
    std::uint64_t send_sequence_ = 0;
    std::uint64_t frames_received_ = 0;
    std::uint16_t local_port_ = 0;
};

}  // namespace fabric_topology::runtime

#endif  // FABRIC_TOPOLOGY_RUNTIME_FRAME_TRANSPORT_HPP
