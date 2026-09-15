// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_RUNTIME_SOCKET_HPP
#define FABRIC_TOPOLOGY_RUNTIME_SOCKET_HPP

#include <cstdint>
#include <string>

#include "fabric_topology/result.hpp"

namespace fabric_topology::runtime {

inline constexpr std::intptr_t kInvalidSocketHandle = -1;

/// Initialise the process-wide networking subsystem. Idempotent and thread-safe.
[[nodiscard]] Status initialize_networking();

/// Release the process-wide networking subsystem.
void shutdown_networking();

/// Human-readable description of the last socket error on this thread.
[[nodiscard]] std::string last_socket_error();

/// Owned native socket handle with move-only semantics.
class Socket {
public:
    Socket() = default;
    explicit Socket(std::intptr_t handle) noexcept;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::intptr_t native() const noexcept;
    /// Relinquish ownership; the caller becomes responsible for closing the handle.
    std::intptr_t release() noexcept;
    void close() noexcept;

    [[nodiscard]] bool set_reuse_address(std::string& error) const;
    [[nodiscard]] bool set_receive_timeout_ms(int milliseconds, std::string& error) const;
    [[nodiscard]] bool set_tcp_nodelay(std::string& error) const;

private:
    std::intptr_t handle_ = kInvalidSocketHandle;
};

}  // namespace fabric_topology::runtime

#endif  // FABRIC_TOPOLOGY_RUNTIME_SOCKET_HPP
