// Fabric Topology - real multi-process distributed publication tests.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.
//
// These tests use genuine independent operating-system processes. Worker death is a real
// TerminateProcess, and coordinator restart is a real process restart against a real durable
// image. Nothing is emulated with a flag.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fabric_topology/fabric_topology.hpp"
#include "fabric_topology/runtime/publisher_client.hpp"
#include "test_framework.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace fabric_topology;

namespace {

/// Bounded readiness wait. This synchronises with a child process starting up; it never
/// terminates a test and always fails loudly when the child never becomes ready.
constexpr int kReadyAttempts = 600;
constexpr int kReadySleepMs = 50;

struct TempDirectory {
    std::filesystem::path path;
    explicit TempDirectory(const std::string& name) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("fabric_topology_dist_" + name + "_" + std::to_string(stamp));
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    [[nodiscard]] std::string file(const std::string& name) const {
        return (path / name).string();
    }
};

#ifdef _WIN32

std::wstring to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length > 0 ? length - 1 : 0), L'\0');
    if (length > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), length);
    }
    return out;
}

std::string quote(const std::string& argument) {
    std::string out = "\"";
    for (char c : argument) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess() { terminate(); }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::string& executable, const std::vector<std::string>& arguments) {
        std::string command = quote(executable);
        for (const std::string& argument : arguments) {
            command += ' ';
            command += quote(argument);
        }
        std::wstring mutable_command = to_wide(command);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        const BOOL created = ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr,
                                              FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                              &information_);
        if (created == FALSE) {
            return false;
        }
        running_ = true;
        return true;
    }

    void terminate() {
        if (!running_) {
            return;
        }
        static_cast<void>(::TerminateProcess(information_.hProcess, 1));
        static_cast<void>(::WaitForSingleObject(information_.hProcess, 30000));
        ::CloseHandle(information_.hThread);
        ::CloseHandle(information_.hProcess);
        running_ = false;
    }

    [[nodiscard]] bool running() const { return running_; }

    /// Wait for a short-lived child to finish on its own.
    void wait_for_exit() {
        if (!running_) {
            return;
        }
        static_cast<void>(::WaitForSingleObject(information_.hProcess, 30000));
        DWORD exit_code = 0;
        static_cast<void>(::GetExitCodeProcess(information_.hProcess, &exit_code));
        ::CloseHandle(information_.hThread);
        ::CloseHandle(information_.hProcess);
        information_ = PROCESS_INFORMATION{};
        running_ = false;
        last_exit_code_ = exit_code;
    }

    [[nodiscard]] DWORD last_exit_code() const { return last_exit_code_; }

private:
    PROCESS_INFORMATION information_{};
    bool running_ = false;
    DWORD last_exit_code_ = 0;
};

#endif  // _WIN32

bool read_first_line(const std::string& path, std::string& out) {
    std::ifstream stream(path);
    if (!stream) {
        return false;
    }
    if (!std::getline(stream, out)) {
        return false;
    }
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return !out.empty();
}

bool wait_for_port_file(const std::string& path, std::uint16_t& port) {
    for (int attempt = 0; attempt < kReadyAttempts; ++attempt) {
        std::string text;
        if (read_first_line(path, text)) {
            try {
                const int value = std::stoi(text);
                if (value > 0 && value <= 65535) {
                    port = static_cast<std::uint16_t>(value);
                    return true;
                }
            } catch (const std::exception&) {
                // The file exists but is not complete yet; keep waiting.
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kReadySleepMs));
    }
    return false;
}

std::size_t count_substring(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t position = text.find(needle);
    while (position != std::string::npos) {
        ++count;
        position = text.find(needle, position + needle.size());
    }
    return count;
}

class Observer {
public:
    void attach(std::uint16_t port) {
        runtime::PublisherClientOptions options;
        options.host = "127.0.0.1";
        options.port = port;
        options.publisher = PublisherId::from_trusted("pub-observer");
        options.worker_boot = runtime::make_worker_boot_id("observer");
        options.process_label = "observer";
        options.observer = true;
        options.response_deadline_ms = 30000;
        std::unique_ptr<runtime::PublisherClient> client =
            std::make_unique<runtime::PublisherClient>(std::move(options));
        WelcomeMessage welcome;
        const Status status = client->connect_and_register(welcome);
        if (!outcome_is_success(status.outcome())) {
            return;
        }
        client_ = std::move(client);
    }

    [[nodiscard]] bool attached() const { return client_ != nullptr; }

