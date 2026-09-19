#include "moex/connector_host/dtc_read_only_server.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace moex::connector_host::dtc {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxLocalCredentialBytes = 256;

// This DTC endpoint currently exposes futures books only. Keep the instrument
// kind to wire-enum mapping explicit and independent of the client SymbolID.
enum class SupportedInstrumentKind : std::uint8_t { Unsupported, Future };
enum class DtcSecurityType : std::uint32_t { Future = 1 };

std::optional<DtcSecurityType> dtc_security_type(SupportedInstrumentKind kind) noexcept {
    switch (kind) {
    case SupportedInstrumentKind::Unsupported:
        return std::nullopt;
    case SupportedInstrumentKind::Future:
        return DtcSecurityType::Future;
    }
    return std::nullopt;
}

std::optional<SupportedInstrumentKind> supported_instrument_kind(const DtcMarketDataSnapshot& snapshot,
                                                                 DtcSourceMode source_mode) noexcept {
    if (source_mode == DtcSourceMode::Replay)
        return SupportedInstrumentKind::Future; // replay definition fixtures in this endpoint are futures
    if (snapshot.refdata_vcb_join_current && snapshot.future_vcb_provenance_present)
        return SupportedInstrumentKind::Future;
    return std::nullopt;
}

// Minimal wire subset of https://www.sierrachart.com/DTC_Files/DTCProtocol.proto
// (DTC v8). Fields 8-20 of message 145 are the pinned Kairos extension, not
// official DTC fields. No protobuf library or generated files are required.
void varint(Bytes& out, std::uint64_t n) {
    while (n >= 128) {
        out.push_back(static_cast<std::uint8_t>(n) | 128);
        n >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(n));
}
void integer(Bytes& out, unsigned field, std::uint64_t n) {
    varint(out, field * 8);
    varint(out, n);
}
void str(Bytes& out, unsigned field, const std::string& s) {
    if (!plaza2::cgate::text::valid_utf8(s))
        throw std::runtime_error("invalid UTF-8 output");
    varint(out, field * 8 + 2);
    varint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}
void real(Bytes& out, unsigned field, float n) {
    varint(out, field * 8 + 5);
    auto bits = std::bit_cast<std::uint32_t>(n);
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
}
Bytes frame(std::uint16_t type, const Bytes& payload) {
    const auto size = payload.size() + 4;
    if (size > UINT16_MAX)
        throw std::runtime_error("DTC output frame too large");
    Bytes out{static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(type),
              static_cast<std::uint8_t>(type >> 8)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
bool utf8(const std::string& s) {
    return plaza2::cgate::text::valid_utf8(s);
}
struct Field {
    unsigned wire{};
    std::uint64_t n{};
    std::string s;
};
struct Proto {
    std::map<unsigned, Field> fields;
    explicit Proto(const Bytes& b) {
        std::size_t pos = 0;
        auto read = [&]() {
            std::uint64_t n = 0;
            for (unsigned i = 0; i < 10; ++i) {
                if (pos == b.size())
                    throw std::runtime_error("truncated protobuf");
                const auto c = b[pos++];
                if (i == 9 && c > 1)
                    throw std::runtime_error("protobuf varint overflow");
                n |= std::uint64_t(c & 127) << (i * 7);
                if (!(c & 128))
                    return n;
            }
            throw std::runtime_error("protobuf varint overflow");
        };
        while (pos < b.size()) {
            auto tag = read();
            if ((tag >> 3) == 0 || (tag >> 3) > 536870911)
                throw std::runtime_error("invalid protobuf tag");
            Field f;
            f.wire = tag & 7;
            if (f.wire == 0)
                f.n = read();
            else if (f.wire == 2) {
                const auto len = read();
                if (len > b.size() - pos)
                    throw std::runtime_error("truncated protobuf string");
                f.s.assign(reinterpret_cast<const char*>(b.data() + pos), static_cast<std::size_t>(len));
                pos += static_cast<std::size_t>(len);
            } else if (f.wire == 1 || f.wire == 5) {
                auto len = f.wire == 1 ? 8U : 4U;
                if (len > b.size() - pos)
                    throw std::runtime_error("truncated protobuf fixed field");
                pos += len;
            } else
                throw std::runtime_error("unsupported protobuf wire type");
            fields[static_cast<unsigned>(tag >> 3)] = std::move(f);
        }
    }
    std::uint64_t number(unsigned key) const {
        auto it = fields.find(key);
        if (it == fields.end())
            return 0;
        if (it->second.wire != 0)
            throw std::runtime_error("wrong protobuf integer type");
        return it->second.n;
    }
    std::string text(unsigned key) const {
        auto it = fields.find(key);
        if (it == fields.end())
            return {};
        if (it->second.wire != 2 || !utf8(it->second.s))
            throw std::runtime_error("invalid protobuf text");
        return it->second.s;
    }
};
unsigned witness_number(plaza2::cgate::SessionReadyWitnessKind a) {
    using Kind = plaza2::cgate::SessionReadyWitnessKind;
    switch (a) {
    case Kind::OnlineSynchronousEvent:
        return 1;
    case Kind::PersistedOnlineWitness:
        return 2;
    case Kind::LateJoinCorroboratedSnapshot:
        return 3;
    default:
        return 0;
    }
}
bool nonblocking(int fd) {
    const auto flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}
void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}
bool bounded_constant_time_equal(std::string_view actual, std::string_view expected) noexcept {
    // The loop count is independent of both values and lengths.  Length is
    // folded into the result only after every bounded byte has been read.
    std::uint64_t different = actual.size() ^ expected.size();
    for (std::size_t i = 0; i < kMaxLocalCredentialBytes; ++i) {
        const auto actual_byte = i < actual.size() ? static_cast<std::uint8_t>(actual[i]) : 0;
        const auto expected_byte = i < expected.size() ? static_cast<std::uint8_t>(expected[i]) : 0;
        different |= actual_byte ^ expected_byte;
    }
    return different == 0 && actual.size() <= kMaxLocalCredentialBytes;
}
bool positive_float(std::string_view text, float& value) noexcept {
    if (text.empty() || text.size() > 64)
        return false;
    try {
        std::size_t end = 0;
        value = std::stof(std::string(text), &end);
        return end == text.size() && std::isfinite(value) && value > 0;
    } catch (...) {
        return false;
    }
}
} // namespace

