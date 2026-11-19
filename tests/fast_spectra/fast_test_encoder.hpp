#pragma once

// Test-only FAST encoder, written independently of the decoder under test.

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace fast_test {

using Bytes = std::vector<std::uint8_t>;
using Dec = std::pair<std::int64_t, std::int32_t>; // mantissa, exponent

struct Encoder {
    Bytes out;

    Encoder& u(std::uint64_t value) {
        std::uint8_t groups[10];
        int count = 0;
        do {
            groups[count++] = value & 0x7F;
            value >>= 7;
        } while (value);
        return flush(groups, count);
    }
    Encoder& i(std::int64_t value) {
        std::uint8_t groups[10];
        int count = 0;
        for (;;) {
            const std::uint8_t group = value & 0x7F;
            value >>= 7;
            groups[count++] = group;
            if ((value == 0 && !(group & 0x40)) || (value == -1 && (group & 0x40)))
                break;
        }
        return flush(groups, count);
    }
    Encoder& nu(std::optional<std::uint64_t> value) {
        if (!value)
            return raw(0x80);
        return u(*value + 1);
    }
    Encoder& ni(std::optional<std::int64_t> value) {
        if (!value)
            return raw(0x80);
        return i(*value >= 0 ? *value + 1 : *value);
    }
    Encoder& s(std::string_view value) {
        if (value.empty())
            return raw(0x80);
        for (std::size_t index = 0; index < value.size(); ++index)
            raw(static_cast<std::uint8_t>(value[index]) | (index + 1 == value.size() ? 0x80 : 0));
        return *this;
    }
    Encoder& ns(std::optional<std::string_view> value) {
        if (!value)
            return raw(0x80);
        if (value->empty())
            return raw(0x00).raw(0x80);
        return s(*value);
    }
    Encoder& dec(Dec value) {
        return i(value.second).i(value.first);
    }
    Encoder& ndec(std::optional<Dec> value) {
        if (!value)
            return raw(0x80);
        return ni(value->second).i(value->first);
    }
    Encoder& vec(std::string_view value) {
        u(value.size());
        out.insert(out.end(), value.begin(), value.end());
        return *this;
    }
    Encoder& raw(std::uint8_t byte) {
        out.push_back(byte);
        return *this;
    }
    // Preamble, presence map with the template-id bit, template id, MsgSeqNum, SendingTime.
    Encoder& header(std::uint32_t seq, std::uint32_t template_id, std::uint64_t sending_time = 20261120100000000ull) {
        for (int shift = 0; shift < 32; shift += 8)
            raw(static_cast<std::uint8_t>(seq >> shift));
        return raw(0xC0).u(template_id).u(seq).u(sending_time);
    }

  private:
    Encoder& flush(const std::uint8_t* groups, int count) {
        while (count-- > 0)
            raw(groups[count] | (count == 0 ? 0x80 : 0));
        return *this;
    }
};

struct IncrementalEntry {
    std::uint32_t action{};
    char type{'0'};
    std::optional<std::uint64_t> security_id;
    std::uint32_t rpt_seq{};
    std::optional<std::uint64_t> level;
    std::optional<Dec> price;
    std::optional<std::int64_t> size;
    std::uint64_t time{100000000000000ull};
    std::optional<std::string_view> symbol;
    std::optional<std::int64_t> entry_id;
    std::optional<std::int64_t> trd_type;
    std::optional<std::string_view> order_side;
};

inline Bytes incremental(std::uint32_t seq, const std::vector<IncrementalEntry>& entries,
                         std::optional<std::uint64_t> last_fragment = {}) {
    Encoder e;
    e.header(seq, 31).nu(last_fragment).u(entries.size());
    for (const IncrementalEntry& entry : entries) {
        const char type[1] = {entry.type};
        e.u(entry.action).s({type, 1}).nu(entry.security_id).ns(entry.symbol).ns({}).nu({});
        e.u(entry.rpt_seq).nu({}).nu(entry.level).ni(entry.entry_id).ndec(entry.price).ni(entry.size);
        e.nu({}).u(entry.time).ni({}).ns({}).ni(entry.trd_type).ndec({}).ni({}).ns({}).nu({});
        e.ns(entry.order_side).ni({}).ni({}).ndec({}).ns({});
    }
    return e.out;
}

struct SnapshotEntry {
    char type{'0'};
    std::optional<std::uint64_t> level;
    std::optional<Dec> price;
    std::optional<std::int64_t> size;
};

inline Bytes snapshot(std::uint32_t seq, std::optional<std::uint64_t> security_id, std::uint32_t rpt_seq,
                      std::uint32_t last_processed, const std::vector<SnapshotEntry>& entries,
                      std::optional<std::uint64_t> last_fragment = {}) {
    Encoder e;
    e.header(seq, 32).nu(last_fragment).u(rpt_seq).u(1).u(last_processed).nu(security_id).ns({}).ns({});
    e.u(entries.size());
    for (const SnapshotEntry& entry : entries) {
        const char type[1] = {entry.type};
        e.s({type, 1}).nu({}).ni({}).nu({}).ndec(entry.price).nu({}).u(100000000000000ull).ni(entry.size);
        e.nu(entry.level).ni({}).ns({}).ni({}).ni({}).ns({}).ns({}).ni({}).ndec({}).ns({});
    }
    return e.out;
}

inline Bytes heartbeat(std::uint32_t seq) {
    return Encoder{}.header(seq, 6).out;
}

inline Bytes sequence_reset(std::uint32_t seq, std::uint32_t new_seq) {
    return Encoder{}.header(seq, 7).u(new_seq).out;
}

} // namespace fast_test