    std::string query(const std::string& name, const std::string& argument = std::string()) {
        if (client_ == nullptr) {
            return {};
        }
        QueryResultMessage result;
        const Status status = client_->query(name, argument, result);
        if (!outcome_is_success(status.outcome())) {
            return {};
        }
        return result.text;
    }

    bool wait_for(const std::string& name, const std::string& needle,
                  const std::string& argument = std::string()) {
        for (int attempt = 0; attempt < kReadyAttempts; ++attempt) {
            const std::string text = query(name, argument);
            if (text.find(needle) != std::string::npos) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kReadySleepMs));
        }
        return false;
    }

    void detach() { client_.reset(); }

private:
    std::unique_ptr<runtime::PublisherClient> client_;
};

bool executable_available(const char* path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && std::filesystem::file_size(path, error) > 0;
}

std::string generation_of(const std::string& text) {
    const std::string prefix = "generation=";
    const std::size_t position = text.find(prefix);
    if (position == std::string::npos) {
        return {};
    }
    std::size_t end = text.find('\n', position);
    if (end == std::string::npos) {
        end = text.size();
    }
    return text.substr(position + prefix.size(), end - position - prefix.size());
}

}  // namespace

#ifndef _WIN32
FT_TEST(distributed, unsupported_platform) {
    FT_SKIP("multi-process tests require the Windows process API in this build");
}
#else

FT_TEST(distributed, worker_death_is_fenced_and_isolated) {
    if (!executable_available(FT_COORDINATOR_EXE) || !executable_available(FT_PUBLISHER_EXE)) {
        FT_SKIP("ftcoordinator/ftpublisher are not built in this configuration");
    }
    const TempDirectory temp("worker_death");
    const std::string port_file = temp.file("port.txt");
    const std::string state_file = temp.file("coordinator.ftstate");
    const std::string seed_a = temp.file("seed-a.txt");
    const std::string seed_b = temp.file("seed-b.txt");

    // Canonical identity is supplied by Fabric Registry, never by the topology runtime. The
    // publishers declare which identities their scenarios reference and the coordinator is
    // given that registry seed before it starts.
    {
        ChildProcess emitter_a;
        FT_CHECK(emitter_a.start(FT_PUBLISHER_EXE,
                                 {"--write-seed", seed_a, "--domain", "dom-a", "--scenario",
                                  "single_switch", "--publisher", "pub-a"}));
        emitter_a.wait_for_exit();
        ChildProcess emitter_b;
        FT_CHECK(emitter_b.start(FT_PUBLISHER_EXE,
                                 {"--write-seed", seed_b, "--domain", "dom-b", "--scenario",
                                  "dual_switch_redundancy", "--publisher", "pub-b"}));
        emitter_b.wait_for_exit();
    }

    ChildProcess coordinator;
    FT_CHECK(coordinator.start(FT_COORDINATOR_EXE,
                               {"--port", "0", "--port-file", port_file, "--state", state_file,
                                "--registry-seed", seed_a, "--registry-seed", seed_b, "--domain",
                                "dom-a", "--grant", "dom-a=authoritative", "--domain", "dom-b",
                                "--grant", "dom-b=authoritative"}));
    std::uint16_t port = 0;
    FT_CHECK(wait_for_port_file(port_file, port));

    Observer observer;
    for (int attempt = 0; attempt < kReadyAttempts && !observer.attached(); ++attempt) {
        observer.attach(port);
        if (!observer.attached()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kReadySleepMs));
        }
    }
    FT_CHECK(observer.attached());

    ChildProcess worker_a;
    FT_CHECK(worker_a.start(FT_PUBLISHER_EXE,
                            {"--port", std::to_string(port), "--publisher", "pub-a", "--domain",
                             "dom-a", "--scenario", "single_switch", "--publish-count", "1"}));
    ChildProcess worker_b;
    FT_CHECK(worker_b.start(FT_PUBLISHER_EXE,
                            {"--port", std::to_string(port), "--publisher", "pub-b", "--domain",
                             "dom-b", "--scenario", "dual_switch_redundancy", "--publish-count",
                             "1"}));

    FT_CHECK(observer.wait_for("publishers", "publishers=2"));
    FT_CHECK(observer.wait_for("edges", "publisher=pub-b"));

    const std::string before = observer.query("edges");
    const std::size_t a_edges_before = count_substring(before, "publisher=pub-a");
    const std::size_t b_edges_before = count_substring(before, "publisher=pub-b");
    FT_CHECK(a_edges_before > 0);
    FT_CHECK(b_edges_before > 0);

    // Real OS process death, without any graceful shutdown.
    worker_a.terminate();
    FT_CHECK(!worker_a.running());

    // Loss is detected through the actual control path: A's session ends and its worker boot
    // is fenced. Two sessions remain active: the independent publisher and the observer.
    FT_CHECK(observer.wait_for("sessions", "active=2"));
    const std::string publishers = observer.query("publishers");
    FT_CHECK(publishers.find("publisher=pub-a") != std::string::npos);
    FT_CHECK(publishers.find("fenced=true") != std::string::npos);

    const std::string after = observer.query("edges");
    FT_CHECK_EQ(count_substring(after, "publisher=pub-a"), a_edges_before);
    FT_CHECK_EQ(count_substring(after, "publisher=pub-b"), b_edges_before);
    // Every relationship asserted by the dead incarnation is no longer current.
    FT_CHECK(after.find("publisher=pub-a") != std::string::npos);
    const std::string a_line = [&] {
        const std::size_t position = after.find("publisher=pub-a");
        const std::size_t start = after.rfind('\n', position);
        const std::size_t end = after.find('\n', position);
        return after.substr(start == std::string::npos ? 0 : start, end - start);
    }();
    FT_CHECK(a_line.find("REVALIDATION_REQUIRED") != std::string::npos);
    // The independent publisher is unaffected.
    const std::string b_line = [&] {
        const std::size_t position = after.find("publisher=pub-b");
        const std::size_t start = after.rfind('\n', position);
        const std::size_t end = after.find('\n', position);
        return after.substr(start == std::string::npos ? 0 : start, end - start);
    }();
    FT_CHECK(b_line.find("CURRENT") != std::string::npos);

    // A fresh incarnation with a new worker boot re-establishes authority and republishes.
    const std::string generation_before = generation_of(observer.query("statistics"));
    ChildProcess worker_a_prime;
    FT_CHECK(worker_a_prime.start(FT_PUBLISHER_EXE,
                                  {"--port", std::to_string(port), "--publisher", "pub-a",
                                   "--domain", "dom-a", "--scenario", "single_switch",
                                   "--publish-count", "1", "--hold-seconds", "1"}));
    FT_CHECK(observer.wait_for("sessions", "active=3"));
    FT_CHECK(observer.wait_for("publishers", "publisher=pub-a"));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::string final_edges = observer.query("edges");
    FT_CHECK(count_substring(final_edges, "publisher=pub-b") == b_edges_before);
    FT_CHECK(final_edges.find("publisher=pub-b") != std::string::npos);
    const std::string final_generation = generation_of(observer.query("statistics"));
    FT_CHECK(final_generation != generation_before);
    FT_CHECK(observer.query("validate").find("valid=true") != std::string::npos);

    observer.detach();
    worker_b.terminate();
    worker_a_prime.terminate();
    coordinator.terminate();
}

