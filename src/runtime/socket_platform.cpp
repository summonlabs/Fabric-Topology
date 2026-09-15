// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/runtime/socket.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fabric_topology::runtime {

namespace {

std::atomic<int> g_initialization_count{0};
std::mutex g_initialization_mutex;

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] int native_last_error() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

[[nodiscard]] int close_native(std::intptr_t handle) noexcept {
#ifdef _WIN32
    return ::closesocket(static_cast<SOCKET>(handle));
#else
    return ::close(static_cast<int>(handle));
#endif
}

}  // namespace

std::string last_socket_error() {
    const int code = native_last_error();
    std::string message = "socket error ";
    message += std::to_string(code);
#ifdef _WIN32
    char* buffer = nullptr;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer), 0, nullptr);
    if (length > 0 && buffer != nullptr) {
        std::string text(buffer, static_cast<std::size_t>(length));
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
        message += ": ";
        message += text;
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
#endif
    return message;
}

Status initialize_networking() {
    std::lock_guard<std::mutex> lock(g_initialization_mutex);
    if (g_initialization_count.load() > 0) {
        g_initialization_count.fetch_add(1);
        return make_status(Outcome::Ok, "networking.already_initialized");
    }
#ifdef _WIN32
    WSADATA data{};
    const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        Status status = make_status(Outcome::InternalError, "networking.wsa_startup_failed");
        status.with("code", std::to_string(result));
        return status;
    }
#endif
    g_initialization_count.store(1);
    return make_status(Outcome::Ok, "networking.initialized");
}

void shutdown_networking() {
    std::lock_guard<std::mutex> lock(g_initialization_mutex);
    const int current = g_initialization_count.load();
    if (current <= 0) {
        return;
    }
    if (current > 1) {
        g_initialization_count.store(current - 1);
        return;
    }
    g_initialization_count.store(0);
#ifdef _WIN32
    ::WSACleanup();
#endif
}

Socket::Socket(std::intptr_t handle) noexcept : handle_(handle) {}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.release();
    }
    return *this;
}

bool Socket::valid() const noexcept { return handle_ != kInvalidSocketHandle; }

std::intptr_t Socket::native() const noexcept { return handle_; }

std::intptr_t Socket::release() noexcept {
    const std::intptr_t handle = handle_;
    handle_ = kInvalidSocketHandle;
    return handle;
}

void Socket::close() noexcept {
    if (handle_ != kInvalidSocketHandle) {
        static_cast<void>(close_native(handle_));
        handle_ = kInvalidSocketHandle;
    }
}

bool Socket::set_reuse_address(std::string& error) const {
    if (!valid()) {
        error = "socket.invalid";
        return false;
    }
    const int one = 1;
    if (::setsockopt(static_cast<int>(handle_), SOL_SOCKET,
                     static_cast<int>(SO_REUSEADDR), reinterpret_cast<const char*>(&one),
                     static_cast<int>(sizeof(one))) != 0) {
        error = last_socket_error();
        return false;
    }
    return true;
}

bool Socket::set_receive_timeout_ms(int milliseconds, std::string& error) const {
    if (!valid()) {
        error = "socket.invalid";
        return false;
    }
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(milliseconds);
    if (::setsockopt(static_cast<SOCKET>(handle_), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value))) != 0) {
        error = last_socket_error();
        return false;
    }
#else
    timeval value{};
    value.tv_sec = milliseconds / 1000;
    value.tv_usec = (milliseconds % 1000) * 1000;
    if (::setsockopt(static_cast<int>(handle_), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value))) != 0) {
        error = last_socket_error();
        return false;
    }
#endif
    return true;
}

bool Socket::set_tcp_nodelay(std::string& error) const {
    if (!valid()) {
        error = "socket.invalid";
        return false;
    }
    const int one = 1;
    if (::setsockopt(static_cast<int>(handle_), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), static_cast<int>(sizeof(one))) != 0) {
        error = last_socket_error();
        return false;
    }
    return true;
}

}  // namespace fabric_topology::runtime
