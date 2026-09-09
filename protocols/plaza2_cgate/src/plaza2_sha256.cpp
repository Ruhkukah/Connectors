#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include <array>
#include <fstream>
#include <sstream>

namespace moex::plaza2::cgate {
namespace {
struct Sha256State {
    std::array<std::uint32_t, 8> hash{
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
    };
    std::uint64_t total_bits{0};
    std::array<std::byte, 64> buffer{};
    std::size_t buffer_size{0};
};

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFau, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t shift) noexcept {
    return (value >> shift) | (value << (32u - shift));
}

void sha256_compress(Sha256State& state, const std::byte* block) {
    std::uint32_t schedule[64]{};
    for (std::size_t idx = 0; idx < 16; ++idx) {
        const auto base = idx * 4;
        schedule[idx] = (static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[base])) << 24u) |
                        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[base + 1])) << 16u) |
                        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[base + 2])) << 8u) |
                        static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[base + 3]));
    }
    for (std::size_t idx = 16; idx < 64; ++idx) {
        const auto s0 = rotr(schedule[idx - 15], 7u) ^ rotr(schedule[idx - 15], 18u) ^ (schedule[idx - 15] >> 3u);
        const auto s1 = rotr(schedule[idx - 2], 17u) ^ rotr(schedule[idx - 2], 19u) ^ (schedule[idx - 2] >> 10u);
        schedule[idx] = schedule[idx - 16] + s0 + schedule[idx - 7] + s1;
    }

    auto a = state.hash[0];
    auto b = state.hash[1];
    auto c = state.hash[2];
    auto d = state.hash[3];
    auto e = state.hash[4];
    auto f = state.hash[5];
    auto g = state.hash[6];
    auto h = state.hash[7];

    for (std::size_t idx = 0; idx < 64; ++idx) {
        const auto s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
        const auto choice = (e & f) ^ (~e & g);
        const auto temp1 = h + s1 + choice + kSha256K[idx] + schedule[idx];
        const auto s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state.hash[0] += a;
    state.hash[1] += b;
    state.hash[2] += c;
    state.hash[3] += d;
    state.hash[4] += e;
    state.hash[5] += f;
    state.hash[6] += g;
    state.hash[7] += h;
}

void sha256_update(Sha256State& state, std::span<const std::byte> bytes) {
    for (const auto byte : bytes) {
        state.buffer[state.buffer_size++] = byte;
        if (state.buffer_size == state.buffer.size()) {
            sha256_compress(state, state.buffer.data());
            state.total_bits += static_cast<std::uint64_t>(state.buffer.size()) * 8u;
            state.buffer_size = 0;
        }
    }
}

[[nodiscard]] std::string sha256_finish(Sha256State& state) {
    state.total_bits += static_cast<std::uint64_t>(state.buffer_size) * 8u;
    state.buffer[state.buffer_size++] = std::byte{0x80u};
    if (state.buffer_size > 56) {
        while (state.buffer_size < 64) {
            state.buffer[state.buffer_size++] = std::byte{0};
        }
        sha256_compress(state, state.buffer.data());
        state.buffer_size = 0;
    }
    while (state.buffer_size < 56) {
        state.buffer[state.buffer_size++] = std::byte{0};
    }

    for (int shift = 56; shift >= 0; shift -= 8) {
        state.buffer[state.buffer_size++] = std::byte{static_cast<unsigned char>((state.total_bits >> shift) & 0xFFu)};
    }
    sha256_compress(state, state.buffer.data());

    std::ostringstream out;
    out << std::hex;
    for (const auto value : state.hash) {
        out.width(8);
        out.fill('0');
        out << std::nouppercase << value;
    }
    return out.str();
}

} // namespace
namespace detail {
[[nodiscard]] std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Sha256State state;
    std::array<std::byte, 4096> buffer{};
    while (input.good()) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0) {
            break;
        }
        sha256_update(state, std::span<const std::byte>(buffer.data(), count));
    }
    return sha256_finish(state);
}

} // namespace detail
std::string plaza2_sha256_hex(std::span<const std::byte> bytes) {
    Sha256State state;
    sha256_update(state, bytes);
    return sha256_finish(state);
}

std::string plaza2_sha256_hex(std::string_view text) {
    return plaza2_sha256_hex(std::as_bytes(std::span{text.data(), text.size()}));
}

} // namespace moex::plaza2::cgate