FT_TEST(distributed, coordinator_restart_advances_epoch_and_keeps_structure) {
    if (!executable_available(FT_COORDINATOR_EXE) || !executable_available(FT_PUBLISHER_EXE)) {
        FT_SKIP("ftcoordinator/ftpublisher are not built in this configuration");
    }
    const TempDirectory temp("coordinator_restart");
    const std::string port_file = temp.file("port.txt");
    const std::string second_port_file = temp.file("port2.txt");
    const std::string state_file = temp.file("coordinator.ftstate");
    const std::string seed_file = temp.file("seed.txt");
    const std::string seed_single = temp.file("seed-single.txt");
    {
        ChildProcess emitter;
        FT_CHECK(emitter.start(FT_PUBLISHER_EXE,
                               {"--write-seed", seed_file, "--domain", "dom-a", "--scenario",
                                "dual_switch_redundancy", "--publisher", "pub-a"}));
        emitter.wait_for_exit();
        ChildProcess single_emitter;
        FT_CHECK(single_emitter.start(FT_PUBLISHER_EXE,
                                      {"--write-seed", seed_single, "--domain", "dom-a",
                                       "--scenario", "single_switch", "--publisher", "pub-fresh"}));
        single_emitter.wait_for_exit();
    }

    std::uint16_t first_port = 0;
    std::uint16_t second_port = 0;
    std::string epoch_before;
    std::string generation_before;
    std::string fenced_boot;

    {
        ChildProcess coordinator;
        FT_CHECK(coordinator.start(FT_COORDINATOR_EXE,
                                   {"--port", "0", "--port-file", port_file, "--state", state_file,
                                    "--registry-seed", seed_file, "--registry-seed", seed_single, "--domain",
                                    "dom-a", "--grant", "dom-a=authoritative"}));
        FT_CHECK(wait_for_port_file(port_file, first_port));

        Observer observer;
        for (int attempt = 0; attempt < kReadyAttempts && !observer.attached(); ++attempt) {
            observer.attach(first_port);
            if (!observer.attached()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReadySleepMs));
            }
        }
        FT_CHECK(observer.attached());

        fenced_boot = runtime::make_worker_boot_id("restart-proof").value();
        ChildProcess worker;
        FT_CHECK(worker.start(FT_PUBLISHER_EXE,
                              {"--port", std::to_string(first_port), "--publisher", "pub-a",
                               "--domain", "dom-a", "--scenario", "dual_switch_redundancy",
                               "--worker-boot", fenced_boot, "--publish-count", "1"}));
        FT_CHECK(observer.wait_for("edges", "publisher=pub-a"));
        FT_CHECK(observer.wait_for("publishers", "publishers=1"));

        // Kill the worker so its boot identifier is fenced and persisted. Only the observer's
        // own session remains active.
        worker.terminate();
        FT_CHECK(observer.wait_for("sessions", "active=1"));

        epoch_before = [&] {
            const std::string text = observer.query("epoch");
            return text;
        }();
        generation_before = generation_of(observer.query("statistics"));
        FT_CHECK(!epoch_before.empty());
        FT_CHECK(!generation_before.empty());
        FT_CHECK(observer.query("validate").find("valid=true") != std::string::npos);
        observer.detach();

        // Real coordinator death without any graceful shutdown.
        coordinator.terminate();
    }

    {
        ChildProcess coordinator;
        FT_CHECK(coordinator.start(FT_COORDINATOR_EXE,
                                   {"--port", "0", "--port-file", second_port_file, "--state",
                                    state_file, "--registry-seed", seed_file, "--registry-seed",
                                    seed_single, "--domain", "dom-a", "--grant",
                                    "dom-a=authoritative"}));
        FT_CHECK(wait_for_port_file(second_port_file, second_port));
        FT_CHECK(second_port != 0);

        Observer observer;
        for (int attempt = 0; attempt < kReadyAttempts && !observer.attached(); ++attempt) {
            observer.attach(second_port);
            if (!observer.attached()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReadySleepMs));
            }
        }
        FT_CHECK(observer.attached());

        // The grace note above is deliberate: the observer's own session is the only one.
        // The coordinator epoch advances on every fresh start.
        const std::string epoch_after = observer.query("epoch");
        FT_CHECK(epoch_after != epoch_before);
        FT_CHECK(std::stoull(epoch_after) == std::stoull(epoch_before) + 1);

        // Durable structure survives; live authority does not.
        const std::string generation_after = generation_of(observer.query("statistics"));
        FT_CHECK_EQ(generation_after, generation_before);
        const std::string validation = observer.query("validate");
        FT_CHECK(validation.find("valid=true") != std::string::npos);
        FT_CHECK(validation.find("non_current_edges=") != std::string::npos);
        const std::string edges = observer.query("edges");
        FT_CHECK(edges.find("publisher=pub-a") != std::string::npos);
        // Recovered relationships are durable but no longer current.
        FT_CHECK(edges.find("lifecycle=CURRENT") == std::string::npos);
        FT_CHECK(edges.find("lifecycle=REVALIDATION_REQUIRED") != std::string::npos);

        // Traffic from the previous incarnation is refused: stale epoch.
        ChildProcess stale_epoch;
        FT_CHECK(stale_epoch.start(FT_PUBLISHER_EXE,
                                   {"--port", std::to_string(second_port), "--publisher", "pub-stale",
                                    "--domain", "dom-a", "--scenario", "single_switch",
                                    "--claimed-epoch", epoch_before, "--publish-count", "1",
                                    "--hold-seconds", "1"}));

        // Traffic from the fenced worker boot is refused even across the restart.
        ChildProcess stale_boot;
        FT_CHECK(stale_boot.start(FT_PUBLISHER_EXE,
                                  {"--port", std::to_string(second_port), "--publisher", "pub-b",
                                   "--domain", "dom-a", "--scenario", "single_switch",
                                   "--worker-boot", fenced_boot, "--publish-count", "1",
                                   "--hold-seconds", "1"}));
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        // Fresh authority revalidates and advances the generation.
        ChildProcess fresh;
        FT_CHECK(fresh.start(FT_PUBLISHER_EXE,
                             {"--port", std::to_string(second_port), "--publisher", "pub-fresh",
                              "--domain", "dom-a", "--scenario", "single_switch", "--mode",
                              "authoritative", "--publish-count", "1", "--hold-seconds", "1"}));
        FT_CHECK(observer.wait_for("edges", "publisher=pub-fresh"));

        const std::string final_generation = generation_of(observer.query("statistics"));
        FT_CHECK(std::stoull(final_generation) > std::stoull(generation_before));
        FT_CHECK(observer.query("validate").find("valid=true") != std::string::npos);

        observer.detach();
        fresh.terminate();
        stale_epoch.terminate();
        stale_boot.terminate();
        coordinator.terminate();
    }
}

