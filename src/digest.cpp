// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fabric_topology {

namespace {

constexpr std::array<std::uint32_t, 64> kSha256K{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
}};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

[[nodiscard]] constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
    return rotr(x, 2U) ^ rotr(x, 13U) ^ rotr(x, 22U);
}

[[nodiscard]] constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
    return rotr(x, 6U) ^ rotr(x, 11U) ^ rotr(x, 25U);
}

[[nodiscard]] constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
    return rotr(x, 7U) ^ rotr(x, 18U) ^ (x >> 3U);
}

[[nodiscard]] constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
    return rotr(x, 17U) ^ rotr(x, 19U) ^ (x >> 10U);
}

void sha256_compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t t1 = h + big_sigma1(e) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
        const std::uint32_t t2 = big_sigma0(a) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

[[nodiscard]] constexpr char hex_digit(unsigned value) noexcept {
    return static_cast<char>(value < 10U ? ('0' + value) : ('a' + (value - 10U)));
}

}  // namespace

std::string sha256_raw(std::string_view bytes) {
    std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                       0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    const std::size_t total = bytes.size();
    std::size_t offset = 0;
    std::array<std::uint8_t, 64> block{};

    while (total - offset >= 64) {
        sha256_compress(state, reinterpret_cast<const std::uint8_t*>(bytes.data()) + offset);
        offset += 64;
    }

    const std::size_t tail = total - offset;
    block.fill(0);
    for (std::size_t i = 0; i < tail; ++i) {
        block[i] = static_cast<std::uint8_t>(bytes[offset + i]);
    }
    block[tail] = 0x80U;

    if (tail >= 56) {
        sha256_compress(state, block.data());
        block.fill(0);
    }

    const std::uint64_t bit_length = static_cast<std::uint64_t>(total) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        block[63 - i] = static_cast<std::uint8_t>((bit_length >> (8U * i)) & 0xFFU);
    }
    sha256_compress(state, block.data());

    std::string out(32, '\0');
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<char>((state[i] >> 24U) & 0xFFU);
        out[i * 4 + 1] = static_cast<char>((state[i] >> 16U) & 0xFFU);
        out[i * 4 + 2] = static_cast<char>((state[i] >> 8U) & 0xFFU);
        out[i * 4 + 3] = static_cast<char>(state[i] & 0xFFU);
    }
    return out;
}

std::string sha256_hex(std::string_view bytes) {
    const std::string raw = sha256_raw(bytes);
    std::string out;
    out.resize(raw.size() * 2);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto byte = static_cast<unsigned>(static_cast<unsigned char>(raw[i]));
        out[i * 2] = hex_digit(byte >> 4U);
        out[i * 2 + 1] = hex_digit(byte & 0x0FU);
    }
    return out;
}

std::uint32_t crc32(std::string_view bytes) noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256U; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0U ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
            }
            result[i] = value;
        }
        return result;
    }();

    std::uint32_t crc = 0xFFFFFFFFU;
    for (char c : bytes) {
        const auto index = static_cast<std::uint8_t>((crc ^ static_cast<std::uint32_t>(
                                                               static_cast<unsigned char>(c))) &
                                                      0xFFU);
        crc = table[index] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint64_t fnv1a64(std::string_view bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (char c : bytes) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace fabric_topology
