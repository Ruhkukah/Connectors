#pragma once

// FAST 1.1 wire primitives for the MOEX Spectra FAST gate.
//
// The Spectra templates use only mandatory constants and operator-less fields,
// so decoding is a straight walk over stop-bit encoded values: no dictionaries,
// no per-field presence-map bits. Everything here is allocation-free and
// bounds-checked through one sticky error flag instead of per-field branches
// in the caller.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace moex::fast {

struct Reader {
    const std::uint8_t* p{};
    const std::uint8_t* end{};
    bool ok{true};

    [[nodiscard]] std::size_t remaining() const noexcept {
        return static_cast<std::size_t>(end - p);
    }
    std::uint64_t fail() noexcept {
        p = end;
        ok = false;
        return 0;
    }
};

template <class T> struct Opt {
    T value{};
    bool present{};

    explicit operator bool() const noexcept {
        return present;
    }
    [[nodiscard]] T value_or(T fallback) const noexcept {
        return present ? value : fallback;
    }
};

// ASCII field viewed in place. The final byte still carries the stop bit, so
// read characters through this type rather than through the raw pointer.
struct Ascii {
    const std::uint8_t* data{};
    std::uint32_t length{};

    [[nodiscard]] std::size_t size() const noexcept {
        return length;
    }
    [[nodiscard]] bool empty() const noexcept {
        return length == 0;
    }
    [[nodiscard]] char operator[](std::size_t index) const noexcept {
        return static_cast<char>(data[index] & 0x7F);
    }
    // First character, or 0 for an empty field. MDEntryType and friends are single characters.
    [[nodiscard]] char front() const noexcept {
        return length ? static_cast<char>(data[0] & 0x7F) : '\0';
    }
    [[nodiscard]] bool operator==(std::string_view other) const noexcept {
        if (other.size() != length)
            return false;
        if (length == 0)
            return true;
        return std::memcmp(data, other.data(), length - 1) == 0 && (*this)[length - 1] == other[length - 1];
    }
    // Copies at most capacity characters; returns the count written. No terminator is added.
    std::size_t copy(char* out, std::size_t capacity) const noexcept {
        const std::size_t count = length < capacity ? length : capacity;
        for (std::size_t i = 0; i < count; ++i)
            out[i] = (*this)[i];
        return count;
    }
    [[nodiscard]] std::string str() const {
        std::string out(length, '\0');
        copy(out.data(), length);
        return out;
    }
};

struct Decimal {
    std::int64_t mantissa{};
    std::int32_t exponent{};

    [[nodiscard]] double to_double() const noexcept {
        static constexpr double kPow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
                                            1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
        const auto value = static_cast<double>(mantissa);
        if (exponent >= 0)
            return exponent <= 18 ? value * kPow10[exponent] : value * 1e18 * kPow10[18];
        return exponent >= -18 ? value / kPow10[-exponent] : 0.0;
    }
    // Mantissa rescaled to 10^-decimals units, truncating toward zero when precision is lost.
    [[nodiscard]] std::int64_t scaled(std::int32_t decimals) const noexcept {
        std::int64_t value = mantissa;
        for (std::int32_t shift = exponent + decimals; shift > 0 && value != 0; --shift)
            value *= 10;
        for (std::int32_t shift = exponent + decimals; shift < 0 && value != 0; ++shift)
            value /= 10;
        return value;
    }
    friend bool operator==(const Decimal&, const Decimal&) = default;
};

[[nodiscard]] inline std::uint64_t read_u64(Reader& r) noexcept {
    std::uint64_t value = 0;
    while (r.p < r.end) {
        const std::uint8_t byte = *r.p++;
        value = (value << 7) | (byte & 0x7Fu);
        if (byte & 0x80u)
            return value;
    }
    return r.fail();
}

[[nodiscard]] inline std::int64_t read_i64(Reader& r) noexcept {
    if (r.p >= r.end)
        return static_cast<std::int64_t>(r.fail());
    std::uint64_t value = (*r.p & 0x40u) ? ~std::uint64_t{0} : 0;
    while (r.p < r.end) {
        const std::uint8_t byte = *r.p++;
        value = (value << 7) | (byte & 0x7Fu);
        if (byte & 0x80u)
            return static_cast<std::int64_t>(value);
    }
    return static_cast<std::int64_t>(r.fail());
}

[[nodiscard]] inline std::uint32_t read_u32(Reader& r) noexcept {
    return static_cast<std::uint32_t>(read_u64(r));
}
[[nodiscard]] inline std::int32_t read_i32(Reader& r) noexcept {
    return static_cast<std::int32_t>(read_i64(r));
}

