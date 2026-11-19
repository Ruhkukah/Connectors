#include "fast_spectra_messages.hpp"
#include "fast_test_encoder.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace moex::fast;
using fast_test::Bytes;
using fast_test::Encoder;

namespace {

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition))                                                                                              \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition);       \
    } while (false)

Reader reader(const Bytes& bytes, std::size_t skip = 0) {
    return {bytes.data() + skip, bytes.data() + bytes.size()};
}

// Byte sequences taken from the FAST 1.1 specification's worked examples, so
// the decoder is checked against something other than this repo's own encoder.
void specification_examples() {
    const auto u = [](std::initializer_list<std::uint8_t> bytes) {
        const Bytes b(bytes);
        Reader r = reader(b);
        const std::uint64_t value = read_u64(r);
        REQUIRE(r.ok && r.p == r.end);
        return value;
    };
    const auto i = [](std::initializer_list<std::uint8_t> bytes) {
        const Bytes b(bytes);
        Reader r = reader(b);
        const std::int64_t value = read_i64(r);
        REQUIRE(r.ok && r.p == r.end);
        return value;
    };
    REQUIRE(u({0x39, 0x45, 0xA3}) == 942755);
    REQUIRE(u({0x80}) == 0);
    REQUIRE(i({0x39, 0x45, 0xA3}) == 942755);
    REQUIRE(i({0x46, 0x3A, 0xDD}) == -942755);
    REQUIRE(i({0x7C, 0x1B, 0x1B, 0x9D}) == -7942755);
    REQUIRE(i({0x00, 0x40, 0x81}) == 8193);
    REQUIRE(i({0x7F, 0x3F, 0xFF}) == -8193);

    const Bytes nullable_int{0x39, 0x45, 0xA4};
    Reader r1 = reader(nullable_int);
    const auto optional_value = read_opt_u32(r1);
    REQUIRE(optional_value.present && optional_value.value == 942755);

    const Bytes null_byte{0x80};
    Reader r2 = reader(null_byte);
    REQUIRE(!read_opt_i64(r2).present);

    const Bytes positive_decimal{0x82, 0x39, 0x45, 0xA3};
    Reader r3 = reader(positive_decimal);
    const Decimal big = read_decimal(r3);
    REQUIRE(big.mantissa == 942755 && big.exponent == 2 && big.to_double() == 94275500.0);

    const Bytes negative_decimal{0xFE, 0x46, 0x3A, 0xDD};
    Reader r4 = reader(negative_decimal);
    const Decimal small = read_decimal(r4);
    REQUIRE(small.mantissa == -942755 && small.exponent == -2 && small.scaled(2) == -942755);
    REQUIRE(small.scaled(4) == -94275500 && small.scaled(0) == -9427);

    const Bytes ascii{0x41, 0x42, 0xC3};
    Reader r5 = reader(ascii);
    const Ascii text = read_ascii(r5);
    REQUIRE(text == "ABC" && text.str() == "ABC" && text.front() == 'A' && text[2] == 'C' && !(text == "ABD"));
}

void integer_boundaries() {
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{127}, std::uint64_t{128}, std::uint64_t{16383},
                                      std::uint64_t{16384}, std::uint64_t{0xFFFFFFFF}, std::uint64_t{1} << 32,
                                      std::numeric_limits<std::uint64_t>::max()}) {
        const Bytes b = Encoder{}.u(value).out;
        Reader r = reader(b);
        REQUIRE(read_u64(r) == value && r.ok && r.p == r.end);
        if (value != std::numeric_limits<std::uint64_t>::max()) {
            const Bytes n = Encoder{}.nu(value).out;
            Reader rn = reader(n);
            const auto decoded = read_opt_u64(rn);
            REQUIRE(decoded.present && decoded.value == value && rn.p == rn.end);
        }
    }
    for (const std::int64_t value :
         {std::int64_t{0}, std::int64_t{-1}, std::int64_t{63}, std::int64_t{64}, std::int64_t{-64}, std::int64_t{-65},
          std::int64_t{8191}, std::int64_t{8192}, std::int64_t{-8192}, std::int64_t{-8193},
          std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()}) {
        const Bytes b = Encoder{}.i(value).out;
        Reader r = reader(b);
        REQUIRE(read_i64(r) == value && r.ok && r.p == r.end);
        if (value != std::numeric_limits<std::int64_t>::max()) {
            const Bytes n = Encoder{}.ni(value).out;
            Reader rn = reader(n);
            const auto decoded = read_opt_i64(rn);
            REQUIRE(decoded.present && decoded.value == value && rn.p == rn.end);
        }
    }
}

