#include "moex/connector_host/dtc_read_only_server.hpp"
#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2/cgate/plaza2_public_deals.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace moex::connector_host::dtc;
using Deal = moex::plaza2::cgate::Plaza2PublicDeal;
using Deals = moex::plaza2::cgate::Plaza2PublicDealsSnapshot;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kSymbol = "ALRS-12.26";
constexpr std::string_view kExchange = "MOEX_SPECTRA";
constexpr std::int64_t kIsin = 4519450;
constexpr std::int32_t kSession = 11715;
constexpr std::uint32_t kSymbolId = 17;
constexpr std::uint64_t kActiveSide = 0x20000000000ULL;
constexpr std::uint64_t kPassiveSide = 0x40000000000ULL;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void put_varint(Bytes& out, std::uint64_t value) {
    while (value >= 128) {
        out.push_back(static_cast<std::uint8_t>(value) | 0x80U);
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_integer(Bytes& out, unsigned field, std::uint64_t value) {
    put_varint(out, field * 8U);
    put_varint(out, value);
}

void put_text(Bytes& out, unsigned field, std::string_view value) {
    put_varint(out, field * 8U + 2U);
    put_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

Bytes packet(unsigned type, const Bytes& payload) {
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

struct FakeSource final : DtcMarketDataSource {
    DtcMarketDataSnapshot state;
    Deals deals;
    bool publisher{true};
    mutable std::vector<std::uint64_t> after_sequence_calls;

    FakeSource() {
        state.symbol = std::string(kSymbol);
        state.underlying_board = "RFUD";
        state.min_step = "1";
        state.isin_id = kIsin;
        state.session_id = kSession;
        state.refdata_metadata_current = true;
        state.valid = true;
        state.transport_active = true;
        state.source_online = true;
        state.aggr_online = true;
        state.snapshot_complete = true;
        state.book_snapshot_current = true;
        state.market_data_display_allowed = true;
        state.session_ready_witness_kind =
            moex::connector_host::dtc::SessionReadyWitnessKind::LateJoinCorroboratedSnapshot;
        state.market_data_authority_epoch = 1;
        state.target_authoritative = false; // Synthetic provisional display, never exchange confirmation.
        state.target_is_future = true;
        state.refdata_vcb_join_current = true;
        state.future_vcb_provenance_present = true;
        state.refdata_board_proven = true;
        state.refdata_currency_proven = true;
        state.future_vcb_base_contract_code = "ALRS";
        state.future_vcb_base_contract_id = 42;
        namespace generated = moex::plaza2::generated;
        const auto provenance = [](generated::TableCode table, std::int64_t revision) {
            return moex::plaza2::private_state::SourceRowProvenance{.stream_code =
                                                                        generated::StreamCode::kFortsRefdataRepl,
                                                                    .table_code = table,
                                                                    .repl_rev = revision,
                                                                    .lifenum = 12,
                                                                    .present = true};
        };
        state.definition_source_provenance = provenance(generated::TableCode::kFortsRefdataReplFutInstruments, 73);
        state.future_instruments_provenance = state.definition_source_provenance;
        state.future_sess_contents_provenance = provenance(generated::TableCode::kFortsRefdataReplFutSessContents, 74);
        state.session_provenance = provenance(generated::TableCode::kFortsRefdataReplSession, 76);
        state.future_vcb_provenance = provenance(generated::TableCode::kFortsRefdataReplFutVcb, 77);
        state.future_vcb_repl_rev = 77;
        state.future_vcb_lifenum = 12;
        state.description = "OFFLINE SYNTHETIC future in LiveTest mode; not exchange data";
        state.currency = "RUB";
        state.contract_size = "100";
        state.currency_value_per_increment = "1";
        state.stream_epoch = 73;
        state.source_snapshot_version = 1;
        state.snapshot_watermark = 1;
        state.source_snapshot_hash = 12345;
        state.levels = {{.price_scaled = 10000000,
                         .volume = 2,
                         .side = DtcDepthSide::Bid,
                         .source_row_id = 8001,
                         .source_sequence = 1},
                        {.price_scaled = 10100000,
                         .volume = 3,
                         .side = DtcDepthSide::Ask,
                         .source_row_id = 8002,
                         .source_sequence = 2}};
        deals.online = true;
        deals.valid = true;
        deals.stream_epoch = 91;
        deals.lifenum = 44;
    }

    DtcMarketDataSnapshot snapshot() const override {
        return state;
    }

    DtcReadOnlyCapabilities capabilities() const noexcept override {
        return {.market_data = publisher,
                .market_depth = true,
                .security_definitions = true,
                .accounts = false,
                .positions = false,
                .orders = false,
                .order_entry = false};
    }

    Deals public_deals(std::uint64_t after_sequence = 0) const override {
        after_sequence_calls.push_back(after_sequence);
        auto result = deals;
        std::erase_if(result.trades, [&](const auto& trade) { return trade.sequence <= after_sequence; });
        return result;
    }
};

DtcReadOnlyServerConfig server_config(bool fixed_symbol = true) {
    DtcReadOnlyServerConfig config;
    config.source_mode = DtcSourceMode::LiveTest;
    config.symbol_id = fixed_symbol ? kSymbolId : 0;
    config.currency = "RUB";
    config.description = "OFFLINE SYNTHETIC public deals fixture; not exchange data";
    config.contract_size = 1;
    config.currency_value_per_increment = 1;
    return config;
}

std::uint64_t unix_now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

Deal make_deal(const FakeSource& source, std::uint64_t sequence, std::string price = "123.45678",
               std::int64_t price_scaled = 12345678, std::int64_t quantity = 1) {
    Deal trade;
    trade.sequence = sequence;
    trade.stream_epoch = source.deals.stream_epoch;
    trade.lifenum = source.deals.lifenum;
    trade.repl_id = 7000 + sequence;
    trade.repl_rev = static_cast<std::int64_t>(8000 + sequence);
    trade.deal_id = static_cast<std::int64_t>(9000 + sequence);
    trade.isin_id = source.state.isin_id;
    trade.session_id = source.state.session_id;
    trade.price_scaled = price_scaled;
    trade.quantity = quantity;
    trade.price = std::move(price);
    trade.moment = 987654321 + sequence; // Decoded MOEX calendar-seconds counter, not P2TIME bytes.
    trade.moment_ns = 1790553600123456789ULL + sequence * 100000000ULL;
    trade.received_at_unix_ns = trade.moment_ns + 1000000ULL;
    trade.xstatus_buy = kActiveSide;
    trade.xstatus_sell = kPassiveSide;
    trade.at_bid_or_ask = 0;
    return trade;
}

void append_deal(FakeSource& source, Deal trade) {
    if (!source.deals.first_sequence)
        source.deals.first_sequence = trade.sequence;
    source.deals.last_sequence = trade.sequence;
    source.deals.trades.push_back(std::move(trade));
}

struct Harness {
    FakeSource source;
    DtcReadOnlyServer server;
    DtcFrameDecoder decoder;
    std::vector<DtcFrame> received;
    int socket{-1};
    bool eof{false};

    explicit Harness(bool publisher = true) : source(), server(source, server_config()) {
        source.publisher = publisher;
        std::string error;
        require(server.start(error), error);
        socket = ::socket(AF_INET, SOCK_STREAM, 0);
        require(socket >= 0, "create test client socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(server.port());
        require(::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect test client");
        require(fcntl(socket, F_SETFL, O_NONBLOCK) == 0, "set test client nonblocking");
        server.poll();
    }

    ~Harness() {
        if (socket >= 0)
            ::close(socket);
    }

    void send_bytes(const Bytes& bytes) {
        std::size_t offset = 0;
        for (unsigned attempt = 0; offset < bytes.size() && attempt < 100; ++attempt) {
            const auto count = ::send(socket, bytes.data() + offset, bytes.size() - offset, 0);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            require(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR), "send test client bytes");
            pump(1);
        }
        require(offset == bytes.size(), "complete test client send");
    }

    void send_message(unsigned type, const Bytes& payload) {
        send_bytes(packet(type, payload));
    }

    void pump(unsigned iterations = 40) {
        for (unsigned i = 0; i < iterations; ++i) {
            server.poll();
            std::array<std::uint8_t, 8192> buffer{};
            const auto count = ::recv(socket, buffer.data(), buffer.size(), 0);
            if (count > 0) {
                std::string error;
                require(decoder.append(std::span(buffer.data(), static_cast<std::size_t>(count)), received, error),
                        error);
            } else if (count == 0) {
                eof = true;
            } else {
                require(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR, "receive test client bytes");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::size_t count(unsigned type) const {
        return static_cast<std::size_t>(std::count_if(received.begin(), received.end(),
                                                      [&](const auto& frame) { return frame.message_type == type; }));
    }

    const DtcFrame& first(unsigned type) const {
        const auto found = std::find_if(received.begin(), received.end(),
                                        [&](const auto& frame) { return frame.message_type == type; });
        require(found != received.end(), "expected DTC response frame missing");
        return *found;
    }

    void logon() {
        const Bytes encoding{8, 0, 0, 0, 4, 0, 0, 0, 'D', 'T', 'C', 0};
        send_message(6, encoding);
        pump();
        require(count(7) == 1, "DTC protobuf encoding negotiated");
        Bytes request;
        put_integer(request, 1, 8);
        put_integer(request, 7, 10);
        put_text(request, 11, "public-deals-unit-test");
        send_message(1, request);
        pump();
        require(count(2) == 1, "DTC logon completed");
        const WireMessage reply(first(2).payload);
        require(reply.number(20) == (source.publisher ? 1U : 0U),
                "LOGON_RESPONSE field20 reflects public trade capability");
        require(server.last_wire_logon_capabilities().market_data_supported == (source.publisher ? 1U : 0U),
                "completed wire capability receipt reflects public trade capability");
        for (const auto field : {8U, 9U, 10U, 17U, 19U})
            require(reply.number(field) == 0, "logon keeps trading and private capabilities disabled");
    }

    void definition() {
        Bytes request;
        put_integer(request, 1, 41);
        put_text(request, 2, kSymbol);
        put_text(request, 3, kExchange);
        send_message(506, request);
        pump();
        require(count(507) == 1, "current LiveTest fixture receives final 507");
        const WireMessage response(first(507).payload);
        require(response.number(1) == 41, "507 request id preserved");
        require(!response.has(20), "507 does not encode EarningsPerShare field20 as a varint capability");
        require(source.publisher ? response.number(35) == static_cast<std::uint64_t>(kSession) : !response.has(35),
                "507 session extension binds public trades and remains absent for depth-only clients");
    }

    void trade_request(std::string_view symbol = kSymbol, unsigned action = 1, std::uint32_t id = kSymbolId) {
        Bytes request;
        put_integer(request, 1, action);
        put_integer(request, 2, id);
        if (action != 2) {
            put_text(request, 3, symbol);
            put_text(request, 4, kExchange);
        }
        send_message(101, request);
        pump();
    }

    void depth_request() {
        Bytes request;
        put_integer(request, 1, 1);
        put_integer(request, 2, kSymbolId);
        put_text(request, 3, kSymbol);
        put_text(request, 4, kExchange);
        put_integer(request, 5, 20);
        send_message(102, request);
        pump();
    }

    void require_market_data_reject(std::size_t before) {
        require(count(103) == before + 1, "invalid trade request produces DTC103");
        require(server.has_client() && !eof, "trade rejection keeps the DTC client connected");
    }
};

void test_wire_fields_and_forward_only_cursor() {
    Harness h;
    append_deal(h.source, make_deal(h.source, 1));
    append_deal(h.source, make_deal(h.source, 2));
    h.logon();

    auto rejects = h.count(103);
    h.trade_request(); // No accepted 507 yet.
    h.require_market_data_reject(rejects);

    h.definition();
    rejects = h.count(103);
    h.trade_request("WRONG-SYMBOL");
    h.require_market_data_reject(rejects);

    h.trade_request();
    require(h.server.has_public_deals_subscription(), "valid 507-gated DTC101 subscribes to public trades");
    require(h.count(107) == 0, "retained source trades are not fabricated as initial DTC trades");

    h.trade_request(); // Duplicate SUBSCRIBE must not move the tail cursor.
    require(h.count(107) == 0, "repeated subscribe does not replay source history");
    append_deal(h.source, make_deal(h.source, 3));
    h.pump();
    require(h.count(107) == 1, "one newly committed source event emits one DTC107");
    const WireMessage trade(h.first(107).payload);
    require(trade.number(1) == kSymbolId && trade.number(2) == 0, "symbol ID and UNKNOWN side are preserved");
    const double price = static_cast<double>(12345678) / 100000.0;
    require(trade.real64(3) == price && static_cast<double>(static_cast<float>(price)) != price,
            "DTC price uses precise protobuf double rather than float32");
    require(trade.real64(4) == 1.0, "trade quantity uses protobuf double");
    const auto moment_ns = 1790553600123456789ULL + 3ULL * 100000000ULL;
    require(trade.real64(5) == static_cast<double>(moment_ns) / 1000000000.0,
            "standard DateTime is canonical UTC unix seconds from moment_ns");
    require(trade.number(6) == h.source.deals.stream_epoch && trade.number(7) == 1 && trade.string(8) == "9003" &&
                trade.number(9) == 3,
            "epoch, per-client delivery sequence, trade ID, and source capture sequence");
    require(trade.number(10) == moment_ns && trade.number(11) == 0 && trade.number(12) == 0,
            "canonical exchange time and zero unavailable engine clocks");
    require(trade.number(13) == 7003 && trade.number(14) == 8003 && trade.number(15) == h.source.deals.lifenum &&
                trade.number(16) == static_cast<std::uint64_t>(kSession) && trade.number(17) == kIsin &&
                trade.number(18) == moment_ns + 1000000ULL,
            "CGate revision, life, target, and receive provenance");
    require(trade.number(19) == 987654324 && trade.number(20) == moment_ns && trade.number(21) == kActiveSide &&
                trade.number(22) == kPassiveSide,
            "decoded moment counter, canonical nanoseconds, and raw xstatus provenance");

    h.trade_request();
    require(h.count(107) == 1, "duplicate subscribe does not replay an already delivered event");
    append_deal(h.source, make_deal(h.source, 4, "124.00001", 12400001, 2));
    h.pump();
    require(h.count(107) == 2, "the next committed event is delivered once after a duplicate subscribe");
    const auto second_frame = std::find_if(h.received.rbegin(), h.received.rend(),
                                           [](const auto& frame) { return frame.message_type == 107; });
    require(second_frame != h.received.rend(), "second DTC107 frame exists");
    const WireMessage second(second_frame->payload);
    require(second.number(7) == 2 && second.number(9) == 4 && second.real64(3) == 124.00001,
            "second delivery advances both delivery and capture sequences");
}

void test_capability_offline_and_no_publisher() {
    {
        Harness h(false);
        h.logon();
        h.definition();
        const auto rejects = h.count(103);
        h.trade_request();
        h.require_market_data_reject(rejects);
        require(!h.server.has_public_deals_subscription(), "disabled publisher never creates a trade subscription");
    }
    {
        Harness h;
        h.source.deals.online = false;
        h.source.deals.valid = false;
        h.logon();
        h.definition();
        const auto rejects = h.count(103);
        h.trade_request();
        h.require_market_data_reject(rejects);
        require(!h.server.has_public_deals_subscription(), "offline empty source rejects without closing the client");
        h.source.deals.online = h.source.deals.valid = true;
        h.trade_request();
        require(h.server.has_public_deals_subscription(), "subscription succeeds after source becomes online");
        append_deal(h.source, make_deal(h.source, 1));
        h.pump();
        require(h.count(107) == 1, "newly online publisher delivers only a post-subscribe commit");
    }
}

void test_overflow_epoch_and_invalid_deals() {
    {
        Harness h;
        append_deal(h.source, make_deal(h.source, 1));
        h.logon();
        h.definition();
        h.trade_request();
        require(h.server.has_public_deals_subscription(), "overflow test subscribed");
        h.source.deals.trades.clear();
        h.source.deals.first_sequence = 3;
        h.source.deals.last_sequence = 4;
        auto third = make_deal(h.source, 3);
        auto fourth = make_deal(h.source, 4);
        h.source.deals.trades = {third, fourth};
        const auto rejects = h.count(103);
        h.pump();
        h.require_market_data_reject(rejects);
        require(!h.server.has_public_deals_subscription() && h.count(107) == 0,
                "ring lag is explicitly rejected without a partial trade batch");
    }
    {
        Harness h;
        h.logon();
        h.definition();
        h.trade_request();
        ++h.source.deals.stream_epoch;
        const auto rejects = h.count(103);
        h.pump();
        h.require_market_data_reject(rejects);
        require(!h.server.has_public_deals_subscription(), "stream epoch changes retire the trade subscription");
    }

    const auto reject_mutated_deal = [](auto mutate, std::string_view label) {
        Harness h;
        h.logon();
        h.definition();
        h.trade_request();
        auto trade = make_deal(h.source, 1);
        mutate(trade);
        append_deal(h.source, std::move(trade));
        const auto rejects = h.count(103);
        h.pump();
        h.require_market_data_reject(rejects);
        require(!h.server.has_public_deals_subscription() && h.count(107) == 0, label);
    };
    reject_mutated_deal([](Deal& deal) { deal.session_id = kSession + 1; }, "wrong-session trade is rejected");
    reject_mutated_deal([](Deal& deal) { deal.isin_id = kIsin + 1; }, "wrong-ISIN trade is rejected");
    reject_mutated_deal([](Deal& deal) { --deal.lifenum; }, "stale-life trade is rejected");
    reject_mutated_deal([](Deal& deal) { deal.moment_ns = 0; }, "missing canonical exchange time is rejected");
    reject_mutated_deal([](Deal& deal) { deal.received_at_unix_ns = 0; }, "missing source receive time is rejected");
}

void test_trade_only_revalidates_definition_and_keeps_depth_independent() {
    {
        Harness h;
        h.logon();
        h.definition();
        h.trade_request();
        require(h.server.has_public_deals_subscription(), "trade-only definition retirement test subscribed");
        ++h.source.state.session_id;
        const auto rejects = h.count(103);
        h.pump();
        h.require_market_data_reject(rejects);
        require(!h.server.has_public_deals_subscription(),
                "trade-only subscription retires when current session definition changes");
    }
    {
        Harness h;
        h.logon();
        h.definition();
        h.depth_request();
        require(h.count(145) == 2, "depth subscription starts with complete depth snapshot");
        h.trade_request();
        append_deal(h.source, make_deal(h.source, 1));
        h.pump();
        require(h.count(107) == 1 && h.count(145) == 2, "trade update leaves depth snapshot history intact");
        h.source.deals.online = false;
        const auto rejects = h.count(103);
        h.pump();
        h.require_market_data_reject(rejects);
        ++h.source.state.source_snapshot_version;
        h.source.state.levels[0].volume = 7;
        const auto depth_count = h.count(145);
        h.pump();
        require(h.count(145) == depth_count + 2 && h.server.has_client(),
                "public trade source loss retires only trades and depth continues");
    }
}

void append_fixture_trade(FakeSource& source, std::uint64_t sequence, std::uint64_t moment_ns) {
    const bool buy_active = (sequence & 1U) != 0;
    const auto price = buy_active ? 10000000LL : 10100000LL;
    const auto price_text = buy_active ? "100.00000" : "101.00000";
    Deal trade;
    trade.sequence = sequence;
    trade.stream_epoch = source.deals.stream_epoch;
    trade.lifenum = 1;
    trade.repl_id = sequence;
    trade.repl_rev = static_cast<std::int64_t>(sequence);
    trade.deal_id = static_cast<std::int64_t>(sequence);
    trade.isin_id = kIsin;
    trade.session_id = kSession;
    trade.price_scaled = price;
    trade.quantity = buy_active ? 1 : 2;
    trade.price = price_text;
    trade.moment = 700000000 + sequence;
    trade.moment_ns = moment_ns;
    trade.received_at_unix_ns = moment_ns + 1000000ULL;
    trade.xstatus_buy = buy_active ? kActiveSide : kPassiveSide;
    trade.xstatus_sell = buy_active ? kPassiveSide : kActiveSide;
    trade.at_bid_or_ask = buy_active ? 2 : 1;
    append_deal(source, std::move(trade));
}

int serve_fixture() {
    FakeSource source;
    source.state.description = "OFFLINE SYNTHETIC DTC fixture; not exchange data";
    source.state.min_step = "1";
    source.state.isin_id = kIsin;
    source.state.session_id = kSession;
    source.deals.online = source.deals.valid = true;
    source.deals.stream_epoch = 1;
    source.deals.lifenum = 1;
    auto config = server_config();
    DtcReadOnlyServer server(source, std::move(config));
    std::string error;
    if (!server.start(error)) {
        std::cerr << "fixture server start failed: " << error << '\n';
        return 2;
    }
    std::cout << "PORT=" << server.port() << '\n' << std::flush;
    std::cerr << "OFFLINE SYNTHETIC fixture only; not MOEX exchange data\n";

    const auto connect_deadline = Clock::now() + std::chrono::seconds(15);
    auto client_deadline = Clock::time_point::max();
    auto publish_deadline = Clock::time_point::max();
    auto next_publish = Clock::time_point::max();
    bool saw_client = false;
    bool publishing = false;
    std::uint64_t sequence = 0;
    std::uint64_t next_moment_ns = 0;
    while (true) {
        server.poll();
        const auto now = Clock::now();
        if (server.has_client()) {
            if (!saw_client)
                client_deadline = now + std::chrono::seconds(15);
            saw_client = true;
        } else if (saw_client) {
            server.stop();
            return 0;
        }
        if (!saw_client && now >= connect_deadline) {
            server.stop();
            return 0;
        }
        if (server.has_public_deals_subscription()) {
            if (!publishing) {
                publishing = true;
                publish_deadline = now + std::chrono::seconds(15);
                next_publish = now;
                next_moment_ns = unix_now_ns();
            }
            if (now >= publish_deadline) {
                server.stop();
                return 0;
            }
            if (now >= next_publish && sequence < 150) {
                ++sequence;
                append_fixture_trade(source, sequence, next_moment_ns);
                next_moment_ns += 100000000ULL;
                next_publish = now + std::chrono::milliseconds(100);
            }
        } else if (saw_client && !publishing && now >= client_deadline) {
            server.stop();
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--serve-fixture")
            return serve_fixture();
        require(argc == 1, "usage: connector_host_dtc_public_deals_test [--serve-fixture]");
        test_wire_fields_and_forward_only_cursor();
        test_capability_offline_and_no_publisher();
        test_overflow_epoch_and_invalid_deals();
        test_trade_only_revalidates_definition_and_keeps_depth_independent();
        std::cout << "PASS connector_host_dtc_public_deals_test\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL connector_host_dtc_public_deals_test: " << error.what() << '\n';
        return 1;
    }
}