// Nullable integers: 0 is NULL, non-negative values travel incremented by one.
[[nodiscard]] inline Opt<std::uint64_t> read_opt_u64(Reader& r) noexcept {
    const std::uint64_t raw = read_u64(r);
    return {raw - (raw != 0), raw != 0};
}
[[nodiscard]] inline Opt<std::uint32_t> read_opt_u32(Reader& r) noexcept {
    const std::uint64_t raw = read_u64(r);
    return {static_cast<std::uint32_t>(raw - (raw != 0)), raw != 0};
}
[[nodiscard]] inline Opt<std::int64_t> read_opt_i64(Reader& r) noexcept {
    const std::int64_t raw = read_i64(r);
    return {raw - (raw > 0), raw != 0};
}
[[nodiscard]] inline Opt<std::int32_t> read_opt_i32(Reader& r) noexcept {
    const std::int64_t raw = read_i64(r);
    return {static_cast<std::int32_t>(raw - (raw > 0)), raw != 0};
}

namespace detail {
// Returns the raw stop-bit terminated run, including any zero preamble.
[[nodiscard]] inline Ascii read_ascii_run(Reader& r) noexcept {
    const std::uint8_t* start = r.p;
    while (r.p < r.end) {
        if (*r.p++ & 0x80u)
            return {start, static_cast<std::uint32_t>(r.p - start)};
    }
    r.fail();
    return {};
}
} // namespace detail

[[nodiscard]] inline Ascii read_ascii(Reader& r) noexcept {
    const Ascii run = detail::read_ascii_run(r);
    return run.length == 1 && run.data[0] == 0x80u ? Ascii{} : run;
}

[[nodiscard]] inline Opt<Ascii> read_opt_ascii(Reader& r) noexcept {
    const Ascii run = detail::read_ascii_run(r);
    if (run.length == 1 && run.data[0] == 0x80u)
        return {{}, false};
    if (run.length == 2 && run.data[0] == 0x00u && run.data[1] == 0x80u)
        return {{}, r.ok};
    return {run, r.ok};
}

// charset="unicode" fields are length-prefixed byte vectors (UTF-8).
[[nodiscard]] inline std::string_view read_bytes(Reader& r) noexcept {
    const std::uint64_t length = read_u64(r);
    if (length > r.remaining()) {
        r.fail();
        return {};
    }
    const std::string_view out(reinterpret_cast<const char*>(r.p), static_cast<std::size_t>(length));
    r.p += length;
    return out;
}

[[nodiscard]] inline Opt<std::string_view> read_opt_bytes(Reader& r) noexcept {
    const std::uint64_t raw = read_u64(r);
    if (raw == 0)
        return {{}, false};
    if (raw - 1 > r.remaining()) {
        r.fail();
        return {{}, false};
    }
    const std::string_view out(reinterpret_cast<const char*>(r.p), static_cast<std::size_t>(raw - 1));
    r.p += raw - 1;
    return {out, true};
}

[[nodiscard]] inline Decimal read_decimal(Reader& r) noexcept {
    const std::int32_t exponent = read_i32(r);
    return {read_i64(r), exponent};
}

// A NULL exponent means the whole decimal is absent and no mantissa follows.
[[nodiscard]] inline Opt<Decimal> read_opt_decimal(Reader& r) noexcept {
    const Opt<std::int32_t> exponent = read_opt_i32(r);
    if (!exponent.present)
        return {{}, false};
    return {{read_i64(r), exponent.value}, true};
}

// Sequence lengths are validated against the bytes left: every entry costs at
// least one byte, so a hostile length cannot drive a long loop.
[[nodiscard]] inline std::uint32_t read_length(Reader& r) noexcept {
    const std::uint64_t length = read_u64(r);
    return length > r.remaining() ? static_cast<std::uint32_t>(r.fail()) : static_cast<std::uint32_t>(length);
}
[[nodiscard]] inline Opt<std::uint32_t> read_opt_length(Reader& r) noexcept {
    const Opt<std::uint64_t> length = read_opt_u64(r);
    if (length.present && length.value > r.remaining()) {
        r.fail();
        return {};
    }
    return {static_cast<std::uint32_t>(length.value), length.present};
}

// Spectra prefixes every UDP datagram with MsgSeqNum as a little-endian uint32.
inline constexpr std::size_t kPreambleSize = 4;

[[nodiscard]] inline std::uint32_t read_preamble(const std::uint8_t* packet) noexcept {
    return static_cast<std::uint32_t>(packet[0]) | static_cast<std::uint32_t>(packet[1]) << 8 |
           static_cast<std::uint32_t>(packet[2]) << 16 | static_cast<std::uint32_t>(packet[3]) << 24;
}

// Consumes the presence map and template id. Returns 0 on malformed input or
// when the message omits its template id, which the Spectra gate never does.
[[nodiscard]] inline std::uint32_t read_template_id(Reader& r) noexcept {
    if (r.p >= r.end || !(*r.p & 0x40u)) {
        r.fail();
        return 0;
    }
    while (r.p < r.end && !(*r.p & 0x80u))
        ++r.p;
    if (r.p >= r.end) {
        r.fail();
        return 0;
    }
    ++r.p;
    const std::uint32_t id = read_u32(r);
    return r.ok ? id : 0;
}

} // namespace moex::fast