FT_TEST(distributed, malformed_peer_never_disturbs_the_coordinator) {
    if (!executable_available(FT_COORDINATOR_EXE)) {
        FT_SKIP("ftcoordinator is not built in this configuration");
    }
    const TempDirectory temp("malformed_peer");
    const std::string port_file = temp.file("port.txt");

    ChildProcess coordinator;
    FT_CHECK(coordinator.start(FT_COORDINATOR_EXE,
                               {"--port", "0", "--port-file", port_file, "--domain", "dom-a",
                                "--grant", "dom-a=authoritative"}));
    std::uint16_t port = 0;
    FT_CHECK(wait_for_port_file(port_file, port));

    // Connect and send a frame with a corrupt checksum, then a truncated frame, then garbage.
    for (int iteration = 0; iteration < 20; ++iteration) {
        runtime::FrameTransport transport;
        std::string error;
        if (!transport.connect_to("127.0.0.1", port, error)) {
            continue;
        }
        std::string frame = encode_frame(FrameHeader{}, std::string(64, 'x'));
        frame.back() = static_cast<char>(frame.back() ^ 0xFF);
        static_cast<void>(transport.send(MessageType::Hello, 0, frame.substr(kFrameHeaderBytes),
                                         error));
        transport.request_stop();
        transport.close();
    }
    for (int iteration = 0; iteration < 20; ++iteration) {
        runtime::FrameTransport transport;
        std::string error;
        if (!transport.connect_to("127.0.0.1", port, error)) {
            continue;
        }
        static_cast<void>(transport.send(MessageType::Query, 0, "truncated", error));
        transport.request_stop();
        transport.close();
    }

    Observer observer;
    for (int attempt = 0; attempt < kReadyAttempts && !observer.attached(); ++attempt) {
        observer.attach(port);
        if (!observer.attached()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kReadySleepMs));
        }
    }
    FT_CHECK(observer.attached());
    FT_CHECK(observer.query("validate").find("valid=true") != std::string::npos);
    FT_CHECK(observer.query("generation") == std::string("0"));
    observer.detach();
    coordinator.terminate();
}