void strings_and_nulls() {
    const Bytes b = Encoder{}.s("").ns({}).ns("").ns("RTS-12.26").vec("\xD0\x96").ndec({}).ndec({{-5, -1}}).out;
    Reader r = reader(b);
    REQUIRE(read_ascii(r).empty());
    REQUIRE(!read_opt_ascii(r).present);
    const auto empty = read_opt_ascii(r);
    REQUIRE(empty.present && empty.value.empty());
    const auto symbol = read_opt_ascii(r);
    REQUIRE(symbol.present && symbol.value == "RTS-12.26");
    REQUIRE(read_bytes(r) == "\xD0\x96");
    REQUIRE(!read_opt_decimal(r).present);
    const auto decimal = read_opt_decimal(r);
    REQUIRE(decimal.present && decimal.value == (Decimal{-5, -1}) && r.ok && r.p == r.end);
    // Reading past the end reports failure instead of running off the buffer.
    REQUIRE(read_u64(r) == 0 && !r.ok);

    const Bytes oversized = Encoder{}.u(1000).raw('x').out;
    Reader bad = reader(oversized);
    REQUIRE(read_bytes(bad).empty() && !bad.ok);
}

void incremental_message() {
    const Bytes packet = fast_test::incremental(
        77,
        {{0, '0', 3540000ull, 11, 2, fast_test::Dec{1234550, -2}, 40},
         {2, '1', 3540000ull, 12, 5, fast_test::Dec{-15, 0}, {}},
         {0, '2', {}, 9, {}, fast_test::Dec{9, 3}, 1, 184424123456789ull, "Si-12.26", -7, 1005, "2"}},
        1);
    REQUIRE(read_preamble(packet.data()) == 77);
    Reader r = reader(packet, kPreambleSize);
    REQUIRE(read_template_id(r) == DefaultIncrementalRefreshMessage::kTemplateId);
    DefaultIncrementalRefreshMessage message;
    int seen = 0;
    const bool ok = decode(r, message, [&](const auto& m, const auto& e) {
        REQUIRE(m.MsgSeqNum == 77 && m.LastFragment.present && m.LastFragment.value == 1 && m.NoMDEntries == 3);
        if (seen == 0) {
            REQUIRE(e.MDUpdateAction == 0 && e.MDEntryType.front() == '0' && e.SecurityID.value == 3540000);
            REQUIRE(e.RptSeq == 11 && e.MDPriceLevel.value == 2 && e.MDEntrySize.value == 40);
            REQUIRE(e.MDEntryPx.present && e.MDEntryPx.value.to_double() == 12345.5);
            REQUIRE(!e.Symbol.present && !e.TrdType.present && !e.LastPx.present && !e.OrderSide.present);
        } else if (seen == 1) {
            REQUIRE(e.MDUpdateAction == 2 && e.MDEntryType.front() == '1' && e.MDEntryPx.value.mantissa == -15);
            REQUIRE(!e.MDEntrySize.present && e.RptSeq == 12);
        } else {
            REQUIRE(!e.SecurityID.present && e.Symbol.value == "Si-12.26" && e.MDEntryID.value == -7);
            REQUIRE(e.TrdType.value == 1005 && e.OrderSide.value.front() == '2');
            REQUIRE(e.MDEntryTime == 184424123456789ull && e.MDEntryPx.value.to_double() == 9000.0);
        }
        ++seen;
    });
    REQUIRE(ok && seen == 3 && r.p == r.end);

    // Every truncation must be rejected rather than read past the buffer.
    for (std::size_t size = kPreambleSize; size < packet.size(); ++size) {
        Reader cut{packet.data() + kPreambleSize, packet.data() + size};
        DefaultIncrementalRefreshMessage partial;
        const bool id_ok = read_template_id(cut) == DefaultIncrementalRefreshMessage::kTemplateId;
        REQUIRE(!id_ok || !decode(cut, partial, [](const auto&, const auto&) {}));
    }
}

