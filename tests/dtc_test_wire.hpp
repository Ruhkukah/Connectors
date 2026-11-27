#pragma once

#include <bit>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dtc_test_wire {
using Bytes = std::vector<std::uint8_t>;

inline void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

inline void put_varint(Bytes& out, std::uint64_t value) {
    while (value >= 128) {
        out.push_back(static_cast<std::uint8_t>(value) | 0x80U);
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

inline void put_integer(Bytes& out, unsigned field, std::uint64_t value) {
    put_varint(out, field * 8U);
    put_varint(out, value);
}

inline void put_text(Bytes& out, unsigned field, std::string_view value) {
    put_varint(out, field * 8U + 2U);
    put_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

inline Bytes packet(unsigned type, const Bytes& payload) {
    const auto size = payload.size() + 4;
    require(size <= UINT16_MAX, "test DTC request frame size");
    Bytes out{static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(type),
              static_cast<std::uint8_t>(type >> 8)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

struct WireField {
    unsigned wire{0};
    std::uint64_t integer{0};
    std::uint64_t fixed64{0};
    std::uint32_t fixed32{0};
    std::string text;
};

// Independent protobuf reader used by this test. In particular it preserves
// wire types so a varint accidentally written to a DTC double field is caught.
struct WireMessage {
    std::map<unsigned, WireField> fields;

    explicit WireMessage(const Bytes& bytes) {
        std::size_t pos = 0;
        const auto read_varint = [&]() {
            std::uint64_t value = 0;
            for (unsigned shift = 0; shift < 70; shift += 7) {
                require(pos < bytes.size(), "protobuf varint truncated");
                const auto byte = bytes[pos++];
                if (shift == 63)
                    require(byte <= 1, "protobuf varint overflow");
                value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
                if (!(byte & 0x80U))
                    return value;
            }
            throw std::runtime_error("protobuf varint overflow");
        };

        while (pos < bytes.size()) {
            const auto tag = read_varint();
            const auto number = static_cast<unsigned>(tag >> 3U);
            const auto wire = static_cast<unsigned>(tag & 7U);
            require(number > 0, "protobuf field number zero");
            WireField field;
            field.wire = wire;
            switch (wire) {
            case 0:
                field.integer = read_varint();
                break;
            case 1:
                require(bytes.size() - pos >= 8, "protobuf fixed64 truncated");
                for (unsigned i = 0; i < 8; ++i)
                    field.fixed64 |= static_cast<std::uint64_t>(bytes[pos++]) << (i * 8U);
                break;
            case 2: {
                const auto length = read_varint();
                require(length <= bytes.size() - pos, "protobuf text truncated");
                field.text.assign(reinterpret_cast<const char*>(bytes.data() + pos), static_cast<std::size_t>(length));
                pos += static_cast<std::size_t>(length);
                break;
            }
            case 5:
                require(bytes.size() - pos >= 4, "protobuf fixed32 truncated");
                for (unsigned i = 0; i < 4; ++i)
                    field.fixed32 |= static_cast<std::uint32_t>(bytes[pos++]) << (i * 8U);
                break;
            default:
                throw std::runtime_error("unsupported protobuf wire type");
            }
            fields[number] = std::move(field);
        }
    }

    bool has(unsigned field) const {
        return fields.contains(field);
    }
    std::uint64_t number(unsigned field) const {
        const auto found = fields.find(field);
        require(found != fields.end() && found->second.wire == 0, "expected protobuf varint field");
        return found->second.integer;
    }
    std::string_view string(unsigned field) const {
        const auto found = fields.find(field);
        require(found != fields.end() && found->second.wire == 2, "expected protobuf string field");
        return found->second.text;
    }
    double real64(unsigned field) const {
        const auto found = fields.find(field);
        require(found != fields.end() && found->second.wire == 1, "expected protobuf double field");
        return std::bit_cast<double>(found->second.fixed64);
    }
};
} // namespace dtc_test_wire
