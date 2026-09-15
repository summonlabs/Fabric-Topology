// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/runtime/frame_transport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "fabric_topology/digest.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fabric_topology::runtime {

namespace {

[[nodiscard]] int native_last_error() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

[[nodiscard]] bool is_would_block(int code) noexcept {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAETIMEDOUT || code == WSAEINTR;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
#endif
}

[[nodiscard]] int native_recv(std::intptr_t handle, char* buffer, int length) noexcept {
#ifdef _WIN32
    return ::recv(static_cast<SOCKET>(handle), buffer, length, 0);
#else
    return static_cast<int>(
        ::recv(static_cast<int>(handle), buffer, static_cast<std::size_t>(length), 0));
#endif
}

[[nodiscard]] int native_send(std::intptr_t handle, const char* buffer, int length) noexcept {
#ifdef _WIN32
    return ::send(static_cast<SOCKET>(handle), buffer, length, 0);
#else
    return static_cast<int>(
        ::send(static_cast<int>(handle), buffer, static_cast<std::size_t>(length), 0));
#endif
}

[[nodiscard]] int native_close_listen(std::intptr_t handle) noexcept {
#ifdef _WIN32
    return ::closesocket(static_cast<SOCKET>(handle));
#else
    return ::close(static_cast<int>(handle));
#endif
}

constexpr std::size_t kChunkBytes = 16384;

}  // namespace

FrameTransport::FrameTransport(Limits limits) : limits_(limits) {}

FrameTransport::~FrameTransport() { close(); }

FrameTransport::FrameTransport(FrameTransport&& other) noexcept
    : limits_(other.limits_),
      socket_(std::move(other.socket_)),
      buffer_(std::move(other.buffer_)),
      peer_(std::move(other.peer_)),
      stop_(other.stop_.load()),
      poll_interval_ms_(other.poll_interval_ms_),
      send_sequence_(other.send_sequence_),
      frames_received_(other.frames_received_),
      local_port_(other.local_port_) {}

FrameTransport& FrameTransport::operator=(FrameTransport&& other) noexcept {
    if (this != &other) {
        close();
        limits_ = other.limits_;
        socket_ = std::move(other.socket_);
        buffer_ = std::move(other.buffer_);
        peer_ = std::move(other.peer_);
        stop_.store(other.stop_.load());
        poll_interval_ms_ = other.poll_interval_ms_;
        send_sequence_ = other.send_sequence_;
        frames_received_ = other.frames_received_;
        local_port_ = other.local_port_;
    }
    return *this;
}