struct DtcReadOnlyServer::Impl {
    DtcMarketDataSource& source;
    DtcReadOnlyServerConfig config;
    DtcFrameDecoder decoder;
    int listener{-1}, client{-1};
    std::uint16_t bound_port{};
    Bytes output;
    std::size_t sent{};
    bool negotiated{}, logged_on{}, closing{}, definition{}, subscribed{}, definitions_advertised{};
    std::uint32_t symbol_id{};
    std::size_t depth_limit{};
    std::uint64_t batch{}, version{}, epoch{}, authority_epoch{};
    std::string symbol, board, tick, error;
    std::int64_t isin{};
    unsigned witness{};
    Clock::time_point last_read{}, last_write{}, last_heartbeat{};
    std::chrono::seconds heartbeat{10};

    struct DefinitionMetadata {
        std::string currency;
        std::string description;
        float contract_size{};
        float currency_value_per_increment{};
    };

    Impl(DtcMarketDataSource& s, DtcReadOnlyServerConfig c)
        : source(s), config(std::move(c)), decoder(config.max_frame_bytes) {
        if (config.max_frame_bytes < 16 || config.max_frame_bytes > UINT16_MAX || config.max_queued_bytes < 64 ||
            config.max_queued_bytes > 1024 * 1024 || config.max_depth_levels == 0 || config.max_depth_levels > 20 ||
            config.idle_timeout.count() <= 0 || config.write_timeout.count() <= 0)
            throw std::invalid_argument("invalid DTC server bounds");
        if (config.require_local_auth && (config.local_username.empty() || config.local_password.empty() ||
                                          config.local_username.size() > kMaxLocalCredentialBytes ||
                                          config.local_password.size() > kMaxLocalCredentialBytes ||
                                          !utf8(config.local_username) || !utf8(config.local_password)))
            throw std::invalid_argument("invalid bounded DTC local credentials");
        symbol_id = config.symbol_id;
    }
    void disconnect() {
        close_fd(client);
        decoder.reset();
        output.clear();
        sent = 0;
        negotiated = logged_on = closing = definition = subscribed = false;
        definitions_advertised = false;
        symbol_id = config.symbol_id;
        batch = version = epoch = authority_epoch = 0;
    }
    bool queue(std::uint16_t type, const Bytes& body) {
        auto bytes = frame(type, body);
        if (bytes.size() > config.max_frame_bytes || bytes.size() > config.max_queued_bytes - output.size()) {
            error = "DTC backpressure limit exceeded; fresh connection required";
            disconnect();
            return false;
        }
        if (output.empty())
            last_write = Clock::now();
        output.insert(output.end(), bytes.begin(), bytes.end());
        return true;
    }
    void reject(const std::string& reason) {
        error = reason;
        // A partially written frame cannot be replaced by a LOGOFF header.
        if (sent) {
            disconnect();
            return;
        }
        // Do not leave stale queued market data ahead of a fencing response.
        output.clear();
        sent = 0;
        Bytes p;
        str(p, 1, reason);
        integer(p, 2, 1);
        if (queue(5, p))
            closing = true;
    }
    bool usable(const DtcMarketDataSnapshot& s) const {
        // source_consistent is the online synchronization proof, deliberately
        // false for corroborated late joins. Display permission is separate.
        return s.market_data_display_allowed && s.transport_active && s.aggr_online && s.snapshot_complete &&
               s.book_snapshot_current && s.refdata_metadata_current && source_metadata_usable(s);
    }
    std::string source_error(std::string_view detail) const {
        return "source_mode=" + std::string(dtc_source_mode_name(config.source_mode)) + ": " + std::string(detail);
    }
    bool source_metadata_usable(const DtcMarketDataSnapshot& s) const noexcept {
        if (config.source_mode == DtcSourceMode::Replay)
            return true;
        return s.refdata_vcb_join_current && !s.refdata_vcb_join_ambiguous && s.future_vcb_provenance_present &&
               s.refdata_board_proven && s.refdata_currency_proven;
    }
    bool metadata(const DtcMarketDataSnapshot& s, float& increment, DefinitionMetadata& terms) const {
        if (!s.refdata_metadata_current || s.isin_id <= 0 || s.symbol.empty() || s.board.empty() ||
            s.symbol.size() > 128 || s.board.size() > 128 || !utf8(s.symbol) || !utf8(s.board) ||
            !positive_float(s.min_step, increment))
            return false;
        if (config.source_mode == DtcSourceMode::Replay) {
            // Replay fixtures retain their explicit economics for compatibility.
            // They are never used by the live TEST path.
            if (config.currency.empty() || config.currency.size() > 16 || !utf8(config.currency) ||
                config.description.empty() || config.description.size() > 512 || !utf8(config.description) ||
                !std::isfinite(config.contract_size) || config.contract_size <= 0 ||
                !std::isfinite(config.currency_value_per_increment) || config.currency_value_per_increment <= 0)
                return false;
            terms = {.currency = config.currency,
                     .description = config.description,
                     .contract_size = config.contract_size,
                     .currency_value_per_increment = config.currency_value_per_increment};
            return true;
        }
        // Live TEST definitions use only the current ConnectorHost snapshot.
        // Missing source terms fail closed; no value is derived from min_step,
        // lot size, or a remembered replay fixture.
        if (!source_metadata_usable(s))
            return false;
        if (s.description.empty() || s.description.size() > 512 || !utf8(s.description) || s.currency.empty() ||
            s.currency.size() > 16 || !utf8(s.currency))
            return false;
        if (!positive_float(s.contract_size, terms.contract_size) ||
            !positive_float(s.currency_value_per_increment, terms.currency_value_per_increment))
            return false;
        terms.currency = s.currency;
        terms.description = s.description;
        return true;
    }
    bool security_definitions_available(const DtcMarketDataSnapshot& s, float& increment) const {
        DefinitionMetadata terms;
        return source.capabilities().security_definitions && metadata(s, increment, terms);
    }
    bool security_definitions_available(const DtcMarketDataSnapshot& s) const {
        float increment{};
        return security_definitions_available(s, increment);
    }
    void status(const DtcMarketDataSnapshot& s) {
        Bytes p;
        integer(p, 1, usable(s) ? 2 : 1);
        if (!queue(100, p))
            return;
        // Standard GENERAL_LOG_MESSAGE field 3, valid UTF-8, explicit source
        // authority disclosure. Consumers must not infer order authorization.
        p.clear();
        str(p, 3,
            std::string("source_mode=") + std::string(dtc_source_mode_name(config.source_mode)) + ";authority=" +
                std::string(plaza2::cgate::session_ready_witness_kind_name(s.session_ready_witness_kind)) +
                ";transport_active=" + (s.transport_active ? "true" : "false") +
                ";snapshot_current=" + (usable(s) ? "true" : "false") +
                ";order_entry_allowed=false;accounts=false;positions=false;orders=false");
        queue(701, p);
    }
    bool authority(const DtcMarketDataSnapshot& s) {
        Bytes p;
        auto boolean = [](bool value) { return value ? "true" : "false"; };
        const auto name = plaza2::cgate::session_ready_witness_kind_name(s.session_ready_witness_kind);
        str(p, 1,
            std::string("moex.source_authority.v1 {\"symbol_id\":") + std::to_string(symbol_id) +
                ",\"source_mode\":\"" + std::string(dtc_source_mode_name(config.source_mode)) +
                "\",\"stream_epoch\":" + std::to_string(s.stream_epoch) + ",\"aggr_online\":" + boolean(s.aggr_online) +
                ",\"book_snapshot_current\":" + boolean(s.book_snapshot_current) +
                ",\"session_ready_witness_kind\":\"" + plaza2::cgate::text::json_escape_utf8(name) +
                "\",\"market_data_display_allowed\":" + boolean(s.market_data_display_allowed) +
                ",\"order_entry_allowed\":false,\"exchange_confirmed\":false}");
        integer(p, 2, 0);
        return queue(kDtcSourceAuthorityMessage, p);
    }
    void snapshot(const DtcMarketDataSnapshot& s) {
        if (!usable(s)) {
            reject("source authority unavailable; fresh session required");
            return;
        }
        if (s.symbol != symbol || s.board != board || s.min_step != tick || s.isin_id != isin) {
            reject("source metadata changed; fresh definition required");
            return;
        }
        if (s.levels.empty() || s.levels.size() > 40 || !s.stream_epoch || !s.source_snapshot_version) {
            reject(source_error("invalid depth snapshot"));
            return;
        }
        auto rows = s.levels;
        std::set<std::uint64_t> ids;
        for (const auto& row : rows) {
            if ((row.side != DtcDepthSide::Bid && row.side != DtcDepthSide::Ask) || row.volume <= 0 ||
                !row.source_row_id || !row.source_sequence || row.source_sequence > INT64_MAX ||
                !ids.insert(row.source_row_id).second) {
                reject(source_error("invalid row identity or quantity"));
                return;
            }
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (a.side != b.side)
                return a.side < b.side;
            return a.side == DtcDepthSide::Bid ? a.price_scaled > b.price_scaled : a.price_scaled < b.price_scaled;
        });
        std::array<std::size_t, 3> counts{};
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const auto& r) { return ++counts[static_cast<unsigned>(r.side)] > depth_limit; }),
                   rows.end());
        std::array<float, 3> previous{};
        counts = {};
        std::uint64_t next_batch = batch + 1;
        if (!next_batch) {
            reject("DTC batch overflow");
            return;
        }
        // Stage an entire batch before queuing any bytes. A capacity failure
        // closes the socket; a partial batch can never acquire a final marker.
        Bytes staged;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            auto side = static_cast<unsigned>(r.side);
            const auto price = static_cast<float>(static_cast<double>(r.price_scaled) / 100000.0);
            if (!std::isfinite(price) ||
                (counts[side] && (side == 1 ? price >= previous[side] : price <= previous[side]))) {
                reject("DTC float price collision or invalid level ordering");
                return;
            }
            previous[side] = price;
            Bytes p;
            integer(p, 1, symbol_id);
            real(p, 2, price);
            real(p, 3, static_cast<float>(r.volume));
            integer(p, 5, ++counts[side]);
            integer(p, 6, side);
            integer(p, 7, i + 1 == rows.size() ? 1 : i == 0 ? 3 : 2);
            // Exchange time is source time; no fabricated ingress timestamps.
            integer(p, 9, r.exchange_moment_ns / 1000000);
            integer(p, 11, s.stream_epoch);
            integer(p, 12, next_batch);
            integer(p, 13, s.source_snapshot_version);
            integer(p, 14, s.snapshot_watermark);
            integer(p, 15, rows.size());
            integer(p, 18, s.source_snapshot_hash);
            integer(p, 19, r.source_sequence);
            integer(p, 20, r.source_row_id);
            auto f = frame(145, p);
            if (f.size() > config.max_frame_bytes || staged.size() + f.size() > config.max_queued_bytes) {
                error = "DTC snapshot exceeds queue bounds";
                disconnect();
                return;
            }
            staged.insert(staged.end(), f.begin(), f.end());
        }
        // Feed/symbol AVAILABLE precedes authority, which precedes depth.
        Bytes available;
        integer(available, 1, 2);
        if (!queue(100, available))
            return;
        available.clear();
        integer(available, 1, symbol_id);
        integer(available, 2, 2);
        if (!queue(116, available) || !authority(s))
            return;
        if (staged.size() > config.max_queued_bytes - output.size()) {
            error = "DTC snapshot backpressure";
            disconnect();
            return;
        }
        if (output.empty())
            last_write = Clock::now();
        output.insert(output.end(), staged.begin(), staged.end());
        batch = next_batch;
        version = s.source_snapshot_version;
        epoch = s.stream_epoch;
        authority_epoch = s.market_data_authority_epoch;
        witness = witness_number(s.session_ready_witness_kind);
    }
    void handle(const DtcFrame& f) {
        if (!negotiated) {
            // Official binary EncodingRequest: int32 version, int32 encoding,
            // char[4] protocol. Never parse this handshake as protobuf.
            const Bytes expected{8, 0, 0, 0, 4, 0, 0, 0, 'D', 'T', 'C', 0};
            if (f.message_type != 6 || f.payload.size() != 12 ||
                !std::equal(expected.begin(), expected.begin() + 4, f.payload.begin()) ||
                !std::equal(expected.begin() + 8, expected.end(), f.payload.begin() + 8)) {
                error = "invalid DTC binary negotiation";
                disconnect();
                return;
            }
            // DTC permits replying with our sole supported encoding even for
            // another request. This bounded endpoint instead closes unsupported
            // encodings explicitly so a JSON/binary-only peer cannot proceed.
            if (!std::equal(expected.begin() + 4, expected.begin() + 8, f.payload.begin() + 4)) {
                error = "unsupported DTC encoding; protobuf required";
                disconnect();
                return;
            }
            if (queue(7, expected))
                negotiated = true;
            return;
        }
        Proto p(f.payload);
        if (!logged_on) {
            if (f.message_type != 1 || p.number(1) != 8) {
                reject("DTC v8 logon required");
                return;
            }
            // Validate text without storing/logging credentials.
            for (auto key : {2U, 3U, 4U, 9U, 10U, 11U})
                (void)p.text(key);
            if (config.require_local_auth) {
                const auto username = p.text(2);
                const auto password = p.text(3);
                const bool username_ok = bounded_constant_time_equal(username, config.local_username);
                const bool password_ok = bounded_constant_time_equal(password, config.local_password);
                if (!username_ok || !password_ok) {
                    reject("DTC local authentication failed");
                    return;
                }
            }
            const auto interval = p.number(7);
            if (interval > 60) {
                reject("heartbeat interval exceeds 60 seconds");
                return;
            }
            heartbeat = std::chrono::seconds(interval ? interval : 10);
            const auto s = source.snapshot();
            Bytes reply;
            integer(reply, 1, 8);
            integer(reply, 2, 1);
            str(reply, 3,
                config.source_mode == DtcSourceMode::LiveTest ? "Read-only live TEST; trading disabled"
                                                              : "Read-only replay; trading disabled");
            str(reply, 6, config.source_mode == DtcSourceMode::LiveTest ? "MOEX live TEST DTC" : "MOEX replay DTC");
            for (auto field : {7U, 8U, 9U, 10U, 13U, 17U, 19U, 20U})
                integer(reply, field, 0);
            // Advertise exactly what a 506 request can satisfy for this
            // source and this explicitly configured source-mode endpoint.
            definitions_advertised = security_definitions_available(s);
            integer(reply, 12, definitions_advertised);
            integer(reply, 14, 1);
            integer(reply, 15, source.capabilities().market_depth);
            if (queue(2, reply)) {
                logged_on = true;
                status(s);
            }
            return;
        }
        if (f.message_type == 3)
            return;
        if (f.message_type == 5) {
            disconnect();
            return;
        }
        if (f.message_type == 506) {
            auto s = source.snapshot();
            float increment{};
            DefinitionMetadata terms;
            // LiveTest futures are derived from the committed fut_vcb join;
            // replay fixtures for this endpoint have the explicit futures
            // contract. Unsupported/unknown kinds have no wire enum mapping.
            const auto kind = supported_instrument_kind(s, config.source_mode);
            const auto security_type = kind ? dtc_security_type(*kind) : std::nullopt;
            Bytes reply;
            integer(reply, 1, p.number(1));
            if (!security_type || !definitions_advertised || !security_definitions_available(s, increment) ||
                !metadata(s, increment, terms) || p.text(2) != s.symbol || p.text(3) != s.board) {
                str(reply, 2, "unknown instrument or metadata unavailable");
                queue(509, reply);
                return;
            }
            // ConnectorHost's board is the raw committed provider value. The
            // runner does not translate it into a guessed exchange code.
            str(reply, 2, s.symbol);
            str(reply, 3, s.board);
            integer(reply, 4, static_cast<std::uint32_t>(*security_type));
            str(reply, 5, terms.description);
            real(reply, 6, increment);
            real(reply, 8, terms.currency_value_per_increment);
            // A 507 is the complete final response for this single-instrument
            // server, in both replay and live TEST modes.
            integer(reply, 9, 1);
            if (config.source_mode == DtcSourceMode::Replay) {
                // Preserve the existing replay fixture wire contract. These
                // optional fields are intentionally absent for live TEST
                // until each value has an authoritative source mapping.
                integer(reply, 7, 5);
                real(reply, 10, 1);
                real(reply, 11, 1);
                real(reply, 22, 1);
            }
            integer(reply, 23, source.capabilities().market_depth);
            if (config.source_mode == DtcSourceMode::Replay)
                real(reply, 24, 1);
            str(reply, 28, terms.currency);
            real(reply, 29, terms.contract_size);
            integer(reply, 33, static_cast<std::uint64_t>(s.isin_id));
            if (queue(507, reply)) {
                definition = true;
                symbol = s.symbol;
                board = s.board;
                tick = s.min_step;
                isin = s.isin_id;
            }
            return;
        }
        if (f.message_type == 101) {
            Bytes reply;
            integer(reply, 1, p.number(2));
            str(reply, 2, "AGGR " + std::string(dtc_source_mode_name(config.source_mode)) + " is depth only");
            queue(103, reply);
            return;
        }
        if (f.message_type == 102) {
            const auto id = p.number(2), action = p.number(1), levels = p.number(5);
            if (action == 2 && id == symbol_id) {
                subscribed = false;
                return;
            }
            auto s = source.snapshot();
            if (!definition || !source.capabilities().market_depth || !id || id > UINT32_MAX ||
                (action != 1 && action != 3) || p.text(3) != symbol || p.text(4) != board ||
                levels > config.max_depth_levels || (config.symbol_id && id != config.symbol_id) ||
                (subscribed && id != symbol_id)) {
                Bytes reply;
                integer(reply, 1, id);
                str(reply, 2, "invalid or unsupported depth subscription");
                queue(121, reply);
                return;
            }
            symbol_id = static_cast<std::uint32_t>(id);
            depth_limit = levels ? static_cast<std::size_t>(levels) : config.max_depth_levels;
            subscribed = action == 1;
            snapshot(s);
            return;
        }
        reject("Read-only DTC: trading, accounts and unsupported requests prohibited");
    }
    void poll() {
        if (listener < 0)
            return;
        if (client < 0) {
            client = ::accept(listener, nullptr, nullptr);
            if (client < 0)
                return;
            if (!nonblocking(client)) {
                error = "cannot configure client socket";
                disconnect();
                return;
            }
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            last_read = last_write = last_heartbeat = Clock::now();
        }
        auto now = Clock::now();
        if (now - last_read > config.idle_timeout || (!output.empty() && now - last_write > config.write_timeout)) {
            error = "DTC session timeout";
            disconnect();
            return;
        }
        if (!closing) {
            std::array<std::uint8_t, 4096> buffer{};
            // Fixed per-poll I/O budget prevents a flooding peer starving owner.
            const auto n = ::recv(client, buffer.data(), buffer.size(), 0);
            if (n == 0) {
                disconnect();
                return;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                disconnect();
                return;
            }
            if (n > 0) {
                last_read = now;
                std::vector<DtcFrame> frames;
                if (!decoder.append(std::span(buffer.data(), static_cast<std::size_t>(n)), frames, error)) {
                    disconnect();
                    return;
                }
                try {
                    for (const auto& f : frames) {
                        handle(f);
                        if (client < 0 || closing)
                            break;
                    }
                } catch (const std::exception&) {
                    if (client >= 0)
                        reject("invalid DTC request or source failure");
                }
            }
            if (client < 0)
                return;
            if (logged_on && !closing && subscribed) {
                auto s = source.snapshot();
                if (!usable(s) || s.symbol != symbol || s.board != board || s.min_step != tick || s.isin_id != isin ||
                    s.market_data_authority_epoch != authority_epoch ||
                    witness_number(s.session_ready_witness_kind) != witness) {
                    // Discard unsent stale batches. If a frame was partially
                    // written, close immediately rather than corrupt framing.
                    if (sent) {
                        disconnect();
                        return;
                    }
                    output.clear();
                    if (!authority(s))
                        return;
                    Bytes unavailable;
                    integer(unavailable, 1, symbol_id);
                    integer(unavailable, 2, 1);
                    if (!queue(116, unavailable))
                        return;
                    Bytes logoff;
                    str(logoff, 1, "source authority or metadata changed; fresh session required");
                    integer(logoff, 2, 0);
                    if (queue(5, logoff))
                        closing = true;
                } else if (s.source_snapshot_version != version || s.stream_epoch != epoch)
                    snapshot(s);
            }
            if (logged_on && !closing && now - last_heartbeat >= heartbeat) {
                queue(3, {});
                last_heartbeat = now;
            }
        }
        if (client < 0)
            return;
        if (!output.empty()) {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            const auto n =
                ::send(client, output.data() + sent, std::min<std::size_t>(4096, output.size() - sent), flags);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                last_write = now;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                disconnect();
                return;
            }
            if (sent == output.size()) {
                output.clear();
                sent = 0;
            }
        }
        if (closing && output.empty())
            disconnect();
    }
};

