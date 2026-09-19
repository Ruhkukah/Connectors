#include "moex/connector_host/dtc_read_only_server.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <map>
#include <limits>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace moex::connector_host::dtc;
using Kind = moex::plaza2::cgate::SessionReadyWitnessKind;
using Bytes = std::vector<std::uint8_t>;
void check(bool ok, const char* text) {
    if (!ok) {
        std::cerr << "FAIL: " << text << '\n';
        std::exit(1);
    }
}
void vint(Bytes& b, std::uint64_t v) {
    while (v > 127) {
        b.push_back(static_cast<std::uint8_t>(v) | 128);
        v >>= 7;
    }
    b.push_back(static_cast<std::uint8_t>(v));
}
void num(Bytes& b, unsigned f, std::uint64_t v) {
    vint(b, f * 8);
    vint(b, v);
}
void txt(Bytes& b, unsigned f, const std::string& s) {
    vint(b, f * 8 + 2);
    vint(b, s.size());
    b.insert(b.end(), s.begin(), s.end());
}
Bytes packet(unsigned type, Bytes p) {
    const auto n = p.size() + 4;
    Bytes b{static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(type),
            static_cast<std::uint8_t>(type >> 8)};
    b.insert(b.end(), p.begin(), p.end());
    return b;
}
// Independent test reader: verifies outgoing protobuf wire types and decodes
// floats/text, rather than consulting any production serialization helper.
struct Read {
    std::map<unsigned, std::uint64_t> n;
    std::map<unsigned, std::string> s;
    std::map<unsigned, float> f;
    explicit Read(const Bytes& b) {
        std::size_t pos = 0;
        auto var = [&]() {
            std::uint64_t result = 0;
            unsigned shift = 0;
            for (;;) {
                check(pos < b.size() && shift < 70, "outgoing varint bounds");
                auto c = b[pos++];
                result |= std::uint64_t(c & 127) << shift;
                if (c < 128)
                    return result;
                shift += 7;
            }
        };
        while (pos < b.size()) {
            const auto tag = var();
            auto key = static_cast<unsigned>(tag >> 3);
            switch (tag & 7) {
            case 0:
                n[key] = var();
                break;
            case 2: {
                auto len = var();
                check(len <= b.size() - pos, "outgoing string bounds");
                s[key] = std::string(b.begin() + static_cast<std::ptrdiff_t>(pos),
                                     b.begin() + static_cast<std::ptrdiff_t>(pos + len));
                check(moex::plaza2::cgate::text::valid_utf8(s[key]), "all protobuf strings are UTF-8");
                pos += len;
                break;
            }
            case 5: {
                check(pos + 4 <= b.size(), "outgoing float bounds");
                std::uint32_t bits = 0;
                for (unsigned i = 0; i < 4; ++i)
                    bits |= std::uint32_t(b[pos++]) << (i * 8);
                f[key] = std::bit_cast<float>(bits);
                break;
            }
            default:
                check(false, "unexpected outgoing wire type");
            }
        }
    }
};
std::uint64_t number_or_zero(const Read& message, unsigned field) {
    const auto found = message.n.find(field);
    return found == message.n.end() ? 0 : found->second;
}
struct Replay : DtcMarketDataSource {
    DtcMarketDataSnapshot state;
    bool security_definitions{true};
    bool optimistic_capabilities{false};
    unsigned failure_mode{0};
    Replay() {
        state.symbol = "ALRS-12.26";
        state.underlying_board = "RFUD";
        state.min_step = "0.01";
        state.isin_id = 123;
        state.transport_active = state.source_online = state.aggr_online = state.snapshot_complete = true;
        state.book_snapshot_current = state.market_data_display_allowed = state.refdata_metadata_current = true;
        state.session_ready_witness_kind = Kind::LateJoinCorroboratedSnapshot;
        state.source_consistent = false;
        state.target_authoritative = false; // intentional provisional fixture
        state.order_entry_allowed = true;   // malicious/optimistic upstream cannot turn on execution
        state.stream_epoch = 73;
        state.source_snapshot_version = 9;
        state.market_data_authority_epoch = 2;
        state.snapshot_watermark = 55;
        state.source_snapshot_hash = 1234;
        state.levels = {
            {.price_scaled = -100000,
             .volume = 5,
             .side = DtcDepthSide::Bid,
             .source_row_id = 9001,
             .source_sequence = 54},
            {.price_scaled = 0, .volume = 7, .side = DtcDepthSide::Ask, .source_row_id = 9002, .source_sequence = 55}};
    }
    DtcMarketDataSnapshot snapshot() const override {
        if (failure_mode == 1)
            throw std::runtime_error("synthetic source failure");
        if (failure_mode == 2)
            throw 42;
        return state;
    }
    DtcReadOnlyCapabilities capabilities() const noexcept override {
        return {.market_data = false,
                .market_depth = true,
                .security_definitions = security_definitions,
                .accounts = optimistic_capabilities,
                .positions = optimistic_capabilities,
                .orders = optimistic_capabilities,
                .order_entry = optimistic_capabilities};
    }
};
std::string description() {
    return moex::plaza2::cgate::text::spectra_fixed_string_to_utf8(
        "\xd4\xfc\xfe\xf7\xe5\xf0\xf1\xed\xfb\xe9 \xea\xee\xed\xf2\xf0\xe0\xea\xf2 ALRS-12.26");
}
DtcReadOnlyServerConfig config() {
    DtcReadOnlyServerConfig c;
    c.currency = "RUB";
    c.contract_size = 100;
    c.currency_value_per_increment = 1; // explicitly declared fake replay economics, RUB/tick/contract
    c.description = description();
    return c;
}
void mark_live_refdata_proven(Replay& source) {
    auto& state = source.state;
    state.session_id = 321;
    state.refdata_vcb_join_current = true;
    state.future_vcb_provenance_present = true;
    state.refdata_board_proven = true;
    state.refdata_currency_proven = true;
    state.target_is_future = true;
    state.future_vcb_base_contract_code = "ALRS";
    state.future_vcb_base_contract_id = 42;
    state.definition_source_provenance = {.stream_code = moex::plaza2::generated::StreamCode::kFortsRefdataRepl,
                                          .table_code =
                                              moex::plaza2::generated::TableCode::kFortsRefdataReplFutInstruments,
                                          .repl_rev = 73,
                                          .lifenum = 12,
                                          .present = true};
    state.future_instruments_provenance = {.stream_code = moex::plaza2::generated::StreamCode::kFortsRefdataRepl,
                                           .table_code =
                                               moex::plaza2::generated::TableCode::kFortsRefdataReplFutInstruments,
                                           .repl_rev = 73,
                                           .lifenum = 12,
                                           .present = true};
    state.future_sess_contents_provenance = {.stream_code = moex::plaza2::generated::StreamCode::kFortsRefdataRepl,
                                             .table_code =
                                                 moex::plaza2::generated::TableCode::kFortsRefdataReplFutSessContents,
                                             .repl_rev = 74,
                                             .lifenum = 12,
                                             .present = true};
    state.session_provenance = {.stream_code = moex::plaza2::generated::StreamCode::kFortsRefdataRepl,
                                .table_code = moex::plaza2::generated::TableCode::kFortsRefdataReplSession,
                                .repl_rev = 76,
                                .lifenum = 12,
                                .present = true};
    state.future_vcb_provenance = {.stream_code = moex::plaza2::generated::StreamCode::kFortsRefdataRepl,
                                   .table_code = moex::plaza2::generated::TableCode::kFortsRefdataReplFutVcb,
                                   .repl_rev = 77,
                                   .lifenum = 12,
                                   .present = true};
    state.future_vcb_repl_rev = 77;
    state.future_vcb_lifenum = 12;
    state.description = "Authoritative TEST future";
    state.currency = "RUB";
    state.contract_size = "10";
    state.currency_value_per_increment = "2.5";
}
struct Harness {
    Replay source;
    DtcSourceMode source_mode{DtcSourceMode::Replay};
    DtcReadOnlyServer server;
    int fd{-1};
    DtcFrameDecoder decoder;
    std::vector<DtcFrame> got;
    bool eof{};
    explicit Harness(DtcReadOnlyServerConfig c = config()) : source_mode(c.source_mode), server(source, std::move(c)) {
        std::string error;
        check(server.start(error), error.c_str());
        connect();
    }
    ~Harness() {
        if (fd >= 0)
            ::close(fd);
    }
    void connect() {
        if (fd >= 0)
            ::close(fd);
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        check(fd >= 0, "client socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(server.port());
        check(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "loopback connect");
        check(fcntl(fd, F_SETFL, O_NONBLOCK) == 0, "client nonblocking");
        decoder.reset();
        got.clear();
        eof = false;
        server.poll();
    }
    void send(const Bytes& b) {
        check(::send(fd, b.data(), b.size(), 0) == static_cast<ssize_t>(b.size()), "client send");
    }
    void pump(unsigned iterations = 50) {
        for (unsigned i = 0; i < iterations; ++i) {
            server.poll();
            std::array<std::uint8_t, 8192> b{};
            auto n = ::recv(fd, b.data(), b.size(), 0);
            if (n > 0) {
                std::string error;
                check(decoder.append(std::span(b.data(), static_cast<std::size_t>(n)), got, error), error.c_str());
            } else if (n == 0)
                eof = true;
            else
                check(errno == EAGAIN || errno == EWOULDBLOCK, "client recv");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::size_t count(unsigned type) const {
        std::size_t result = 0;
        for (const auto& f : got)
            if (f.message_type == type)
                ++result;
        return result;
    }
    const DtcFrame& first(unsigned type) const {
        for (const auto& f : got)
            if (f.message_type == type)
                return f;
        std::string received_types;
        for (const auto& f : got) {
            if (!received_types.empty())
                received_types += ",";
            received_types += std::to_string(f.message_type);
        }
        const auto message =
            "expected response type " + std::to_string(type) + " missing; received types [" + received_types + "]";
        check(false, message.c_str());
        return got.front();
    }
    void logon(bool expect_security_definitions = true, std::string_view username = {}, std::string_view password = {},
               bool expect_success = true) {
        // Literal official binary negotiation, deliberately fragmented.
        const Bytes handshake{16, 0, 6, 0, 8, 0, 0, 0, 4, 0, 0, 0, 'D', 'T', 'C', 0};
        send(Bytes(handshake.begin(), handshake.begin() + 3));
        pump(2);
        check(got.empty(), "fragment produced response");
        send(Bytes(handshake.begin() + 3, handshake.end()));
        pump(5);
        check(first(7).payload == Bytes(handshake.begin() + 4, handshake.end()), "binary encoding response");
        Bytes p;
        num(p, 1, 8);
        num(p, 7, 10);
        if (!username.empty() || !password.empty()) {
            txt(p, 2, std::string(username));
            txt(p, 3, std::string(password));
        }
        txt(p, 11, "Kairos-test");
        send(packet(1, p));
        if (!expect_success) {
            pump(5);
            return;
        }
        // Keep this loopback protocol test robust when the CI host schedules
        // the client/server less promptly than the 5 ms fixture pump window.
        for (unsigned i = 0; i < 100 && count(2) == 0 && !eof; ++i)
            pump(1);
        Read reply(first(2).payload);
        check(reply.n[1] == 8 && reply.n[2] == 1 && reply.n[12] == (expect_security_definitions ? 1 : 0) &&
                  reply.n[15] == 1,
              "v8 read-only capability logon");
        for (unsigned f : {8U, 9U, 10U, 17U, 19U, 20U})
            check(reply.n[f] == 0, "no trading or trade tape capability");
        if (source_mode == DtcSourceMode::LiveTest)
            check(reply.s[3] == "Read-only live TEST; trading disabled" && reply.s[6] == "MOEX live TEST DTC",
                  "live TEST logon identity is not replay text");
    }
    Bytes definition_request() {
        Bytes p;
        num(p, 1, 41);
        txt(p, 2, source.state.symbol);
        txt(p, 3, std::string(kDtcMoexSpectraExchange));
        return packet(506, p);
    }
    Bytes depth(unsigned action = 1, unsigned id = 0) {
        if (id == 0)
            id = server.symbol_id() == 0 ? 7 : server.symbol_id();
        Bytes p;
        num(p, 1, action);
        num(p, 2, id);
        txt(p, 3, source.state.symbol);
        txt(p, 4, std::string(kDtcMoexSpectraExchange));
        num(p, 5, 20);
        return packet(102, p);
    }
    void subscribe() {
        // Coalesced final-definition and subscription requests must retain order.
        auto b = definition_request();
        auto d = depth();
        b.insert(b.end(), d.begin(), d.end());
        send(b);
        pump();
    }
};

void replay_roundtrip() {
    Harness h;
    const auto capabilities = h.source.capabilities();
    check(!capabilities.accounts && !capabilities.positions && !capabilities.orders && !capabilities.order_entry,
          "normal replay fixture keeps account and order capabilities false");
    h.logon();
    h.subscribe();
    check(h.count(145) == 2, "provisional late join emits complete signed/zero depth");
    Read def(h.first(507).payload);
    check(def.n[1] == 41 && def.n[4] == 1 && def.n[9] == 1 && def.n[33] == 123,
          "future security type and final definition identity are independent of symbol ID");
    check(def.s[5] == "Фьючерсный контракт ALRS-12.26", "exact CP1251 to UTF-8 protobuf roundtrip");
    check(def.s[28] == "RUB" && def.f[29] == 100 && def.f[8] == 1,
          "explicit definition metadata and float tag8 tick value");
    std::size_t final_def = 0, feed = 0, symbol = 0, authority = 0, depth = 0;
    for (std::size_t i = 0; i < h.got.size(); ++i) {
        const auto type = h.got[i].message_type;
        if (type == 507)
            final_def = i;
        if (type == 100)
            feed = i;
        if (type == 116)
            symbol = i;
        if (type == 700)
            authority = i;
        if (type == 145 && !depth)
            depth = i;
    }
    check(final_def < feed && feed < symbol && symbol < authority && authority < depth,
          "definition/feed/symbol/authority/depth ordering");
    Read a(h.first(700).payload);
    check(a.n[2] == 0, "authority is nonpopup");
    check(a.s[1] == "moex.source_authority.v1 "
                    "{\"symbol_id\":7,\"source_mode\":\"replay\",\"stream_epoch\":73,\"aggr_online\":true,"
                    "\"book_snapshot_current\":true,"
                    "\"session_ready_witness_kind\":\"LateJoinCorroboratedSnapshot\",\"market_data_display_allowed\":"
                    "true,\"order_entry_allowed\":false,\"exchange_confirmed\":false}",
          "exact agreed authority JSON envelope");
    unsigned row = 0;
    for (const auto& f : h.got)
        if (f.message_type == 145) {
            Read d(f.payload);
            check(d.n[11] == 73 && d.n[12] == 1 && d.n[13] == 9 && d.n[15] == 2,
                  "batch source metadata matches authority epoch");
            check(d.n[20] == 9001 + row && d.n[19] == 54 + row, "CGate row identity/revision preserved");
            check(d.f[2] == (row == 0 ? -1.0F : 0.0F), "zero and negative prices preserved");
            check(d.n[5] == 1 && d.n[7] == (row == 0 ? 3 : 1), "positional begin/final markers");
            ++row;
        }
    h.got.clear();
    ++h.source.state.source_snapshot_version;
    h.source.state.levels[0].volume = 9;
    h.pump();
    check(h.count(145) == 2, "updated replay version emits replacement snapshot");
    check(Read(h.first(145).payload).n[12] == 2, "server batch sequence increments");
    h.got.clear();
    h.send(h.depth(2));
    h.pump();
    ++h.source.state.source_snapshot_version;
    h.pump();
    check(h.count(145) == 0, "unsubscribe stops snapshots");
    h.send(h.depth(3));
    h.pump();
    check(h.count(145) == 2, "one shot snapshot request");
    h.got.clear();
    ++h.source.state.source_snapshot_version;
    h.pump();
    check(h.count(145) == 0, "one shot does not subscribe");
}
void live_test_metadata_and_auth() {
    auto c = config();
    c.source_mode = DtcSourceMode::LiveTest;
    c.symbol_id = 17;
    // Deliberately conflicting replay terms must be ignored by live_test.
    c.currency = "REPLAY-CURRENCY";
    c.description = "replay description must not leak";
    c.contract_size = 999;
    c.currency_value_per_increment = 999;
    Harness h(c);
    mark_live_refdata_proven(h.source);
    const auto mapped = validate_dtc_security_definition(h.source.state, DtcSourceMode::LiveTest);
    check(mapped.available() && mapped.definition->exchange == kDtcMoexSpectraExchange &&
              mapped.definition->underlying_board == "RFUD" && mapped.definition->currency == "RUB" &&
              mapped.definition->contract_size == 10 && mapped.definition->future_vcb_provenance.lifenum == 12,
          "canonical definition keeps DTC venue separate from raw board and includes source terms/provenance");
    const auto initial_definition_version = mapped.definition->definition_version;
    auto book_only_change = h.source.state;
    ++book_only_change.source_snapshot_version;
    book_only_change.source_snapshot_hash ^= 0x100;
    const auto same_definition = validate_dtc_security_definition(book_only_change, DtcSourceMode::LiveTest);
    check(same_definition.available() && same_definition.definition->definition_version == initial_definition_version,
          "canonical definition version is independent of AGGR book-only changes");
    auto changed_session_source = h.source.state;
    ++changed_session_source.session_provenance.repl_rev;
    const auto changed_session_definition =
        validate_dtc_security_definition(changed_session_source, DtcSourceMode::LiveTest);
    check(changed_session_definition.available() &&
              changed_session_definition.definition->definition_version != initial_definition_version,
          "canonical definition version includes active-session source provenance");
    auto wrong_generation = h.source.state;
    wrong_generation.definition_source_provenance.lifenum = 13;
    const auto wrong_generation_result = validate_dtc_security_definition(wrong_generation, DtcSourceMode::LiveTest);
    check(!wrong_generation_result.available() &&
              wrong_generation_result.reason ==
                  "current futures terms lack matching-generation source-table provenance",
          "canonical definition rejects cross-generation REFDATA provenance with a specific reason");
    auto inconsistent_generation = h.source.state;
    inconsistent_generation.future_instruments_provenance.lifenum = 13;
    const auto inconsistent_generation_result =
        validate_dtc_security_definition(inconsistent_generation, DtcSourceMode::LiveTest);
    check(!inconsistent_generation_result.available() &&
              inconsistent_generation_result.reason ==
                  "additional current futures-table provenance is from another REFDATA generation",
          "canonical definition rejects an additional source table from another REFDATA generation");
    auto inconsistent_session_generation = h.source.state;
    inconsistent_session_generation.session_provenance.lifenum = 13;
    const auto inconsistent_session_result =
        validate_dtc_security_definition(inconsistent_session_generation, DtcSourceMode::LiveTest);
    check(!inconsistent_session_result.available() &&
              inconsistent_session_result.reason ==
                  "active session REFDATA source generation/provenance is missing or inconsistent",
          "canonical definition rejects active-session provenance from another REFDATA generation");
    auto missing_session_source = h.source.state;
    missing_session_source.session_provenance.present = false;
    const auto missing_session_result =
        validate_dtc_security_definition(missing_session_source, DtcSourceMode::LiveTest);
    check(!missing_session_result.available() &&
              missing_session_result.reason ==
                  "active session REFDATA source generation/provenance is missing or inconsistent",
          "canonical definition requires committed active-session source provenance");
    auto spread = h.source.state;
    spread.target_is_spread = true;
    check(!validate_dtc_security_definition(spread, DtcSourceMode::LiveTest).available(),
          "calendar spread is not mapped to the outright futures DTC profile");
    check(!h.server.last_wire_logon_capabilities().response_fully_written_to_socket,
          "wire logon capabilities remain unobserved before a LOGON_RESPONSE is fully written");
    h.logon();
    const auto logon_reply = Read(h.first(2).payload);
    const auto& observed_wire_capabilities = h.server.last_wire_logon_capabilities();
    check(observed_wire_capabilities.response_fully_written_to_socket &&
              observed_wire_capabilities.market_depth_updates_best_bid_and_ask == number_or_zero(logon_reply, 7) &&
              observed_wire_capabilities.trading_is_supported == number_or_zero(logon_reply, 8) &&
              observed_wire_capabilities.oco_orders_supported == number_or_zero(logon_reply, 9) &&
              observed_wire_capabilities.order_cancel_replace_supported == number_or_zero(logon_reply, 10) &&
              observed_wire_capabilities.security_definitions_supported == number_or_zero(logon_reply, 12) &&
              observed_wire_capabilities.historical_price_data_supported == number_or_zero(logon_reply, 13) &&
              observed_wire_capabilities.resubscribe_when_market_data_feed_available ==
                  number_or_zero(logon_reply, 14) &&
              observed_wire_capabilities.market_depth_is_supported == number_or_zero(logon_reply, 15) &&
              observed_wire_capabilities.one_historical_price_data_request_per_connection ==
                  number_or_zero(logon_reply, 16) &&
              observed_wire_capabilities.bracket_orders_supported == number_or_zero(logon_reply, 17) &&
              observed_wire_capabilities.multiple_positions_per_symbol_and_trade_account ==
                  number_or_zero(logon_reply, 19) &&
              observed_wire_capabilities.market_data_supported == number_or_zero(logon_reply, 20),
          "receipt wire-capability snapshot exactly matches the independent LOGON_RESPONSE decoder");
    h.subscribe();
    Read definition(h.first(507).payload);
    check(definition.n[4] == 1 && definition.n[9] == 1 && definition.n[33] == 123,
          "live TEST definition uses FUTURE/final fields and source ISIN, not DTC symbol ID");
    check(definition.s[3] == kDtcMoexSpectraExchange,
          "DTC Exchange is the gateway venue identifier, not the source ASTS SECBOARD board");
    check(definition.s[5] == "Authoritative TEST future" && definition.s[28] == "RUB" && definition.f[6] == 0.01F &&
              definition.f[29] == 10 && definition.f[8] == 2.5F,
          "live TEST 507 uses source metadata, not replay economics");
    for (const auto field : {7U, 10U, 11U, 22U, 24U})
        check(!definition.n.contains(field) && !definition.f.contains(field),
              "live TEST 507 omits unmapped optional economics");
    check(Read(h.first(145).payload).n[1] == 17, "live TEST depth uses fixed DTC symbol identity");
    const auto authority = Read(h.first(700).payload).s[1];
    check(authority.find("\"source_mode\":\"live_test\"") != std::string::npos &&
              authority.find("\"symbol_id\":17") != std::string::npos,
          "live TEST authority is explicitly identified");
    auto unsupported = c;
    Harness unsupported_currency(unsupported);
    mark_live_refdata_proven(unsupported_currency.source);
    unsupported_currency.source.state.description = "Authoritative TEST future";
    unsupported_currency.source.state.currency = "USD";
    unsupported_currency.source.state.refdata_currency_proven = false;
    unsupported_currency.source.state.contract_size = "10";
    unsupported_currency.source.state.currency_value_per_increment = "2.5";
    unsupported_currency.logon();
    unsupported_currency.subscribe();
    check(unsupported_currency.count(509) == 1 && unsupported_currency.count(507) == 0,
          "live TEST unsupported quotation currency fails 507 closed");
    h.got.clear();
    h.source.state.refdata_vcb_join_current = false;
    h.pump();
    check(h.count(700) == 1 && h.count(145) == 0 && h.count(5) == 1 && h.eof,
          "live TEST fut_vcb revocation fences depth and closes the session");

    auto missing = c;
    Harness invalid(missing);
    mark_live_refdata_proven(invalid.source);
    invalid.source.state.description.clear();
    invalid.source.state.currency.clear();
    invalid.source.state.contract_size.clear();
    invalid.source.state.currency_value_per_increment.clear();
    invalid.logon();
    invalid.subscribe();
    check(invalid.count(509) == 1 && invalid.count(507) == 0 && invalid.count(145) == 0,
          "live TEST missing authoritative 507 metadata fails closed");

    Harness ambiguous(c);
    mark_live_refdata_proven(ambiguous.source);
    ambiguous.source.state.refdata_vcb_join_ambiguous = true;
    ambiguous.source.state.description = "Authoritative TEST future";
    ambiguous.source.state.currency = "RUB";
    ambiguous.source.state.contract_size = "10";
    ambiguous.source.state.currency_value_per_increment = "2.5";
    ambiguous.logon();
    ambiguous.subscribe();
    check(ambiguous.count(509) == 1 && ambiguous.count(507) == 0, "live TEST ambiguous fut_vcb join fails 507 closed");

    auto auth = c;
    auth.require_local_auth = true;
    auth.local_username = "local-user";
    auth.local_password = "local-password";
    Harness wrong(auth);
    mark_live_refdata_proven(wrong.source);
    wrong.logon(false, "wrong-user", "wrong-password", false);
    wrong.pump(3);
    check(wrong.count(5) == 1 && wrong.eof, "dedicated local DTC credentials fence invalid logon");
    Harness correct(auth);
    mark_live_refdata_proven(correct.source);
    correct.source.state.description = "Authoritative TEST future";
    correct.source.state.currency = "RUB";
    correct.source.state.contract_size = "10";
    correct.source.state.currency_value_per_increment = "2.5";
    correct.logon(true, "local-user", "local-password");
    check(correct.count(2) == 1, "dedicated local DTC credentials permit valid logon");
}
void live_test_late_refdata_after_logon() {
    auto c = config();
    c.source_mode = DtcSourceMode::LiveTest;
    c.symbol_id = 17;
    Harness h(c);
    // Implemented security-definition capability is advertised even while
    // the committed source metadata is still cold.
    h.logon();
    check(Read(h.first(2).payload).n[12] == 1, "LiveTest feature capability survives cold REFDATA");
    h.send(h.definition_request());
    h.pump(5);
    check(h.count(509) == 1 && h.count(507) == 0,
          "cold current metadata rejects 506 without falsely emitting a definition");

    h.got.clear();
    mark_live_refdata_proven(h.source);
    h.send(h.definition_request());
    h.pump(5);
    check(h.count(507) == 1 && h.count(509) == 0 && Read(h.first(507).payload).n[9] == 1,
          "same logged-on connection emits a final 507 when committed REFDATA becomes ready");
}
void revoke_and_reconnect() {
    Harness h;
    h.logon();
    h.subscribe();
    h.got.clear();
    h.source.state.market_data_display_allowed = false;
    h.pump(); // unchanged version must still fence
    check(h.count(700) == 1 && h.count(145) == 0 && h.count(5) == 1 && h.eof,
          "revocation fences before depth and closes");
    check(Read(h.first(700).payload).s[1].find("\"market_data_display_allowed\":false") != std::string::npos,
          "revoked authority exposed");
    h.source.state.market_data_display_allowed = true;
    h.connect();
    h.logon();
    h.subscribe();
    check(Read(h.first(145).payload).n[12] == 1, "reconnection needs fresh complete snapshot");
}
void definition_revocation_case(std::string_view name, const std::function<void(Replay&)>& mutate,
                                const std::function<void(Replay&)>& restore,
                                const std::function<void(const Read&)>& check_new_definition) {
    auto c = config();
    c.source_mode = DtcSourceMode::LiveTest;
    c.symbol_id = 17;
    Harness h(c);
    mark_live_refdata_proven(h.source);
    h.logon();
    h.subscribe();
    check(h.count(507) == 1 && h.count(145) == 2, "LiveTest definition established before mutation");
    h.send(h.definition_request());
    h.pump(5);
    check(h.server.has_client() && h.count(507) == 2 && h.count(5) == 0,
          "identical definition request is idempotent without a reconnect cycle");
    const auto source_version = h.source.state.source_snapshot_version;
    h.got.clear();
    mutate(h.source);
    check(h.source.state.source_snapshot_version == source_version,
          "metadata mutation leaves the source book version unchanged");
    h.pump(5);
    check(!h.server.has_client() && h.eof && h.count(145) == 0 && h.count(5) == 1,
          (std::string(name) + ": metadata change closes without publishing another depth batch").c_str());
    check(h.count(700) == 1 && h.count(116) == 1,
          (std::string(name) + ": authority and symbol invalidation precede close").c_str());

    restore(h.source);
    h.connect();
    h.logon();
    h.subscribe();
    check(h.count(507) == 1 && h.count(145) == 2 && Read(h.first(145).payload).n[12] == 1,
          (std::string(name) + ": a new connection gets a final definition and fresh snapshot").c_str());
    check_new_definition(Read(h.first(507).payload));
}
void definition_revocation() {
    const auto unchanged = [](Replay&) {};
    definition_revocation_case(
        "removed description", [](Replay& source) { source.state.description.clear(); },
        [](Replay& source) { source.state.description = "Authoritative TEST future"; },
        [](const Read& definition) {
            check(definition.s.at(5) == "Authoritative TEST future", "fresh description restored");
        });
    definition_revocation_case(
        "changed description", [](Replay& source) { source.state.description = "Revised TEST future"; }, unchanged,
        [](const Read& definition) {
            check(definition.s.at(5) == "Revised TEST future", "new description reaches fresh 507");
        });
    definition_revocation_case(
        "removed currency", [](Replay& source) { source.state.currency.clear(); },
        [](Replay& source) { source.state.currency = "RUB"; },
        [](const Read& definition) { check(definition.s.at(28) == "RUB", "fresh currency restored"); });
    definition_revocation_case(
        "unsupported changed currency", [](Replay& source) { source.state.currency = "USD"; },
        [](Replay& source) { source.state.currency = "RUB"; },
        [](const Read& definition) { check(definition.s.at(28) == "RUB", "unsupported currency never reaches 507"); });
    definition_revocation_case(
        "removed contract size", [](Replay& source) { source.state.contract_size.clear(); },
        [](Replay& source) { source.state.contract_size = "10"; },
        [](const Read& definition) { check(definition.f.at(29) == 10, "fresh contract size restored"); });
    definition_revocation_case(
        "changed contract size", [](Replay& source) { source.state.contract_size = "20"; }, unchanged,
        [](const Read& definition) { check(definition.f.at(29) == 20, "new contract size reaches fresh 507"); });
    definition_revocation_case(
        "removed tick value", [](Replay& source) { source.state.currency_value_per_increment.clear(); },
        [](Replay& source) { source.state.currency_value_per_increment = "2.5"; },
        [](const Read& definition) { check(definition.f.at(8) == 2.5F, "fresh tick value restored"); });
    definition_revocation_case(
        "changed tick value", [](Replay& source) { source.state.currency_value_per_increment = "3.75"; }, unchanged,
        [](const Read& definition) { check(definition.f.at(8) == 3.75F, "new tick value reaches fresh 507"); });
    definition_revocation_case(
        "new REFDATA generation",
        [](Replay& source) {
            auto& state = source.state;
            state.definition_source_provenance.lifenum = 13;
            state.future_instruments_provenance.lifenum = 13;
            state.future_sess_contents_provenance.lifenum = 13;
            state.session_provenance.lifenum = 13;
            state.future_vcb_provenance.lifenum = 13;
            state.future_vcb_lifenum = 13;
        },
        unchanged,
        [](const Read& definition) {
            check(definition.n.at(33) == 123, "new generation retains instrument identity");
        });
}
void partial_output_revocation() {
    auto c = config();
    c.source_mode = DtcSourceMode::LiveTest;
    c.symbol_id = 17;
    c.max_write_bytes_per_poll = 64;
    Harness h(c);
    mark_live_refdata_proven(h.source);
    h.source.state.description.assign(512, 'X');
    const auto initial_definition = validate_dtc_security_definition(h.source.state, DtcSourceMode::LiveTest);
    check(initial_definition.available(), initial_definition.reason.c_str());
    h.logon();
    h.subscribe();
    check(h.count(507) == 1, "maximum-sized LiveTest description reaches the independent 507 decoder");
    h.got.clear();
    const auto request = h.definition_request();
    h.send(request);
    for (unsigned i = 0; i < 20 && h.server.queued_bytes() == 0; ++i) {
        h.server.poll();
        if (h.server.queued_bytes() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(h.server.queued_bytes() > 0 && h.server.security_definition_responses_fully_written() == 1,
          ("single oversized 507 remains incomplete after bounded socket write (queued=" +
           std::to_string(h.server.queued_bytes()) + ", client=" + (h.server.has_client() ? "true" : "false") +
           ", completed=" + std::to_string(h.server.security_definition_responses_fully_written()) +
           ", error=" + h.server.last_error() + ")")
              .c_str());
    h.source.state.contract_size = "20";
    h.server.poll();
    check(!h.server.has_client() && h.server.queued_bytes() == 0,
          "definition change closes instead of splicing a logoff into partial output");
    h.pump(5);
    check(h.eof && h.count(5) == 0, "partially written response is closed without appending a LOGOFF header");
    std::string decoder_error;
    check(!h.decoder.finish(decoder_error), "partial DTC frame is not completed with a different message");
}
void rejects() {
    for (float value : {0.0F, -1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto c = config();
        c.currency_value_per_increment = value;
        Harness h(c);
        h.logon();
        h.subscribe();
        check(h.count(509) == 1 && h.count(507) == 0 && h.count(145) == 0,
              "missing or invalid currency value per increment rejects metadata and depth");
    }
    {
        Harness h;
        h.source.security_definitions = false;
        h.logon(false);
        h.send(h.definition_request());
        h.pump(5);
        check(h.count(509) == 1 && h.count(507) == 0,
              "unsupported source security definitions reject 506 without emitting 507");
        h.source.security_definitions = true;
        h.got.clear();
        h.send(h.definition_request());
        h.pump(5);
        check(h.count(509) == 1 && h.count(507) == 0,
              "a capability denied at logon requires a fresh connection before upgrade");
    }
    {
        Harness h;
        h.source.optimistic_capabilities = true;
        const auto capabilities = h.source.capabilities();
        check(capabilities.accounts && capabilities.positions && capabilities.orders && capabilities.order_entry,
              "optimistic replay fixture exposes malicious account and order capabilities");
        h.logon();
        h.got.clear();
        h.send(packet(201, {}));
        h.pump(5);
        check(h.count(5) == 1 && h.eof && h.count(301) == 0,
              "read-only server does not propagate optimistic account/order capability");
        check(Read(h.first(5).payload).n[2] == 1, "optimistic capability request is fenced without reconnect");
    }
    for (std::uint8_t encoding : {0, 1, 2, 3, 5}) {
        Harness h;
        h.send({16, 0, 6, 0, 8, 0, 0, 0, encoding, 0, 0, 0, 'D', 'T', 'C', 0});
        h.pump(5);
        check(h.eof && h.count(7) == 0, "unsupported encoding rejected explicitly");
    }
    for (unsigned type : {201U, 203U, 204U, 208U, 209U, 210U, 300U, 303U, 305U, 309U, 400U, 601U, 607U, 10206U}) {
        Harness h;
        h.logon();
        h.got.clear();
        h.send(packet(type, {}));
        h.pump(5);
        check(h.count(5) == 1 && h.eof && h.count(301) == 0, "trading/account request rejected and disconnected");
        check(Read(h.first(5).payload).n[2] == 1, "prohibited request no reconnect");
    }
    {
        Harness h;
        h.logon();
        h.send(h.depth());
        h.pump(5);
        check(h.count(121) == 1 && h.count(145) == 0, "depth requires final definition");
    }
    {
        Harness h;
        h.send(packet(1, {8, 8}));
        h.pump(5);
        check(h.eof, "logon before negotiation fails");
    }
    {
        Harness h;
        h.send({3, 0, 6, 0});
        h.pump(5);
        check(h.eof, "invalid frame size fails");
    }
    {
        Harness h;
        h.send({1, 16, 6, 0});
        h.pump(5);
        check(h.eof, "oversized frame fails before allocation");
    }
    {
        Harness h;
        h.logon();
        h.send(packet(506, {10, 255, 255, 255}));
        h.pump(5);
        check(h.eof, "truncated protobuf fails");
    }
    {
        Harness h;
        h.logon();
        Bytes p;
        txt(p, 2, "\xc0\xaf");
        h.send(packet(506, p));
        h.pump(5);
        check(h.eof && h.count(507) == 0, "malformed UTF-8 request rejected");
    }
    {
        auto c = config();
        c.description = "\xff";
        Harness h(c);
        h.logon();
        h.subscribe();
        check(h.count(509) == 1 && h.count(507) == 0 && h.count(145) == 0, "malformed UTF-8 source metadata rejected");
    }
    {
        Harness h;
        h.source.state.levels[1].source_row_id = 0;
        h.logon();
        h.subscribe();
        check(h.count(145) == 0 && h.eof, "missing CGate identity never synthesized");
    }
}
void float32_wire_representability() {
    {
        Replay source;
        mark_live_refdata_proven(source);
        const auto ordinary_tick = validate_dtc_security_definition(source.state, DtcSourceMode::LiveTest);
        check(ordinary_tick.available() && ordinary_tick.definition->min_price_increment == 0.01F,
              "supported fractional tick remains a positive DTC float32 increment");

        source.state.contract_size = "16777217";
        const auto lossy_lot_volume = validate_dtc_security_definition(source.state, DtcSourceMode::LiveTest);
        check(!lossy_lot_volume.available() &&
                  lossy_lot_volume.reason ==
                      "current fut_instruments.lot_volume cannot be represented exactly by DTC float32 ContractSize",
              "lot_volume that rounds at the float32 integer precision boundary is unavailable");
    }
    {
        Harness exact;
        exact.source.state.levels[0].volume = 16777216;
        exact.logon();
        exact.subscribe();
        check(exact.count(145) == 2 && Read(exact.first(145).payload).f[3] == 16777216.0F,
              "largest consecutive float32 integer quantity is preserved on the wire");
    }
    {
        Harness lossy;
        lossy.source.state.levels[0].volume = 16777217;
        lossy.logon();
        lossy.subscribe();
        check(lossy.count(145) == 0 && lossy.eof && lossy.count(5) == 1 &&
                  lossy.server.last_error() == "DTC depth quantity is not exactly representable as float32",
              "source quantity that rounds from 16777217 to 16777216 is rejected before depth publication");
    }
}
void bounds() {
    {
        Harness h;
        h.source.state.levels.resize(1);
        h.logon();
        h.subscribe();
        check(h.count(145) == 1, "one-sided single-level snapshot");
        Read d(h.first(145).payload);
        check(d.n[7] == 1 && d.n[15] == 1, "single row uses FINAL_TRUE which also opens idle batch in Kairos");
    }
    {
        Harness h;
        h.logon();
        h.subscribe();
        h.got.clear();
        h.source.state.levels.clear();
        ++h.source.state.source_snapshot_version;
        h.pump();
        check(h.eof && h.count(5) == 1 && h.count(145) == 0,
              "empty book explicitly rejects and disconnects stale book");
    }
    {
        auto c = config();
        c.max_queued_bytes = 384;
        Harness h(c);
        h.logon();
        h.subscribe();
        check(h.eof && h.server.queued_bytes() <= 384 && h.count(145) == 0,
              "atomic batch queue limit fences without completed depth");
    }
    {
        auto c = config();
        c.idle_timeout = std::chrono::milliseconds(3);
        Harness h(c);
        h.pump(10);
        check(h.eof, "idle negotiation timeout");
    }
    {
        Harness h;
        h.logon();
        h.subscribe();
        h.got.clear();
        h.source.state.levels[0].price_scaled = -100001;
        h.source.state.levels.push_back({.price_scaled = -100002,
                                         .volume = 1,
                                         .side = DtcDepthSide::Bid,
                                         .source_row_id = 9010,
                                         .source_sequence = 57});
        ++h.source.state.source_snapshot_version;
        h.pump();
        check(h.count(145) == 3, "multiple levels sorted per side");
    }
}
void throwing_source() {
    for (unsigned mode : {1U, 2U}) {
        Harness h;
        h.logon();
        h.subscribe();
        check(h.count(145) == 2, "source healthy before injected failure");
        h.got.clear();
        h.source.failure_mode = mode;
        // No incoming request: this exercises sampling in subscribed poll,
        // outside the request-handler exception boundary.
        h.pump(5);
        check(h.eof && !h.server.has_client() && h.server.queued_bytes() == 0 && h.count(145) == 0,
              "throwing source disconnects and drops pending output without escaping poll");
        check(h.server.last_error() == "DTC source failure", "source failure diagnostic retained");
        h.source.failure_mode = 0;
        h.connect();
        h.logon();
        h.subscribe();
        check(h.count(145) == 2, "owner and listener survive source failure for fresh session");
    }
}
// Opt-in process fixture for the Rust Kairos integration test. Stdout is a
// single decimal port line; diagnostics belong on stderr. No live source is
// constructed, and source polling remains on the socket owner's thread.
int serve_fixture() {
    Replay source;
    DtcReadOnlyServer server(source, config());
    std::string error;
    if (!server.start(error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << server.port() << '\n' << std::flush;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool connected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        server.poll();
        if (server.has_client())
            connected = true;
        else if (connected)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    server.stop();
    return 0;
}

int serve_live507_fixture(std::uint32_t symbol_id, DtcSourceMode source_mode) {
    Replay source;
    if (source_mode == DtcSourceMode::LiveTest)
        mark_live_refdata_proven(source);
    auto server_config = config();
    server_config.port = 0;
    server_config.source_mode = source_mode;
    server_config.symbol_id = symbol_id;
    DtcReadOnlyServer server(source, server_config);
    std::string error;
    if (!server.start(error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << server.port() << '\n' << std::flush;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool connected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        server.poll();
        if (server.has_client())
            connected = true;
        else if (connected)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    server.stop();
    return connected ? 0 : 2;
}

int serve_live507_revocation_fixture() {
    Replay source;
    mark_live_refdata_proven(source);
    const auto initial_book_version = source.state.source_snapshot_version;
    auto server_config = config();
    server_config.port = 0;
    server_config.source_mode = DtcSourceMode::LiveTest;
    server_config.symbol_id = 7;
    DtcReadOnlyServer server(source, server_config);
    std::string error;
    if (!server.start(error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << server.port() << '\n' << std::flush;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    unsigned client_sessions = 0;
    bool was_connected = false;
    bool metadata_revoked = false;
    while (std::chrono::steady_clock::now() < deadline) {
        server.poll();
        const bool connected = server.has_client();
        if (connected && !was_connected)
            ++client_sessions;

        // Wait for the first real 507 bytes to be fully written to the local
        // socket, then revoke only a definition term. The AGGR book version
        // remains byte-for-byte unchanged for this same-client fence test.
        if (client_sessions == 1 && connected && !metadata_revoked &&
            server.security_definition_responses_fully_written() > 0) {
            source.state.description = "Revised TEST future";
            source.state.currency_value_per_increment = "3.75";
            metadata_revoked = true;
        }

        if (client_sessions == 2 && !connected && was_connected)
            break;
        was_connected = connected;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool completed = client_sessions == 2 && metadata_revoked &&
                           server.security_definition_responses_fully_written() >= 2 &&
                           source.state.source_snapshot_version == initial_book_version;
    server.stop();
    if (!completed)
        std::cerr << "LiveTest metadata-revocation fixture did not complete two clean local sessions\n";
    return completed ? 0 : 2;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--serve-fixture")
        return serve_fixture();
    if (argc == 4 && std::string(argv[1]) == "--serve-live507-fixture") {
        char* end = nullptr;
        const auto parsed = std::strtoul(argv[2], &end, 10);
        if (!end || *end != '\0' || parsed == 0 || parsed > UINT32_MAX ||
            (std::string_view(argv[3]) != "live" && std::string_view(argv[3]) != "replay"))
            return 2;
        return serve_live507_fixture(static_cast<std::uint32_t>(parsed), std::string_view(argv[3]) == "live"
                                                                             ? DtcSourceMode::LiveTest
                                                                             : DtcSourceMode::Replay);
    }
    if (argc == 2 && std::string(argv[1]) == "--serve-live507-revocation-fixture")
        return serve_live507_revocation_fixture();
    if (argc != 1) {
        std::cerr << "Usage: connector_host_dtc_server_test [--serve-fixture|--serve-live507-fixture ID live|replay|"
                     "--serve-live507-revocation-fixture]\n";
        return 2;
    }
    replay_roundtrip();
    live_test_metadata_and_auth();
    live_test_late_refdata_after_logon();
    revoke_and_reconnect();
    definition_revocation();
    partial_output_revocation();
    rejects();
    float32_wire_representability();
    bounds();
    throwing_source();
    std::cout << "DTC loopback E2E: negotiation, definitions, UTF-8, authority, signed depth, replay, revocation, "
                 "rejection and bounds passed\n";
}