bool FrameTransport::listen(const std::string& bind_address, std::uint16_t port, std::string& error) {
    const Status initialized = initialize_networking();
    if (initialized.outcome() != Outcome::Ok) {
        error = initialized.one_line();
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(port);
    if (::getaddrinfo(bind_address.c_str(), port_text.c_str(), &hints, &results) != 0 ||
        results == nullptr) {
        error = "transport.resolve_failed: " + last_socket_error();
        return false;
    }

    Socket socket(static_cast<std::intptr_t>(
        ::socket(results->ai_family, results->ai_socktype, results->ai_protocol)));
    if (!socket.valid()) {
        ::freeaddrinfo(results);
        error = "transport.socket_failed: " + last_socket_error();
        return false;
    }
    if (!socket.set_reuse_address(error)) {
        ::freeaddrinfo(results);
        return false;
    }
    if (::bind(static_cast<int>(socket.native()), results->ai_addr,
               static_cast<int>(results->ai_addrlen)) != 0) {
        error = "transport.bind_failed: " + last_socket_error();
        ::freeaddrinfo(results);
        return false;
    }
    if (::listen(static_cast<int>(socket.native()), static_cast<int>(limits_.max_sessions)) != 0) {
        error = "transport.listen_failed: " + last_socket_error();
        ::freeaddrinfo(results);
        return false;
    }

    sockaddr_storage address{};
    int address_length = static_cast<int>(sizeof(address));
    if (::getsockname(static_cast<int>(socket.native()), reinterpret_cast<sockaddr*>(&address),
                      &address_length) == 0 &&
        address.ss_family == AF_INET) {
        const auto* inet = reinterpret_cast<const sockaddr_in*>(&address);
        local_port_ = ntohs(inet->sin_port);
    }
    ::freeaddrinfo(results);
    socket_ = std::move(socket);
    peer_ = "listener:" + std::to_string(local_port_);
    return true;
}

bool FrameTransport::connect_to(const std::string& host, std::uint16_t port, std::string& error) {
    const Status initialized = initialize_networking();
    if (initialized.outcome() != Outcome::Ok) {
        error = initialized.one_line();
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results) != 0 || results == nullptr) {
        error = "transport.resolve_failed: " + last_socket_error();
        return false;
    }

    Socket socket(static_cast<std::intptr_t>(
        ::socket(results->ai_family, results->ai_socktype, results->ai_protocol)));
    if (!socket.valid()) {
        ::freeaddrinfo(results);
        error = "transport.socket_failed: " + last_socket_error();
        return false;
    }
    if (::connect(static_cast<int>(socket.native()), results->ai_addr,
                  static_cast<int>(results->ai_addrlen)) != 0) {
        error = "transport.connect_failed: " + last_socket_error();
        ::freeaddrinfo(results);
        return false;
    }
    ::freeaddrinfo(results);

    std::string option_error;
    static_cast<void>(socket.set_tcp_nodelay(option_error));

    sockaddr_storage address{};
    int address_length = static_cast<int>(sizeof(address));
    if (::getsockname(static_cast<int>(socket.native()), reinterpret_cast<sockaddr*>(&address),
                      &address_length) == 0 &&
        address.ss_family == AF_INET) {
        const auto* inet = reinterpret_cast<const sockaddr_in*>(&address);
        local_port_ = ntohs(inet->sin_port);
    }
    socket_ = std::move(socket);
    peer_ = host + ":" + port_text;
    return true;
}

