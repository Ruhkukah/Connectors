#include <cgate.h>
#include "moex/plaza2/cgate/plaza2_public_decode.hpp"
#include "moex/plaza2/cgate/plaza2_fixed_point.hpp"
#include <array>
#include <cstdio>
#include <string_view>
using moex::plaza2::public_wire::Bcd16_5;

namespace {

Bcd16_5 encode_d16_5(std::int64_t n) {
    const auto magnitude = n < 0 ? static_cast<std::uint64_t>(-(n + 1)) + 1U : static_cast<std::uint64_t>(n);
    auto bytes = Bcd16_5{5, 16};
    auto value = magnitude * 10U;
    for (std::size_t index = bytes.size(); index-- > 2;)
        bytes[index] = static_cast<std::uint8_t>(value % 100U), value /= 100U;
    if (n < 0)
        bytes[2] |= 0x80U;
    return bytes;
}

std::string_view cg_text(std::array<char, 128>& buffer, std::size_t size) {
    if (size == 0 || size > buffer.size())
        return {};
    std::size_t length = 0;
    while (length < size && buffer[length] != '\0')
        ++length;
    return {buffer.data(), length};
}

} // namespace

int main() {
    if (cg_env_open(""))
        return 1;
    unsigned long long state = 1;
    unsigned count = 0;
    const auto verify = [&](std::int64_t n) {
        const auto bytes = encode_d16_5(n);
        std::int64_t native = 0;
        std::int8_t scale = 0;
        if (cg_bcd_get(bytes.data(), &native, &scale) || native != n ||
            scale != static_cast<std::int8_t>(moex::plaza2::cgate::kPlaza2D16_5FractionalDigits))
            return false;
        const auto exact = moex::plaza2::public_wire::decimal_value(bytes);
        if (!exact.has_value() || exact->mantissa != n ||
            exact->scale != static_cast<std::int32_t>(moex::plaza2::cgate::kPlaza2D16_5FractionalDigits))
            return false;

        std::array<char, 128> buffer{};
        std::size_t buffer_size = buffer.size();
        if (cg_getstr("d16.5", bytes.data(), buffer.data(), &buffer_size) != 0)
            return false;
        const auto text = cg_text(buffer, buffer_size);
        const auto parsed =
            moex::plaza2::cgate::parse_fixed_point(text, moex::plaza2::cgate::kPlaza2D16_5FractionalDigits, true,
                                                   moex::plaza2::cgate::kPlaza2D16_5DecimalPrecision);
        return parsed.has_value() && *parsed == n;
    };
    for (const auto n : std::array<std::int64_t, 7>{0, 1, -1, 10'050'000, -10'050'000, 9'999'999'999'999'999LL,
                                                    -9'999'999'999'999'999LL}) {
        if (!verify(n)) {
            cg_env_close();
            return 2;
        }
        ++count;
    }
    for (unsigned i = 0; i < 100004; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        long long n = i == 0   ? 0
                      : i == 1 ? 1
                      : i == 2 ? 9999999999999999LL
                               : static_cast<long long>(state % 10000000000000000ULL);
        if (i & 1)
            n = -n;
        if (!verify(n)) {
            cg_env_close();
            return 2;
        }
        ++count;
    }
    cg_env_close();
    printf("{\"official_cg_bcd_get_cases\":%u,\"failed\":0,\"network\":\"none\"}\n", count);
}
