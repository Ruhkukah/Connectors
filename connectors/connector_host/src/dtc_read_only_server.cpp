#include "moex/connector_host/dtc_read_only_server.hpp"
#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <set>
#include <stdexcept>
#include <utility>
#include <sys/socket.h>
#include <unistd.h>

namespace moex::connector_host::dtc {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxLocalCredentialBytes = 256;

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
void real64(Bytes& out, unsigned field, double n) {
    varint(out, field * 8 + 1);
    auto bits = std::bit_cast<std::uint64_t>(n);
    for (unsigned i = 0; i < 8; ++i)
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
unsigned witness_number(SessionReadyWitnessKind a) {
    using Kind = SessionReadyWitnessKind;
    switch (a) {
    case Kind::OnlineSynchronousEvent:
        return 1;
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
bool exact_dtc_quantity(std::int64_t quantity, float& wire_quantity) noexcept {
    if (quantity <= 0)
        return false;
    wire_quantity = static_cast<float>(quantity);
    constexpr double int64_upper_exclusive = 9223372036854775808.0; // 2^63, exactly representable as double.
    if (!std::isfinite(wire_quantity) || wire_quantity <= 0 ||
        static_cast<double>(wire_quantity) >= int64_upper_exclusive)
        return false;
    return static_cast<std::int64_t>(wire_quantity) == quantity;
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
    bool negotiated{}, logged_on{}, closing{}, definition{}, depth_subscribed{}, trade_subscribed{};
    bool definitions_capability_advertised{};
    std::uint32_t symbol_id{};
    std::uint32_t trade_symbol_id{};
    std::size_t depth_limit{};
    std::uint64_t batch{}, version{}, epoch{}, authority_epoch{}, commit_to_queue_ns{};
    std::uint64_t trade_stream_epoch{}, trade_cursor{}, trade_delivery_sequence{};
    std::vector<std::pair<std::size_t, std::size_t>> trade_output_ranges;
    std::string symbol, exchange, underlying_board, tick, error;
    std::int64_t isin{};
    unsigned witness{};
    std::optional<DtcSecurityDefinitionSnapshot> accepted_definition;
    DtcWireLogonCapabilities last_wire_logon_capabilities;
    DtcWireLogonCapabilities pending_wire_logon_capabilities;
    std::size_t pending_logon_response_end{};
    bool pending_logon_response{};
    std::size_t pending_definition_response_end{};
    bool pending_definition_response{};
    std::uint64_t completed_definition_responses{};
    Clock::time_point last_read{}, last_write{}, last_heartbeat{};
    std::chrono::seconds heartbeat{10};

    Impl(DtcMarketDataSource& s, DtcReadOnlyServerConfig c)
        : source(s), config(std::move(c)), decoder(config.max_frame_bytes) {
        if (config.max_frame_bytes < 16 || config.max_frame_bytes > UINT16_MAX || config.max_queued_bytes < 64 ||
            config.max_queued_bytes > 16 * 1024 * 1024 || config.max_write_bytes_per_poll == 0 ||
            config.max_write_bytes_per_poll > 4096 || config.max_depth_levels == 0 || config.max_depth_levels > 20000 ||
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
        negotiated = logged_on = closing = definition = depth_subscribed = trade_subscribed = false;
        definitions_capability_advertised = false;
        pending_wire_logon_capabilities = {};
        pending_logon_response_end = 0;
        pending_logon_response = false;
        pending_definition_response_end = 0;
        pending_definition_response = false;
        accepted_definition.reset();
        symbol_id = config.symbol_id;
        trade_symbol_id = 0;
        batch = version = epoch = authority_epoch = 0;
        trade_stream_epoch = trade_cursor = trade_delivery_sequence = 0;
        trade_output_ranges.clear();
    }
    bool queue(std::uint16_t type, const Bytes& body) {
        auto bytes = frame(type, body);
        if (bytes.size() > config.max_frame_bytes || output.size() > config.max_queued_bytes ||
            bytes.size() > config.max_queued_bytes - output.size()) {
            error = "DTC backpressure limit exceeded; fresh connection required";
            disconnect();
            return false;
        }
        if (output.empty())
            last_write = Clock::now();
        output.insert(output.end(), bytes.begin(), bytes.end());
        if (type == 507) {
            pending_definition_response_end = output.size();
            pending_definition_response = true;
        }
        return true;
    }
    bool remove_unsent_trade_frames(bool disconnect_if_partial) {
        for (std::size_t i = trade_output_ranges.size(); i > 0; --i) {
            const auto [begin, end] = trade_output_ranges[i - 1];
            if (sent >= end) {
                trade_output_ranges.erase(trade_output_ranges.begin() + static_cast<std::ptrdiff_t>(i - 1));
                continue;
            }
            if (sent > begin) {
                if (disconnect_if_partial) {
                    error = "public trade frame was partially written during source reset";
                    disconnect();
                    return false;
                }
                continue; // An unsubscribe lets this one in-flight frame finish.
            }
            const auto length = end - begin;
            output.erase(output.begin() + static_cast<std::ptrdiff_t>(begin),
                         output.begin() + static_cast<std::ptrdiff_t>(end));
            const auto adjust_end = [&](bool pending, std::size_t& marker) {
                if (pending && marker >= end)
                    marker -= length;
            };
            adjust_end(pending_logon_response, pending_logon_response_end);
            adjust_end(pending_definition_response, pending_definition_response_end);
            trade_output_ranges.erase(trade_output_ranges.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }
        return true;
    }
    void market_data_reject(std::uint32_t id, std::string_view reason) {
        Bytes p;
        integer(p, 1, id);
        str(p, 2, std::string(reason));
        queue(103, p);
    }
    void retire_public_deals(std::string_view reason, bool disconnect_if_partial = true) {
        const auto id = trade_symbol_id;
        if (!remove_unsent_trade_frames(disconnect_if_partial))
            return;
        trade_subscribed = false;
        trade_symbol_id = 0;
        trade_stream_epoch = trade_cursor = 0;
        if (!reason.empty() && client >= 0) {
            error = std::string(reason);
            market_data_reject(id ? id : symbol_id, reason);
        }
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
        trade_output_ranges.clear();
        trade_subscribed = false;
        pending_wire_logon_capabilities = {};
        pending_logon_response_end = 0;
        pending_logon_response = false;
        pending_definition_response_end = 0;
        pending_definition_response = false;
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
               s.book_snapshot_current && definition_validation(s).available();
    }
    std::string source_error(std::string_view detail) const {
        return "source_mode=" + std::string(dtc_source_mode_name(config.source_mode)) + ": " + std::string(detail);
    }
    DtcSecurityDefinitionValidation definition_validation(const DtcMarketDataSnapshot& s) const {
        if (!source.capabilities().security_definitions)
            return {.definition = std::nullopt, .reason = "source does not support DTC security definitions"};
        const DtcReplayDefinitionTerms replay_terms{.currency = config.currency,
                                                    .description = config.description,
                                                    .contract_size = config.contract_size,
                                                    .currency_value_per_increment =
                                                        config.currency_value_per_increment};
        return validate_dtc_security_definition(s, config.source_mode, replay_terms);
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
            std::string("source_mode=") + std::string(dtc_source_mode_name(config.source_mode)) +
                ";authority=" + std::string(session_ready_witness_kind_name(s.session_ready_witness_kind)) +
                ";transport_active=" + (s.transport_active ? "true" : "false") +
                ";snapshot_current=" + (usable(s) ? "true" : "false") +
                ";order_entry_allowed=false;accounts=false;positions=false;orders=false");
        queue(701, p);
    }
    bool authority(const DtcMarketDataSnapshot& s, std::uint64_t empty_batch = 0) {
        Bytes p;
        auto boolean = [](bool value) { return value ? "true" : "false"; };
        const auto name = session_ready_witness_kind_name(s.session_ready_witness_kind);
        auto json = std::string("moex.source_authority.v1 {\"symbol_id\":") + std::to_string(symbol_id) +
                    ",\"source_mode\":\"" + std::string(dtc_source_mode_name(config.source_mode)) +
                    "\",\"stream_epoch\":" + std::to_string(s.stream_epoch) +
                    ",\"aggr_online\":" + boolean(s.aggr_online) +
                    ",\"book_snapshot_current\":" + boolean(s.book_snapshot_current) +
                    ",\"session_ready_witness_kind\":\"" + plaza2::cgate::text::json_escape_utf8(name) +
                    "\",\"market_data_display_allowed\":" + boolean(s.market_data_display_allowed) +
                    ",\"order_entry_allowed\":false,\"exchange_confirmed\":false";
        if (s.full_order_log)
            json += std::string(",\"source_kind\":\"full_order_log\",\"book_state\":\"") +
                    (s.crossed_book                   ? "crossed"
                     : !s.market_data_display_allowed ? "unavailable"
                     : empty_batch || s.empty_book    ? "empty"
                                                      : "nonempty") +
                    "\"";
        if (empty_batch)
            json += ",\"depth_snapshot\":{\"empty\":true,\"dtc_batch_sequence\":" + std::to_string(empty_batch) +
                    ",\"source_snapshot_version\":" + std::to_string(s.source_snapshot_version) +
                    ",\"snapshot_watermark\":" + std::to_string(s.snapshot_watermark) +
                    ",\"level_count\":0,\"exchange_moment_ns\":" + std::to_string(s.exchange_moment_ns) + "}";
        str(p, 1, json + "}");
        integer(p, 2, 0);
        return queue(kDtcSourceAuthorityMessage, p);
    }
    void retire_depth_source(const DtcMarketDataSnapshot& s,
                             std::string_view reason = "source authority or metadata changed; fresh session required") {
        // Discard unsent stale batches. A partially written frame is closed.
        if (sent) {
            disconnect();
            return;
        }
        output.clear();
        trade_output_ranges.clear();
        pending_wire_logon_capabilities = {};
        pending_logon_response_end = 0;
        pending_logon_response = false;
        pending_definition_response_end = 0;
        pending_definition_response = false;
        if (!authority(s))
            return;
        Bytes unavailable;
        integer(unavailable, 1, symbol_id);
        integer(unavailable, 2, 1);
        if (!queue(116, unavailable))
            return;
        Bytes logoff;
        error = std::string(reason);
        str(logoff, 1, error);
        integer(logoff, 2, 0);
        if (queue(5, logoff))
            closing = true;
    }
    void snapshot(const DtcMarketDataSnapshot& s) {
        if (!usable(s)) {
            if (s.full_order_log)
                retire_depth_source(s);
            else
                reject("source authority unavailable; fresh session required");
            return;
        }
        if (s.symbol != symbol || s.underlying_board != underlying_board || s.min_step != tick || s.isin_id != isin ||
            !accepted_definition || definition_validation(s).definition != accepted_definition) {
            reject("source metadata changed; fresh definition required");
            return;
        }
        if ((!s.full_order_log && s.levels.empty()) || s.levels.size() > config.max_depth_levels * 2 ||
            !s.stream_epoch || !s.source_snapshot_version) {
            reject(source_error("invalid depth snapshot"));
            return;
        }
        auto rows = s.levels;
        std::set<std::uint64_t> ids;
        for (const auto& row : rows) {
            if ((row.side != DtcDepthSide::Bid && row.side != DtcDepthSide::Ask) || row.volume <= 0 ||
                (!s.full_order_log && (!row.source_row_id || !row.source_sequence || row.source_sequence > INT64_MAX ||
                                       !ids.insert(row.source_row_id).second))) {
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
            float wire_quantity{};
            if (!exact_dtc_quantity(r.volume, wire_quantity)) {
                reject("DTC depth quantity is not exactly representable as float32");
                return;
            }
            real(p, 3, wire_quantity);
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
            if (!s.full_order_log) {
                integer(p, 19, r.source_sequence);
                integer(p, 20, r.source_row_id);
            }
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
        if (!queue(116, available) || !authority(s, rows.empty() ? next_batch : 0))
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
    void publish_depth_commit() {
        commit_to_queue_ns = 0;
        if (client < 0 || !logged_on || closing || !depth_subscribed || !source.incremental_depth())
            return;
        const auto s = source.status_snapshot();
        if (!usable(s) || !accepted_definition || definition_validation(s).definition != accepted_definition ||
            s.market_data_authority_epoch != authority_epoch || s.stream_epoch != epoch) {
            retire_depth_source(s);
            return;
        }
        if (s.source_snapshot_version == version)
            return;
        if (version == UINT64_MAX || s.source_snapshot_version != version + 1) {
            retire_depth_source(s, "FullOrderLog commit was missed; fresh snapshot required");
            return;
        }
        const auto changes = source.depth_changes();
        const auto next_batch = batch + 1;
        if (!next_batch) {
            reject("DTC batch overflow");
            return;
        }
        Bytes staged;
        for (std::size_t i = 0; i < changes.size(); ++i) {
            const auto& level = changes[i];
            const float price = static_cast<float>(static_cast<double>(level.price_scaled) / 100000.0);
            float quantity{};
            if (!std::isfinite(price) || level.volume < 0 ||
                (level.volume && !exact_dtc_quantity(level.volume, quantity)) ||
                (level.side != DtcDepthSide::Bid && level.side != DtcDepthSide::Ask) || !level.depth_level ||
                level.depth_level > depth_limit) {
                reject("FullOrderLog incremental price or quantity cannot be represented by DTC");
                return;
            }
            Bytes p;
            integer(p, 1, symbol_id);
            integer(p, 2, level.exchange_moment_ns / 1000000);
            real(p, 3, price);
            real(p, 4, quantity);
            integer(p, 5, static_cast<unsigned>(level.side));
            integer(p, 6, level.volume ? 1 : 2); // replace/delete the committed position
            integer(p, 9, level.depth_level);
            integer(p, 8, i + 1 == changes.size() ? 1 : i == 0 ? 3 : 2);
            integer(p, 13, s.stream_epoch);
            integer(p, 14, next_batch);
            integer(p, 15, s.source_snapshot_version);
            auto f = frame(140, p);
            if (f.size() > config.max_frame_bytes || f.size() > config.max_queued_bytes - staged.size()) {
                reject("FullOrderLog commit exceeds DTC queue bounds");
                return;
            }
            staged.insert(staged.end(), f.begin(), f.end());
        }
        if (staged.size() > config.max_queued_bytes - output.size()) {
            retire_depth_source(s, "FullOrderLog commit backpressure; fresh snapshot required");
            return;
        }
        if (!staged.empty()) {
            if (output.empty())
                last_write = Clock::now();
            output.insert(output.end(), staged.begin(), staged.end());
            if (s.committed_at != Clock::time_point{})
                commit_to_queue_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - s.committed_at).count();
        }
        if (!staged.empty())
            batch = next_batch;
        version = s.source_snapshot_version;
    }
    void poll_public_deals() {
        if (!source.capabilities().market_data) {
            retire_public_deals("public trade publisher became unavailable");
            return;
        }

        plaza2::cgate::Plaza2PublicDealsSnapshot deals;
        try {
            deals = source.public_deals(trade_cursor);
        } catch (...) {
            retire_public_deals("public trade source failed; redefine the subscription");
            return;
        }
        if (!deals.online || !deals.valid || !deals.stream_epoch || !deals.lifenum) {
            retire_public_deals("public trade source unavailable; redefine the subscription");
            return;
        }
        if (deals.stream_epoch != trade_stream_epoch) {
            retire_public_deals("public trade stream epoch changed; redefine the subscription");
            return;
        }
        if ((deals.first_sequence && deals.first_sequence > deals.last_sequence) ||
            deals.last_sequence < trade_cursor) {
            retire_public_deals("public trade sequence state is inconsistent; redefine the subscription");
            return;
        }
        if (trade_cursor != UINT64_MAX && deals.first_sequence > trade_cursor + 1) {
            retire_public_deals("public trade ring overflow; events were lost; redefine the subscription");
            return;
        }

        DtcMarketDataSnapshot current;
        try {
            current = source.snapshot();
        } catch (...) {
            retire_public_deals("current target metadata unavailable; redefine the subscription");
            return;
        }
        const auto current_definition = definition_validation(current);
        if (!definition || !accepted_definition || !current_definition.definition ||
            current_definition.definition != accepted_definition || current.symbol != symbol ||
            current.underlying_board != underlying_board || current.min_step != tick || current.isin_id != isin ||
            current.session_id != accepted_definition->session_id ||
            current.isin_id != accepted_definition->security_identifier) {
            retire_public_deals("current target, session, or definition changed; redefine the subscription");
            return;
        }

        std::uint64_t next_capture_sequence = trade_cursor;
        std::uint64_t next_delivery_sequence = trade_delivery_sequence;
        std::vector<Bytes> staged;
        std::size_t staged_bytes = 0;
        std::uint64_t prior_sequence = 0;
        const auto bad_stream = [&](std::string_view reason) { retire_public_deals(reason); };
        for (const auto& deal : deals.trades) {
            if (!deal.sequence || deal.sequence <= prior_sequence || deal.stream_epoch != deals.stream_epoch ||
                (deals.first_sequence && deal.sequence < deals.first_sequence) ||
                (deals.last_sequence && deal.sequence > deals.last_sequence)) {
                bad_stream("public trade ring contains invalid event identity; redefine the subscription");
                return;
            }
            prior_sequence = deal.sequence;
            if (deal.sequence <= trade_cursor)
                continue; // Compatibility with small test sources that ignore after_sequence.
            if (next_capture_sequence == UINT64_MAX || deal.sequence != next_capture_sequence + 1) {
                bad_stream("public trade ring has a sequence gap; events were lost; redefine the subscription");
                return;
            }
            constexpr std::int64_t kMaxExactlyRepresentableDoubleInteger = INT64_C(9007199254740992);
            if (deal.deal_id <= 0 || !deal.repl_id || deal.repl_rev <= 0 || !deal.lifenum ||
                deal.lifenum != deals.lifenum || deal.session_id != accepted_definition->session_id ||
                deal.isin_id != accepted_definition->security_identifier || deal.isin_id != current.isin_id ||
                deal.at_bid_or_ask < 0 || deal.at_bid_or_ask > 2 || deal.quantity <= 0 ||
                deal.quantity > kMaxExactlyRepresentableDoubleInteger || deal.moment_ns == 0 ||
                deal.moment_ns > static_cast<std::uint64_t>(INT64_MAX) || deal.price.size() > 64 ||
                deal.received_at_unix_ns == 0 || deal.received_at_unix_ns > static_cast<std::uint64_t>(INT64_MAX)) {
                bad_stream("public trade contains invalid price, quantity, side, or source clock");
                return;
            }
            const auto parsed_price = plaza2::private_state::parse_session_decimal(deal.price);
            if (!parsed_price || parsed_price->units <= 0 || parsed_price->units != deal.price_scaled) {
                bad_stream("public trade price text does not match its scaled price");
                return;
            }
            if (next_delivery_sequence == UINT64_MAX) {
                bad_stream("DTC public trade delivery sequence overflow");
                return;
            }

            Bytes payload;
            integer(payload, 1, trade_symbol_id);
            integer(payload, 2, static_cast<std::uint32_t>(deal.at_bid_or_ask));
            real64(payload, 3,
                   static_cast<double>(parsed_price->units) /
                       static_cast<double>(plaza2::private_state::SessionDecimal::scale));
            real64(payload, 4, static_cast<double>(deal.quantity));
            real64(payload, 5, static_cast<double>(deal.moment_ns) / 1000000000.0);
            integer(payload, 6, deal.stream_epoch);
            integer(payload, 7, ++next_delivery_sequence);
            str(payload, 8, std::to_string(deal.deal_id));
            integer(payload, 9, deal.sequence);
            integer(payload, 10, deal.moment_ns); // Signed int64, range checked above.
            integer(payload, 11, 0);              // Engine ingress clock is not supplied by the native projector.
            integer(payload, 12, 0);              // Engine emit clock is not supplied by the native projector.
            integer(payload, 13, deal.repl_id);
            integer(payload, 14, static_cast<std::uint64_t>(deal.repl_rev));
            integer(payload, 15, deal.lifenum);
            integer(payload, 16, static_cast<std::uint64_t>(static_cast<std::int64_t>(deal.session_id)));
            integer(payload, 17, static_cast<std::uint64_t>(deal.isin_id));
            integer(payload, 18, deal.received_at_unix_ns);
            integer(payload, 19, deal.moment); // Decoded MOEX calendar-seconds counter, not literal P2TIME bytes.
            integer(payload, 20, deal.moment_ns);
            integer(payload, 21, deal.xstatus_buy);
            integer(payload, 22, deal.xstatus_sell);
            auto encoded = frame(static_cast<std::uint16_t>(DtcMessageType::MarketDataUpdateTrade), payload);
            if (encoded.size() > config.max_frame_bytes || encoded.size() > config.max_queued_bytes ||
                staged_bytes > config.max_queued_bytes - encoded.size()) {
                bad_stream("public trade batch exceeds DTC queue bounds; redefine the subscription");
                return;
            }
            staged_bytes += encoded.size();
            staged.push_back(std::move(encoded));
            next_capture_sequence = deal.sequence;
        }
        if (next_capture_sequence != deals.last_sequence) {
            bad_stream("public trade ring omitted committed events; events were lost; redefine the subscription");
            return;
        }
        if (staged.empty())
            return;
        if (output.size() > config.max_queued_bytes || staged_bytes > config.max_queued_bytes - output.size()) {
            retire_public_deals("public trade output queue overflow; events were lost; redefine the subscription");
            return;
        }
        if (output.empty())
            last_write = Clock::now();
        for (auto& encoded : staged) {
            const auto begin = output.size();
            output.insert(output.end(), encoded.begin(), encoded.end());
            trade_output_ranges.emplace_back(begin, output.size());
        }
        trade_cursor = next_capture_sequence;
        trade_delivery_sequence = next_delivery_sequence;
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
            for (auto field : {7U, 8U, 9U, 10U, 13U, 17U, 19U})
                integer(reply, field, 0);
            // LOGON advertises implemented application capability, not
            // current metadata readiness. Each 506 is gated by fresh source
            // validation so a cold connection can become usable later.
            definitions_capability_advertised = source.capabilities().security_definitions;
            integer(reply, 12, definitions_capability_advertised);
            integer(reply, 14, 1);
            integer(reply, 15, source.capabilities().market_depth);
            integer(reply, 20, source.capabilities().market_data);
            if (queue(2, reply)) {
                logged_on = true;
                pending_wire_logon_capabilities = {
                    .response_fully_written_to_socket = false,
                    .market_depth_updates_best_bid_and_ask = 0,
                    .trading_is_supported = 0,
                    .oco_orders_supported = 0,
                    .order_cancel_replace_supported = 0,
                    .security_definitions_supported = definitions_capability_advertised ? 1U : 0U,
                    .historical_price_data_supported = 0,
                    .resubscribe_when_market_data_feed_available = 1,
                    .market_depth_is_supported = source.capabilities().market_depth ? 1U : 0U,
                    .one_historical_price_data_request_per_connection = 0,
                    .bracket_orders_supported = 0,
                    .multiple_positions_per_symbol_and_trade_account = 0,
                    .market_data_supported = source.capabilities().market_data ? 1U : 0U};
                pending_logon_response_end = output.size();
                pending_logon_response = true;
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
            const auto validated = definition_validation(s);
            Bytes reply;
            integer(reply, 1, p.number(1));
            if (!validated.definition || !definitions_capability_advertised ||
                p.text(2) != validated.definition->symbol || p.text(3) != validated.definition->exchange) {
                str(reply, 2, "unknown instrument or metadata unavailable");
                queue(509, reply);
                return;
            }
            const auto& resolved = *validated.definition;
            str(reply, 2, resolved.symbol);
            str(reply, 3, resolved.exchange);
            integer(reply, 4, static_cast<std::uint32_t>(resolved.security_type));
            str(reply, 5, resolved.description);
            real(reply, 6, resolved.min_price_increment);
            real(reply, 8, resolved.currency_value_per_increment);
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
            str(reply, 28, resolved.currency);
            real(reply, 29, resolved.contract_size);
            integer(reply, 33, static_cast<std::uint64_t>(resolved.security_identifier));
            // Coordinated Connector/Kairos extension, not an official DTC
            // field. A trade-capable client binds every 107 to this session.
            // Preserve the existing depth-only 507 wire contract unchanged.
            if (source.capabilities().market_data)
                integer(reply, 35, static_cast<std::uint64_t>(resolved.session_id));
            if (queue(507, reply)) {
                definition = true;
                accepted_definition = resolved;
                symbol = resolved.symbol;
                exchange = resolved.exchange;
                underlying_board = resolved.underlying_board;
                tick = s.min_step;
                isin = resolved.security_identifier;
            }
            return;
        }
        if (f.message_type == 101) {
            const auto id = p.number(2), action = p.number(1);
            if (action == 2) {
                if (!id || id > UINT32_MAX || (trade_subscribed && id != trade_symbol_id) ||
                    (config.symbol_id && id != config.symbol_id)) {
                    market_data_reject(id <= UINT32_MAX ? static_cast<std::uint32_t>(id) : 0,
                                       "invalid public trade unsubscribe");
                    return;
                }
                if (trade_subscribed)
                    retire_public_deals({}, false);
                return;
            }
            if (action != 1 || !id || id > UINT32_MAX || (config.symbol_id && id != config.symbol_id) ||
                (trade_subscribed && id != trade_symbol_id)) {
                market_data_reject(id <= UINT32_MAX ? static_cast<std::uint32_t>(id) : 0,
                                   "invalid or unsupported public trade subscription");
                return;
            }
            const auto current = source.snapshot();
            const auto validated = definition_validation(current);
            if (!definition || !accepted_definition || !validated.definition ||
                validated.definition != accepted_definition || current.symbol != symbol ||
                current.underlying_board != underlying_board || current.min_step != tick || current.isin_id != isin ||
                p.text(3) != symbol || p.text(4) != exchange) {
                market_data_reject(static_cast<std::uint32_t>(id), "target or current security definition mismatch");
                return;
            }
            if (!source.capabilities().market_data) {
                market_data_reject(static_cast<std::uint32_t>(id), "source does not publish public trades");
                return;
            }
            // An identical duplicate SUBSCRIBE is idempotent. In particular,
            // it must not move the forward-only cursor past unseen events.
            if (trade_subscribed)
                return;
            plaza2::cgate::Plaza2PublicDealsSnapshot deals;
            try {
                deals = source.public_deals();
            } catch (...) {
                market_data_reject(static_cast<std::uint32_t>(id), "public trade source unavailable");
                return;
            }
            if (!deals.online || !deals.valid || !deals.stream_epoch ||
                (deals.first_sequence && deals.last_sequence && deals.first_sequence > deals.last_sequence)) {
                market_data_reject(static_cast<std::uint32_t>(id),
                                   "public trade source unavailable; retry subscription");
                return;
            }
            // Subscribe from the observed tail. Retained rows are capture
            // history and are never synthesized into DTC trade updates.
            trade_symbol_id = static_cast<std::uint32_t>(id);
            trade_stream_epoch = deals.stream_epoch;
            trade_cursor = deals.last_sequence;
            trade_subscribed = true;
            return;
        }
        if (f.message_type == 102) {
            const auto id = p.number(2), action = p.number(1), levels = p.number(5);
            if (action == 2 && id == symbol_id) {
                depth_subscribed = false;
                return;
            }
            auto s = source.snapshot();
            if (!definition || !source.capabilities().market_depth || !id || id > UINT32_MAX ||
                (action != 1 && action != 3) || p.text(3) != symbol || p.text(4) != exchange ||
                levels > config.max_depth_levels || (config.symbol_id && id != config.symbol_id) ||
                (depth_subscribed && id != symbol_id)) {
                Bytes reply;
                integer(reply, 1, id);
                str(reply, 2, "invalid or unsupported depth subscription");
                queue(121, reply);
                return;
            }
            symbol_id = static_cast<std::uint32_t>(id);
            depth_limit = levels ? static_cast<std::size_t>(levels) : config.max_depth_levels;
            depth_subscribed = action == 1;
            source.configure_depth_limit(depth_limit);
            snapshot(source.snapshot());
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
            if (logged_on && !closing && depth_subscribed) {
                auto s = source.status_snapshot();
                if (!usable(s) || s.symbol != symbol || s.underlying_board != underlying_board || s.min_step != tick ||
                    s.isin_id != isin || !accepted_definition ||
                    definition_validation(s).definition != accepted_definition ||
                    s.market_data_authority_epoch != authority_epoch ||
                    witness_number(s.session_ready_witness_kind) != witness) {
                    retire_depth_source(s);
                } else if (s.source_snapshot_version != version || s.stream_epoch != epoch) {
                    if (source.incremental_depth())
                        publish_depth_commit();
                    else
                        snapshot(s);
                }
            }
            if (client >= 0 && logged_on && !closing && trade_subscribed)
                poll_public_deals();
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
            const auto n = ::send(client, output.data() + sent,
                                  std::min(config.max_write_bytes_per_poll, output.size() - sent), flags);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                last_write = now;
                trade_output_ranges.erase(std::remove_if(trade_output_ranges.begin(), trade_output_ranges.end(),
                                                         [&](const auto& range) { return sent >= range.second; }),
                                          trade_output_ranges.end());
                if (pending_logon_response && sent >= pending_logon_response_end) {
                    pending_wire_logon_capabilities.response_fully_written_to_socket = true;
                    last_wire_logon_capabilities = pending_wire_logon_capabilities;
                    pending_wire_logon_capabilities = {};
                    pending_logon_response_end = 0;
                    pending_logon_response = false;
                }
                if (pending_definition_response && sent >= pending_definition_response_end) {
                    ++completed_definition_responses;
                    pending_definition_response_end = 0;
                    pending_definition_response = false;
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                disconnect();
                return;
            }
            if (sent == output.size()) {
                output.clear();
                sent = 0;
                trade_output_ranges.clear();
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
void DtcReadOnlyServer::publish_depth_commit() {
    try {
        impl_->publish_depth_commit();
    } catch (...) {
        impl_->error = "DTC committed source failure";
        impl_->disconnect();
    }
}
std::uint64_t DtcReadOnlyServer::last_commit_to_queue_ns() const noexcept {
    return impl_->commit_to_queue_ns;
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
bool DtcReadOnlyServer::has_public_deals_subscription() const noexcept {
    return impl_->trade_subscribed;
}
std::size_t DtcReadOnlyServer::queued_bytes() const noexcept {
    return impl_->output.size() - impl_->sent;
}
const std::string& DtcReadOnlyServer::last_error() const noexcept {
    return impl_->error;
}
const DtcWireLogonCapabilities& DtcReadOnlyServer::last_wire_logon_capabilities() const noexcept {
    return impl_->last_wire_logon_capabilities;
}
std::uint64_t DtcReadOnlyServer::security_definition_responses_fully_written() const noexcept {
    return impl_->completed_definition_responses;
}
} // namespace moex::connector_host::dtc