std::optional<FrameTransport> FrameTransport::accept_client(std::string& error, bool& timed_out) {
    timed_out = false;
    error.clear();
    if (!socket_.valid()) {
        error = "transport.invalid_listener";
        return std::nullopt;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(static_cast<int>(socket_.native()), &read_set);
    timeval timeout{};
    timeout.tv_sec = poll_interval_ms_ / 1000;
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((poll_interval_ms_ % 1000) * 1000);
    const int ready =
        ::select(static_cast<int>(socket_.native()) + 1, &read_set, nullptr, nullptr, &timeout);
    if (ready == 0) {
        timed_out = true;
        return std::nullopt;
    }
    if (ready < 0) {
        const int code = native_last_error();
        if (is_would_block(code)) {
            timed_out = true;
            return std::nullopt;
        }
        error = "transport.select_failed: " + last_socket_error();
        return std::nullopt;
    }

    sockaddr_storage address{};
    int address_length = static_cast<int>(sizeof(address));
    Socket client(static_cast<std::intptr_t>(::accept(
        static_cast<int>(socket_.native()), reinterpret_cast<sockaddr*>(&address), &address_length)));
    if (!client.valid()) {
        error = "transport.accept_failed: " + last_socket_error();
        return std::nullopt;
    }

    FrameTransport transport(limits_);
    transport.poll_interval_ms_ = poll_interval_ms_;
    transport.socket_ = std::move(client);
    std::string peer = "peer";
    if (address.ss_family == AF_INET) {
        const auto* inet = reinterpret_cast<const sockaddr_in*>(&address);
        std::array<char, 64> text{};
        if (::inet_ntop(AF_INET, &inet->sin_addr, text.data(),
                        static_cast<socklen_t>(text.size())) != nullptr) {
            peer = std::string(text.data());
            peer += ':';
            peer += std::to_string(ntohs(inet->sin_port));
        }
    }
    transport.peer_ = std::move(peer);
    return std::optional<FrameTransport>(std::move(transport));
}

bool FrameTransport::send(MessageType type, std::uint32_t flags, std::string_view payload,
                          std::string& error) {
    error.clear();
    if (!socket_.valid()) {
        error = "transport.not_connected";
        return false;
    }
    if (payload.size() > limits_.max_frame_payload_bytes) {
        error = "transport.payload_too_large";
        return false;
    }
    FrameHeader header;
    header.version = kProtocolVersion;
    header.type = type;
    header.flags = flags;
    header.sequence = send_sequence_ + 1;
    header.payload_bytes = static_cast<std::uint32_t>(payload.size());
    header.payload_crc32 = crc32(payload);

    const std::string frame = encode_frame(header, payload);
    std::size_t offset = 0;
    while (offset < frame.size()) {
        const int remaining =
            static_cast<int>((std::min)(frame.size() - offset, static_cast<std::size_t>(kChunkBytes)));
        const int written = native_send(socket_.native(), frame.data() + offset, remaining);
        if (written <= 0) {
            const int code = native_last_error();
            if (is_would_block(code)) {
                continue;
            }
            error = "transport.send_failed: " + last_socket_error();
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    send_sequence_ = header.sequence;
    return true;
}

ReceiveOutcome FrameTransport::pump(std::string& error, bool& readable) {
    readable = false;
    if (!socket_.valid()) {
        error = "transport.not_connected";
        return ReceiveOutcome::Closed;
    }
    if (stop_.load()) {
        return ReceiveOutcome::Closed;
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(static_cast<int>(socket_.native()), &read_set);
    timeval timeout{};
    timeout.tv_sec = poll_interval_ms_ / 1000;
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((poll_interval_ms_ % 1000) * 1000);
    const int ready =
        ::select(static_cast<int>(socket_.native()) + 1, &read_set, nullptr, nullptr, &timeout);
    if (ready == 0) {
        return ReceiveOutcome::Idle;
    }
    if (ready < 0) {
        const int code = native_last_error();
        if (is_would_block(code)) {
            return ReceiveOutcome::Idle;
        }
        error = "transport.select_failed: " + last_socket_error();
        return ReceiveOutcome::Error;
    }
    readable = true;
    return ReceiveOutcome::Frame;
}

ReceiveOutcome FrameTransport::receive(ReceivedFrame& frame, std::string& error) {
    error.clear();
    for (;;) {
        std::size_t consumed = 0;
        const FrameStatus status =
            decode_frame(buffer_, limits_, frame.header, frame.payload, consumed, error);
        if (status == FrameStatus::Complete) {
            buffer_.erase(0, consumed);
            ++frames_received_;
            return ReceiveOutcome::Frame;
        }
        if (status == FrameStatus::Malformed) {
            return ReceiveOutcome::Error;
        }
        if (buffer_.size() > limits_.max_frame_bytes + kFrameHeaderBytes) {
            error = "transport.buffer_overflow";
            return ReceiveOutcome::Error;
        }

        bool readable = false;
        const ReceiveOutcome pumped = pump(error, readable);
        if (pumped != ReceiveOutcome::Frame) {
            return pumped;
        }
        if (!readable) {
            continue;
        }
        std::array<char, kChunkBytes> chunk{};
        const int received =
            native_recv(socket_.native(), chunk.data(), static_cast<int>(chunk.size()));
        if (received == 0) {
            return ReceiveOutcome::Closed;
        }
        if (received < 0) {
            const int code = native_last_error();
            if (is_would_block(code)) {
                continue;
            }
            error = "transport.receive_failed: " + last_socket_error();
            return ReceiveOutcome::Error;
        }
        buffer_.append(chunk.data(), static_cast<std::size_t>(received));
    }
}

void FrameTransport::request_stop() noexcept { stop_.store(true); }

void FrameTransport::close() noexcept {
    request_stop();
    socket_.close();
    buffer_.clear();
}

}  // namespace fabric_topology::runtime