DtcReadOnlyServer::DtcReadOnlyServer(DtcMarketDataSource& s, DtcReadOnlyServerConfig c)
    : impl_(std::make_unique<Impl>(s, std::move(c))) {}
DtcReadOnlyServer::~DtcReadOnlyServer() {
    stop();
}
bool DtcReadOnlyServer::start(std::string& error) {
    error.clear();
    if (impl_->listener >= 0) {
        error = "DTC server already started";
        return false;
    }
    auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(impl_->config.port);
    if (fd < 0 || !nonblocking(fd) || ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 1) < 0) {
        error = std::string("DTC loopback listen failed: ") + std::strerror(errno);
        close_fd(fd);
        return false;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        error = "DTC getsockname failed";
        close_fd(fd);
        return false;
    }
    impl_->listener = fd;
    impl_->bound_port = ntohs(addr.sin_port);
    return true;
}
void DtcReadOnlyServer::poll() {
    try {
        impl_->poll();
    } catch (...) {
        // Covers request handling and subscribed polling/projection alike,
        // including sources that throw a non-std exception. Never propagate
        // source failures into the caller's UI/control-plane owner loop.
        impl_->error = "DTC source failure";
        impl_->disconnect();
    }
}
void DtcReadOnlyServer::stop() noexcept {
    impl_->disconnect();
    close_fd(impl_->listener);
    impl_->bound_port = 0;
}
std::uint16_t DtcReadOnlyServer::port() const noexcept {
    return impl_->bound_port;
}
std::uint32_t DtcReadOnlyServer::symbol_id() const noexcept {
    return impl_->symbol_id;
}
bool DtcReadOnlyServer::has_client() const noexcept {
    return impl_->client >= 0;
}
std::size_t DtcReadOnlyServer::queued_bytes() const noexcept {
    return impl_->output.size() - impl_->sent;
}
const std::string& DtcReadOnlyServer::last_error() const noexcept {
    return impl_->error;
}
} // namespace moex::connector_host::dtc
