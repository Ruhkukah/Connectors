#include "moex/plaza2/cgate/plaza2_public_decode.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace moex::plaza2;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

int main() {
    try {
        // Exact base-100 vectors independently checked with the official SDK.
        const public_wire::Bcd16_5 positive{5, 16, 0, 0, 0, 0, 0, 12, 34, 56, 70};
        auto negative = positive;
        negative[2] |= 0x80U;
        const public_wire::Bcd16_5 maximum{5, 16, 9, 99, 99, 99, 99, 99, 99, 99, 90};
        require(public_wire::decimal_scaled(positive) == 1234567, "exact decimal");
        require(public_wire::decimal_scaled(negative) == -1234567, "negative decimal");
        require(public_wire::decimal_scaled(maximum) == 9999999999999999LL, "maximum decimal");
        auto malformed = positive;
        malformed[4] = 101;
        require(!public_wire::decimal_scaled(malformed), "invalid base-100 digit");
        for (const auto& table : public_wire::kTables) {
            std::array<std::byte, 152> storage{};
            std::array<std::uint8_t, 19> presence{};
            auto bytes = std::span(storage).first(table.size);
            auto nulls = std::span(presence).first(table.fields.size());
            std::size_t ordinal = 0;
            for (const auto& field : table.fields) {
                require(field.offset + field.size <= table.size, "field boundary");
                if (field.type == "d16.5") {
                    std::memcpy(bytes.data() + field.offset, positive.data(), positive.size());
                } else if (field.type == "i8") {
                    const auto value = std::numeric_limits<std::int64_t>::max();
                    std::memcpy(bytes.data() + field.offset, &value, sizeof(value));
                    require(public_wire::load<std::int64_t>(bytes, field.offset) == value, "maximum quantity/flags");
                } else if (field.type == "u8") {
                    const auto value = std::numeric_limits<std::uint64_t>::max();
                    std::memcpy(bytes.data() + field.offset, &value, sizeof(value));
                    require(public_wire::load<std::uint64_t>(bytes, field.offset) == value, "full moment_ns width");
                } else if (field.type == "t") {
                    const public_wire::Time time{2026, 9, 7, 23, 59, 58, 999};
                    std::memcpy(bytes.data() + field.offset, &time, sizeof(time));
                    const auto decoded = public_wire::load<public_wire::Time>(bytes, field.offset);
                    require(decoded.msec == 999 && decoded.year == 2026 && decoded.second == 58, "exact timestamp");
                } else if (field.type == "i1") {
                    bytes[field.offset] = std::byte{0xFF};
                    require(public_wire::load<std::int8_t>(bytes, field.offset) == -1,
                            "unknown action/status retained");
                }
                ++ordinal;
            }
            require(public_wire::validate(table, bytes, nulls) == public_wire::DecodeResult::Ok, "all tables decode");
            require(public_wire::validate(table, bytes.first(bytes.size() - 1), {}) == public_wire::DecodeResult::Size,
                    "truncated record");
            for (std::size_t i = 3; i < table.fields.size(); ++i) {
                nulls[i] = 1;
            }
            require(public_wire::validate(table, bytes, nulls) == public_wire::DecodeResult::Ok,
                    "preserve null fields");
            nulls[0] = 1;
            require(public_wire::validate(table, bytes, nulls) == public_wire::DecodeResult::NullMap,
                    "null replication ID");
        }
        std::cout << "10 public tables: layouts, timestamps, nulls, decimals and raw flags PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
