#include "moex/connector_host/dtc_read_only_server.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
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
struct Replay : DtcMarketDataSource {
    DtcMarketDataSnapshot state;
    bool security_definitions{true};
    bool optimistic_capabilities{false};
    unsigned failure_mode{0};
    Replay() {
        state.symbol = "ALRS-12.26";
        state.board = "FORTS";
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
        check(false, "expected response type missing");
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
        pump(5);
        if (!expect_success)
            return;
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
        txt(p, 3, source.state.board);
        return packet(506, p);
    }
    Bytes depth(unsigned action = 1, unsigned id = 0) {
        if (id == 0)
            id = server.symbol_id() == 0 ? 7 : server.symbol_id();
        Bytes p;
        num(p, 1, action);
        num(p, 2, id);
        txt(p, 3, source.state.symbol);
        txt(p, 4, source.state.board);
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
    const auto mark_refdata_vcb_proven = [](Harness& harness) {
        harness.source.state.refdata_vcb_join_current = true;
        harness.source.state.future_vcb_provenance_present = true;
        harness.source.state.refdata_board_proven = true;
        harness.source.state.refdata_currency_proven = true;
    };
    auto c = config();
    c.source_mode = DtcSourceMode::LiveTest;
    c.symbol_id = 17;
    // Deliberately conflicting replay terms must be ignored by live_test.
    c.currency = "REPLAY-CURRENCY";
    c.description = "replay description must not leak";
    c.contract_size = 999;
    c.currency_value_per_increment = 999;
    Harness h(c);
    mark_refdata_vcb_proven(h);
    h.source.state.description = "Authoritative TEST future";
    h.source.state.currency = "RUB";
    h.source.state.contract_size = "10";
    h.source.state.currency_value_per_increment = "2.5";
    h.logon();
    h.subscribe();
    Read definition(h.first(507).payload);
    check(definition.n[4] == 1 && definition.n[9] == 1 && definition.n[33] == 123,
          "live TEST definition uses FUTURE/final fields and source ISIN, not DTC symbol ID");
    check(definition.s[5] == "Authoritative TEST future" && definition.s[28] == "RUB" && definition.f[29] == 10 &&
              definition.f[8] == 2.5F,
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
    mark_refdata_vcb_proven(unsupported_currency);
    unsupported_currency.source.state.description = "Authoritative TEST future";
    unsupported_currency.source.state.currency = "USD";
    unsupported_currency.source.state.refdata_currency_proven = false;
    unsupported_currency.source.state.contract_size = "10";
    unsupported_currency.source.state.currency_value_per_increment = "2.5";
    unsupported_currency.logon(false);
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
    mark_refdata_vcb_proven(invalid);
    invalid.source.state.description.clear();
    invalid.source.state.currency.clear();
    invalid.source.state.contract_size.clear();
    invalid.source.state.currency_value_per_increment.clear();
    invalid.logon(false);
    invalid.subscribe();
    check(invalid.count(509) == 1 && invalid.count(507) == 0 && invalid.count(145) == 0,
          "live TEST missing authoritative 507 metadata fails closed");

    Harness ambiguous(c);
    mark_refdata_vcb_proven(ambiguous);
    ambiguous.source.state.refdata_vcb_join_ambiguous = true;
    ambiguous.source.state.description = "Authoritative TEST future";
    ambiguous.source.state.currency = "RUB";
    ambiguous.source.state.contract_size = "10";
    ambiguous.source.state.currency_value_per_increment = "2.5";
    ambiguous.logon(false);
    ambiguous.subscribe();
    check(ambiguous.count(509) == 1 && ambiguous.count(507) == 0, "live TEST ambiguous fut_vcb join fails 507 closed");

    auto auth = c;
    auth.require_local_auth = true;
    auth.local_username = "local-user";
    auth.local_password = "local-password";
    Harness wrong(auth);
    mark_refdata_vcb_proven(wrong);
    wrong.logon(false, "wrong-user", "wrong-password", false);
    wrong.pump(3);
    check(wrong.count(5) == 1 && wrong.eof, "dedicated local DTC credentials fence invalid logon");
    Harness correct(auth);
    mark_refdata_vcb_proven(correct);
    correct.source.state.description = "Authoritative TEST future";
    correct.source.state.currency = "RUB";
    correct.source.state.contract_size = "10";
    correct.source.state.currency_value_per_increment = "2.5";
    correct.logon(true, "local-user", "local-password");
    check(correct.count(2) == 1, "dedicated local DTC credentials permit valid logon");
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
void rejects() {
    for (float value : {0.0F, -1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto c = config();
        c.currency_value_per_increment = value;
        Harness h(c);
        h.logon(false);
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
        h.logon(false);
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
    source.state.refdata_vcb_join_current = source_mode == DtcSourceMode::LiveTest;
    source.state.future_vcb_provenance_present = source_mode == DtcSourceMode::LiveTest;
    source.state.refdata_board_proven = source_mode == DtcSourceMode::LiveTest;
    source.state.refdata_currency_proven = source_mode == DtcSourceMode::LiveTest;
    source.state.description = "Authoritative test future";
    source.state.currency = "RUB";
    source.state.contract_size = "10";
    source.state.currency_value_per_increment = "2.5";
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
    if (argc != 1) {
        std::cerr << "Usage: connector_host_dtc_server_test [--serve-fixture|--serve-live507-fixture ID live|replay]\n";
        return 2;
    }
    replay_roundtrip();
    live_test_metadata_and_auth();
    revoke_and_reconnect();
    rejects();
    bounds();
    throwing_source();
    std::cout << "DTC loopback E2E: negotiation, definitions, UTF-8, authority, signed depth, replay, revocation, "
                 "rejection and bounds passed\n";
}