void other_templates() {
    const Bytes reset = fast_test::sequence_reset(9, 1);
    Reader r1 = reader(reset, kPreambleSize);
    SequenceReset reset_message;
    REQUIRE(read_template_id(r1) == SequenceReset::kTemplateId && decode(r1, reset_message));
    REQUIRE(reset_message.MsgSeqNum == 9 && reset_message.NewSeqNo == 1 && SequenceReset::MessageType == "4");

    // SecurityMassStatus: mandatory sequence with a constant inside the group.
    const Bytes mass = Encoder{}.header(5, 37).u(1).u(2).u(111).u(17).u(222).u(18).out;
    Reader r2 = reader(mass, kPreambleSize);
    SecurityMassStatus mass_message;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> statuses;
    REQUIRE(read_template_id(r2) == SecurityMassStatus::kTemplateId);
    REQUIRE(decode(r2, mass_message,
                   [&](const auto&, const auto& e) { statuses.emplace_back(e.SecurityID, e.SecurityTradingStatus); }));
    REQUIRE(mass_message.LastFragment == 1 && statuses.size() == 2 && statuses[1].first == 222 &&
            statuses[1].second == 18);

    // News: unicode byte vectors, optional fields and a text sequence.
    const Bytes news = Encoder{}.header(6, 9).nu({}).ns("N1").nu({}).ns({}).nu(2).vec("Заголовок").ns("D").u(1).vec(
        "body").out;
    Reader r3 = reader(news, kPreambleSize);
    News news_message;
    std::string text;
    REQUIRE(read_template_id(r3) == News::kTemplateId);
    REQUIRE(decode(r3, news_message, [&](const auto&, const auto& e) { text = std::string(e.Text); }));
    REQUIRE(news_message.NewsId.value == "N1" && news_message.Urgency.value == 2 && !news_message.OrigTime.present);
    REQUIRE(news_message.Headline == "Заголовок" && news_message.MarketSegmentID.value == "D" && text == "body");

    // DiscreteAuction: optional sequence, absent then present.
    for (const bool with_group : {false, true}) {
        Encoder e;
        e.header(7, 42).u(1).u(2).u(3).i(-4).u(5).i(6).i(7);
        if (with_group)
            e.nu(1).s("Si");
        else
            e.nu({});
        e.ni(8);
        Reader r4 = reader(e.out, kPreambleSize);
        DiscreteAuction auction;
        int groups = 0;
        REQUIRE(read_template_id(r4) == DiscreteAuction::kTemplateId);
        REQUIRE(decode(r4, auction, [&](const auto&, const auto& g) { groups += g.UnderlyingSymbol == "Si"; }));
        REQUIRE(auction.AuctionID == -4 && auction.NoUnderlyings.present == with_group && groups == with_group);
        REQUIRE(auction.TradePeriodID.value == 8 && r4.p == r4.end);
    }

    REQUIRE(template_name(31) == "DefaultIncrementalRefreshMessage" && template_name(SecurityDefinition::kTemplateId) == "SecurityDefinition");
    REQUIRE(template_name(12345).empty() && kTemplatesSha256.size() == 64);
}

// Arbitrary bytes through every decoder: must terminate without touching memory
// outside the buffer (the sanitizer build enforces the latter).
void random_input() {
    std::uint64_t state = 0x243F6A8885A308D3ull;
    const auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    static constexpr std::uint32_t kIds[] = {31, 32, 47, 4, 48, 37, 45, 6, 7, 46, 42, 9, 29, 30, 33, 34, 35, 1000, 1001};
    const auto ignore = [](const auto&, const auto&) {};
    std::uint64_t accepted = 0;
    for (int iteration = 0; iteration < 200000; ++iteration) {
        std::uint8_t buffer[96];
        const std::size_t size = next() % sizeof(buffer);
        for (std::size_t index = 0; index < size; ++index) {
            const std::uint64_t value = next();
            // Bias toward stop bits so decoding gets deep into the templates.
            buffer[index] = static_cast<std::uint8_t>(value) | ((value >> 8) % 3 ? 0x80 : 0);
        }
        Reader r{buffer, buffer + size};
        bool ok = false;
#define FAST_CASE_PLAIN(Type)                                                                                          \
    case Type::kTemplateId: {                                                                                          \
        Type m;                                                                                                        \
        ok = decode(r, m);                                                                                             \
        break;                                                                                                         \
    }
#define FAST_CASE_GROUPS(Type)                                                                                         \
    case Type::kTemplateId: {                                                                                          \
        Type m;                                                                                                        \
        ok = decode(r, m, ignore);                                                                                     \
        break;                                                                                                         \
    }
        switch (kIds[next() % std::size(kIds)]) {
            FAST_CASE_GROUPS(DefaultIncrementalRefreshMessage)
            FAST_CASE_GROUPS(DefaultSnapshotMessage)
            FAST_CASE_GROUPS(SecurityDefinition)
            FAST_CASE_PLAIN(SecurityDefinitionUpdateReport)
            FAST_CASE_PLAIN(SecurityStatus)
            FAST_CASE_GROUPS(SecurityMassStatus)
            FAST_CASE_PLAIN(SecurityGroupStatus)
            FAST_CASE_PLAIN(Heartbeat)
            FAST_CASE_PLAIN(SequenceReset)
            FAST_CASE_PLAIN(TradingSessionStatus)
            FAST_CASE_GROUPS(DiscreteAuction)
            FAST_CASE_GROUPS(News)
            FAST_CASE_GROUPS(OrdersLogMessage)
            FAST_CASE_GROUPS(BookMessage)
            FAST_CASE_GROUPS(OtcMonitorIncrementalRefreshMessage)
            FAST_CASE_GROUPS(OtcMonitorSnapshotMessage)
            FAST_CASE_GROUPS(OtcMonitorSecurityDefinition)
            FAST_CASE_PLAIN(Logon)
            FAST_CASE_PLAIN(Logout)
        }
        REQUIRE(r.p <= r.end && ok == (ok && r.ok));
        accepted += ok;
    }
    REQUIRE(accepted > 0);
}

} // namespace

int main() {
    try {
        specification_examples();
        integer_boundaries();
        strings_and_nulls();
        incremental_message();
        other_templates();
        random_input();
    } catch (const std::exception& error) {
        std::cerr << "fast_decoder_test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
