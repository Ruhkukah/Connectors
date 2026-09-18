#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace moex::plaza2::cgate::text {

// SPECTRA fixed strings are Windows-1251, decoded exactly once at the runtime
// boundary. Never infer encoding from content. Raw callback payloads retain
// their original bytes. Undefined CP1251 byte 0x98 becomes U+FFFD.
[[nodiscard]] inline std::string windows1251_to_utf8(std::string_view raw) {
    constexpr std::array<std::uint16_t, 64> upper = {
        0x0402, 0x0403, 0x201a, 0x0453, 0x201e, 0x2026, 0x2020, 0x2021, 0x20ac, 0x2030, 0x0409, 0x2039, 0x040a,
        0x040c, 0x040b, 0x040f, 0x0452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0xfffd, 0x2122,
        0x0459, 0x203a, 0x045a, 0x045c, 0x045b, 0x045f, 0x00a0, 0x040e, 0x045e, 0x0408, 0x00a4, 0x0490, 0x00a6,
        0x00a7, 0x0401, 0x00a9, 0x0404, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x0407, 0x00b0, 0x00b1, 0x0406, 0x0456,
        0x0491, 0x00b5, 0x00b6, 0x00b7, 0x0451, 0x2116, 0x0454, 0x00bb, 0x0458, 0x0405, 0x0455, 0x0457};
    std::string out;
    out.reserve(raw.size());
    for (const unsigned char byte : raw) {
        const std::uint32_t cp = byte < 0x80 ? byte : (byte >= 0xc0 ? 0x0410 + byte - 0xc0 : upper[byte - 0x80]);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}

[[nodiscard]] inline std::string spectra_fixed_string_to_utf8(std::string_view raw) {
    return windows1251_to_utf8(raw.substr(0, raw.find('\0')));
}

// Returns zero for malformed, overlong, surrogate or out-of-range sequences.
[[nodiscard]] inline std::size_t utf8_sequence_size(std::string_view value, std::size_t offset) {
    if (offset >= value.size())
        return 0;
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first < 0x80)
        return 1;
    const std::size_t size = first >= 0xc2 && first <= 0xdf   ? 2
                             : first >= 0xe0 && first <= 0xef ? 3
                             : first >= 0xf0 && first <= 0xf4 ? 4
                                                              : 0;
    if (size == 0 || value.size() - offset < size)
        return 0;
    std::uint32_t cp = first & (0x7fU >> size);
    for (std::size_t i = 1; i < size; ++i) {
        const auto byte = static_cast<unsigned char>(value[offset + i]);
        if ((byte & 0xc0) != 0x80)
            return 0;
        cp = (cp << 6) | (byte & 0x3f);
    }
    if ((size == 2 && cp < 0x80) || (size == 3 && cp < 0x800) || (size == 4 && cp < 0x10000) ||
        (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
        return 0;
    return size;
}

[[nodiscard]] inline bool valid_utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto size = utf8_sequence_size(value, i);
        if (size == 0)
            return false;
        i += size;
    }
    return true;
}

// Input is already UTF-8, never CP1251. Escape each malformed byte as U+FFFD
// without guessing its encoding; valid non-ASCII UTF-8 stays byte-equivalent.
[[nodiscard]] inline std::string json_escape_utf8(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < value.size();) {
        const auto size = utf8_sequence_size(value, i);
        if (size == 0) {
            out += "\\ufffd";
            ++i;
            continue;
        }
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte == '"' || byte == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(byte));
        } else if (byte < 0x20) {
            out += "\\u00";
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0xf]);
        } else {
            out.append(value.substr(i, size));
        }
        i += size;
    }
    return out;
}

} // namespace moex::plaza2::cgate::text
