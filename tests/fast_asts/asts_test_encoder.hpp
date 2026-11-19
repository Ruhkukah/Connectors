#pragma once

// Test-only encoder for ASTS FAST messages, written independently of the
// decoder under test. Sequence entries carry a presence map; a copy field is
// either written (bit set) or left to repeat the previous entry's value.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asts_test {

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
        while (count-- > 0)
            raw(groups[count] | (count == 0 ? 0x80 : 0));
        return *this;
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
        while (count-- > 0)
            raw(groups[count] | (count == 0 ? 0x80 : 0));
        return *this;
    }
    Encoder& nu(std::optional<std::uint64_t> value) {
        return value ? u(*value + 1) : raw(0x80);
    }
    Encoder& ni(std::optional<std::int64_t> value) {
        return value ? i(*value >= 0 ? *value + 1 : *value) : raw(0x80);
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
        return value->empty() ? raw(0x00).raw(0x80) : s(*value);
    }
    Encoder& ndec(std::optional<Dec> value) {
        return value ? ni(value->second).i(value->first) : raw(0x80);
    }
    Encoder& raw(std::uint8_t byte) {
        out.push_back(byte);
        return *this;
    }
    // Presence map from bits in field order, padded to whole 7-bit groups.
    Encoder& pmap(const std::vector<bool>& bits) {
        std::size_t groups = (bits.size() + 6) / 7;
        if (groups == 0)
            groups = 1;
        for (std::size_t group = 0; group < groups; ++group) {
            std::uint8_t byte = 0;
            for (std::size_t bit = 0; bit < 7; ++bit)
                if (group * 7 + bit < bits.size() && bits[group * 7 + bit])
                    byte |= static_cast<std::uint8_t>(0x40 >> bit);
            raw(byte | (group + 1 == groups ? 0x80 : 0));
        }
        return *this;
    }
    // Preamble, message presence map (template-id bit), template id, MsgSeqNum, SendingTime.
    // SendingTime advances with every message built, as it does on the wire: the same
    // snapshot in two cycles is never byte-identical, while a feed A/B pair is.
    Encoder& header(std::uint32_t seq, std::uint32_t template_id) {
        static std::uint64_t sending_time = 20261120100000000ull;
        for (int shift = 0; shift < 32; shift += 8)
            raw(static_cast<std::uint8_t>(seq >> shift));
        return raw(0xC0).u(template_id).u(seq).u(++sending_time);
    }
};

// One X-OLR-CURR entry. Copy fields left empty repeat the previous entry of the message.
struct Order {
    std::optional<std::uint64_t> action; // copy
    std::optional<char> type;            // copy
    std::string id;
    std::optional<std::string> symbol; // copy
    std::int32_t rpt_seq{};
    std::optional<Dec> price; // copy
    std::optional<Dec> size;  // copy
    std::optional<std::string> board; // copy
    bool null_size{};                 // write an explicit NULL size (as deletes do) instead of repeating
};

inline Bytes orders(std::uint32_t seq, const std::vector<Order>& entries) {
    Encoder e;
    e.header(seq, 3710).u(entries.size());
    for (const Order& order : entries) {
        const bool size_bit = order.size.has_value() || order.null_size;
        // Presence bits, in template order, for: MDUpdateAction MDEntryType Symbol MDEntryPx MDEntrySize
        // MDEntryDate MDEntryTime OrigTime OrderStatus TradingSessionID TradingSessionSubID TradingSession
        // SecurityStatistics.
        e.pmap({order.action.has_value(), order.type.has_value(), order.symbol.has_value(), order.price.has_value(),
                size_bit, false, false, false, false, order.board.has_value(), false, false, false});
        if (order.action)
            e.nu(*order.action);
        if (order.type) {
            const char type[1] = {*order.type};
            e.ns(std::string_view(type, 1));
        }
        e.ns(std::string_view(order.id));
        if (order.symbol)
            e.ns(std::string_view(*order.symbol));
        e.ni(order.rpt_seq);
        if (order.price)
            e.ndec(order.price);
        if (size_bit)
            e.ndec(order.size);
        if (order.board)
            e.ns(std::string_view(*order.board));
    }
    return e.out;
}

struct SnapshotOrder {
    char type{'0'};
    std::string id;
    Dec price{};
    Dec size{};
};

// One W-OLS-CURR message. An empty list is sent as a single Empty Book entry, as the gate does.
inline Bytes order_snapshot(std::uint32_t seq, std::string_view symbol, std::string_view board, std::int32_t rpt_seq,
                            const std::vector<SnapshotOrder>& entries, bool first = true, bool last = true,
                            std::uint32_t last_processed = 0) {
    Encoder e;
    e.header(seq, 3700).nu(last_processed).i(rpt_seq).nu(last ? 1 : 0).nu(first ? 1 : 0).ni({}).ns(board).s(symbol);
    e.ni({});
    // Presence bits for: MDEntryType MDEntryDate MDEntryTime OrigTime MDEntryPx MDEntrySize OrderStatus
    // TradingSessionSubID.
    if (entries.empty()) {
        e.u(1).pmap({true, false, false, false, false, false, false, false}).ns("J").ns({}).ns({}).nu({});
        return e.out;
    }
    e.u(entries.size());
    for (const SnapshotOrder& order : entries) {
        const char type[1] = {order.type};
        e.pmap({true, false, false, false, true, true, true, false});
        e.ns(std::string_view(type, 1)).ns(std::string_view(order.id)).ndec(order.price).ndec(order.size).ns("O");
        e.ns({}).nu({});
    }
    return e.out;
}

inline Bytes heartbeat(std::uint32_t seq) {
    return Encoder{}.header(seq, 2108).out;
}

} // namespace asts_test