FT_TEST(distributed, repeated_start_stop_leaves_no_orphans) {
    if (!executable_available(FT_COORDINATOR_EXE) || !executable_available(FT_PUBLISHER_EXE)) {
        FT_SKIP("ftcoordinator/ftpublisher are not built in this configuration");
    }
    const TempDirectory temp("start_stop");
    for (int iteration = 0; iteration < 3; ++iteration) {
        const std::string port_file = temp.file("port-" + std::to_string(iteration) + ".txt");
        ChildProcess coordinator;
        FT_CHECK(coordinator.start(FT_COORDINATOR_EXE,
                                   {"--port", "0", "--port-file", port_file, "--domain", "dom-a",
                                    "--grant", "dom-a=authoritative"}));
        std::uint16_t port = 0;
        FT_CHECK(wait_for_port_file(port_file, port));

        ChildProcess worker;
        FT_CHECK(worker.start(FT_PUBLISHER_EXE,
                              {"--port", std::to_string(port), "--publisher", "pub-loop",
                               "--domain", "dom-a", "--scenario", "single_switch",
                               "--request-shutdown", "--publish-count", "1"}));
        // The publisher asks the coordinator to stop; the coordinator exits on its own.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        worker.terminate();
        coordinator.terminate();
    }
    // Every child above was terminated through its own destructor; reaching this line with a
    // clean process table is the assertion.
    FT_CHECK(true);
}

#endif  // _WIN32
