#include "moex/connector_host/connector_host.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"
#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>

namespace moex::connector_host {
namespace {
namespace cg = plaza2::cgate;
namespace ps = plaza2::private_state;
using plaza2::generated::StreamCode;
using namespace plaza2_trade;
bool online_stream(const ps::Plaza2PrivateStateProjector& data, const Plaza2TransportHealth& health, StreamCode code) {
    if (!health.valid || health.private_count > health.private_streams.size())
        return false;
    for (std::size_t i = 0; i < health.private_count; ++i) {
        if (health.private_streams[i] != code)
            continue;
        if (health.private_states[i] != 3)
            return false;
        for (const auto& row : data.stream_health())
            if (row.stream_code == code)
                return row.online && row.snapshot_complete;
    }
    return false;
}
bool valid_current_min_step(std::string_view value) {
    const auto parsed = ps::parse_session_decimal(value);
    return parsed && parsed->units > 0;
}
bool supported_live_currency_economics(std::string_view currency) noexcept {
    return currency == "RUB";
}
std::string quoted(std::string_view value) {
    return "\"" + cg::text::json_escape_utf8(value) + "\"";
}
} // namespace

std::int32_t current_session_id(const ps::Plaza2PrivateStateProjector& data, std::int64_t now_seconds) {
    return data.current_session_id(now_seconds);
}

bool order_entry_ready(const CgateSession& host, std::int64_t isin_id, std::int32_t session_id,
                       bool allow_opening_auction) {
    const auto health = host.runtime_health();
    if (!host.started() || !health.valid || health.connection != 3 || health.publisher != 3 || health.reply != 3 ||
        !host.trade_replay_anchor_ready())
        return false;
    const auto& data = host.private_state();
    constexpr std::array required{StreamCode::kFortsTradeRepl,        StreamCode::kFortsPosRepl,
                                  StreamCode::kFortsPartRepl,         StreamCode::kFortsRefdataRepl,
                                  StreamCode::kFortsSessionstateRepl, StreamCode::kFortsInstrumentstateRepl};
    for (const auto stream : required)
        if (!online_stream(data, health, stream))
            return false;
    if (session_id == 0)
        session_id = current_session_id(data);
    const auto session = std::find_if(data.sessions().begin(), data.sessions().end(), [session_id](const auto& row) {
        return row.sess_id == session_id && row.has_current_status && row.current_status == 1;
    });
    if (session == data.sessions().end())
        return false;
    const auto instrument = std::find_if(data.instruments().begin(), data.instruments().end(), [=](const auto& row) {
        return row.kind == ps::InstrumentKind::kFuture && !row.is_spread && row.isin_id == isin_id &&
               row.sess_id == session_id && row.current_session_member && row.has_current_status &&
               (row.current_status == 1 || (allow_opening_auction && row.current_status == 6));
    });
    return instrument != data.instruments().end();
}

std::string_view host_state_name(ConnectorHostState state) noexcept {
    switch (state) {
    case ConnectorHostState::Created:
        return "Created";
    case ConnectorHostState::Started:
        return "Started";
    case ConnectorHostState::Ready:
        return "Ready";
    case ConnectorHostState::Stopping:
        return "Stopping";
    case ConnectorHostState::Stopped:
        return "Stopped";
    case ConnectorHostState::Recovering:
        return "Recovering";
    default:
        return "Failed";
    }
}
struct ConnectorHost::Impl {
    explicit Impl(Plaza2HostConfig c) : config(std::move(c)), host(config.transport.host) {}
    Plaza2HostConfig config;
    CgateSession host;
    ConnectorHostState state{ConnectorHostState::Created};
    std::string error;
};
ConnectorHost::ConnectorHost(Plaza2HostConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
ConnectorHost::~ConnectorHost() {
    (void)stop();
}
cg::Plaza2Error ConnectorHost::start() {
    if (impl_->config.read_only_market_data != impl_->config.transport.host.read_only_market_data)
        return {.code = cg::Plaza2ErrorCode::InvalidConfiguration, .message = "read-only mode mismatch"};
    const auto error = impl_->host.start();
    impl_->state = error ? ConnectorHostState::Failed : ConnectorHostState::Started;
    impl_->error = error.message;
    return error;
}
cg::Plaza2Error ConnectorHost::poll() {
    const auto error = impl_->host.poll();
    if (error) {
        impl_->state = ConnectorHostState::Failed;
        impl_->error = error.message;
    } else if (impl_->host.recovering())
        impl_->state = ConnectorHostState::Recovering;
    else
        impl_->state = impl_->host.aggr_online() ? ConnectorHostState::Ready : ConnectorHostState::Started;
    return error;
}
cg::Plaza2Error ConnectorHost::stop() {
    const auto error = impl_->host.stop();
    impl_->state = error ? ConnectorHostState::Failed : ConnectorHostState::Stopped;
    return error;
}
bool ConnectorHost::has_publisher_or_reply_handles() const noexcept {
    return impl_->host.publisher_open() || impl_->host.p2mqreply_open();
}
bool ConnectorHost::public_deals_enabled() const noexcept {
    return !impl_->config.transport.host.public_deals_stream.settings.empty();
}
cg::Plaza2PublicDealsSnapshot ConnectorHost::public_deals_snapshot(std::uint64_t after) const {
    return impl_->host.public_deals_snapshot(after);
}
bool ConnectorHost::order_entry_ready(std::int64_t isin_id) const {
    return !impl_->config.read_only_market_data && moex::connector_host::order_entry_ready(impl_->host, isin_id);
}
ConnectorHostMarketDataSnapshot ConnectorHost::market_data_snapshot() const {
    return market_data_snapshot(impl_->config.transport.target_isin_id);
}
ConnectorHostSnapshot ConnectorHost::snapshot() const {
    const auto& host = impl_->host;
    ConnectorHostSnapshot out;
    out.state = impl_->state;
    out.recovery = host.recovery_status();
    out.environment = impl_->config.transport.host.runtime.environment;
    out.mode = host.mode();
    out.target_isin_id = impl_->config.transport.target_isin_id;
    out.session_id = current_session_id(host.private_state());
    out.runtime_compatibility = cg::plaza2_compatibility_name(host.probe_report().compatibility);
    out.runtime_scheme_sha256 = host.probe_report().runtime_scheme_sha256;
    out.connection_app_name = host.connection_app_name();
    out.transport_health = host.runtime_health();
    out.publisher_handle_open = host.publisher_open();
    out.reply_handle_open = host.p2mqreply_open();
    out.publisher_ready = out.transport_health.connection == 3 && out.transport_health.publisher == 3;
    out.reply_ready = out.transport_health.connection == 3 && out.transport_health.reply == 3;
    const auto& data = host.private_state();
    out.streams.assign(data.stream_health().begin(), data.stream_health().end());
    out.private_snapshot_state_ready =
        !out.streams.empty() && std::all_of(out.streams.begin(), out.streams.end(),
                                            [](const auto& row) { return row.online && row.snapshot_complete; });
    out.private_streams_ready = out.transport_health.private_active && out.private_snapshot_state_ready;
    out.aggr_snapshot_state_ready = host.aggr_snapshot_complete();
    const auto md = market_data_snapshot();
    out.aggr_ready = md.valid;
    out.target = md.symbol;
    out.min_step = md.min_step;
    for (const auto& level : md.levels) {
        if (level.side == 1 && out.bid.empty())
            out.bid = level.price;
        if (level.side == 2 && out.ask.empty())
            out.ask = level.price;
    }
    out.new_order_allowed = order_entry_ready(out.target_isin_id);
    out.observation_ready = impl_->config.read_only_market_data ? md.valid : out.new_order_allowed;
    out.publisher_calls = host.publisher_call_counts();
    out.last_error = host.last_callback_error().empty() ? impl_->error : host.last_callback_error();
    return out;
}
ConnectorHostMarketDataSnapshot ConnectorHost::market_data_snapshot(std::int64_t isin_id) const {
    ConnectorHostMarketDataSnapshot out;
    const auto& host = impl_->host;
    const auto& config = impl_->config;
    const auto& health = host.runtime_health();
    const auto status = host.aggr_status();
    out.connector_generation = host.recovery_status().generation;
    out.market_data_authority_epoch = status.stream_epoch;
    out.stream_epoch = status.stream_epoch;
    out.target_isin_id = isin_id;
    const auto now_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            (config.market_data_now ? config.market_data_now() : std::chrono::system_clock::now()).time_since_epoch())
            .count();
    out.target_session_id = current_session_id(host.private_state(), now_seconds);
    out.underlying_board = config.target_underlying_board;
    // These are explicit operator bindings until an unambiguous committed
    // fut_vcb join supplies the source values below.
    out.currency = config.target_currency;
    out.transport_active = host.started() && health.aggr == 3 && status.transport_active;
    out.snapshot_complete = status.snapshot_complete;
    const bool matching_ready =
        status.ready_event && status.ready_event->sess_id == out.target_session_id && status.session_data_ready;
    out.session_data_ready = matching_ready;
    out.aggr_online = status.aggr_online;

    bool target_instrument_refdata_current = false;
    bool operator_binding_conflict = false;
    for (const auto& instrument : host.private_state().instruments()) {
        if (instrument.isin_id != out.target_isin_id)
            continue;
        out.symbol = instrument.isin;
        out.min_step = instrument.min_step;
        out.description = instrument.name;
        out.target_is_future = instrument.kind == ps::InstrumentKind::kFuture;
        out.target_is_spread = instrument.is_spread;
        out.target_is_multileg = (instrument.signs & 0x100) != 0 || instrument.trade_mode_id == 14;
        out.future_vcb_base_contract_code = instrument.base_contract_code;
        out.future_vcb_base_contract_id = instrument.base_contract_id;
        out.definition_source_provenance = instrument.definition_source_provenance;
        out.future_instruments_provenance =
            host.private_state()
                .instrument_source_provenance(plaza2::generated::TableCode::kFortsRefdataReplFutInstruments,
                                              out.target_isin_id)
                .value_or(ps::SourceRowProvenance{});
        out.future_sess_contents_provenance =
            host.private_state()
                .instrument_source_provenance(plaza2::generated::TableCode::kFortsRefdataReplFutSessContents,
                                              out.target_isin_id)
                .value_or(ps::SourceRowProvenance{});
        if (instrument.lot_volume > 0)
            out.contract_size = std::to_string(instrument.lot_volume);
        out.currency_value_per_increment = instrument.step_price_curr;
        out.refdata_vcb_join_current = instrument.future_vcb_join_status == ps::FutureVcbJoinStatus::Resolved;
        out.refdata_vcb_join_ambiguous = instrument.future_vcb_join_status == ps::FutureVcbJoinStatus::Ambiguous;
        if (out.refdata_vcb_join_current) {
            out.underlying_board = instrument.future_vcb_board_md;
            out.currency = instrument.future_vcb_currency;
            out.future_vcb_provenance = instrument.future_vcb_provenance;
            if (!config.target_underlying_board.empty() && config.target_underlying_board != out.underlying_board)
                operator_binding_conflict = true;
            if (!config.target_currency.empty() && config.target_currency != out.currency)
                operator_binding_conflict = true;
            out.refdata_board_proven = !out.underlying_board.empty() && out.underlying_board.size() <= 128 &&
                                       cg::text::valid_utf8(out.underlying_board);
            out.refdata_currency_proven = !out.currency.empty() && out.currency.size() <= 16 &&
                                          cg::text::valid_utf8(out.currency) &&
                                          supported_live_currency_economics(out.currency);
        }
        target_instrument_refdata_current =
            instrument.kind == ps::InstrumentKind::kFuture && instrument.current_session_member &&
            instrument.sess_id == out.target_session_id && instrument.trade_mode_id != 0 && !instrument.isin.empty() &&
            valid_current_min_step(instrument.min_step);
        break;
    }

    std::optional<std::int32_t> session_status;
    std::optional<std::int32_t> instrument_status;
    for (const auto& instrument : host.private_state().instruments()) {
        if (instrument.isin_id == out.target_isin_id && instrument.has_current_status)
            instrument_status = instrument.current_status;
    }
    for (const auto& session : host.private_state().sessions()) {
        if (session.sess_id == out.target_session_id && session.has_current_status)
            session_status = session.current_status;
    }

    const auto scoped = host.aggr20_projector().snapshot_for_isin(out.target_isin_id);
    if (scoped.has_value()) {
        out.source_snapshot_version = scoped->source_snapshot_version;
        out.source_snapshot_hash = 0; // Reserved legacy DTC field.
        out.source_repl_id = scoped->last_repl_id;
        out.source_repl_rev = scoped->last_repl_rev;
        out.snapshot_watermark = scoped->last_repl_rev > 0 ? static_cast<std::uint64_t>(scoped->last_repl_rev) : 0;
        out.exchange_moment = scoped->exchange_moment;
        out.exchange_moment_ns = scoped->exchange_moment_ns;
        out.committed_at = scoped->committed_at;
        out.two_sided = scoped->top_bid.has_value() && scoped->top_ask.has_value();
        out.levels.reserve(scoped->levels.size());
        for (const auto& level : scoped->levels) {
            out.levels.push_back({.price_scaled = level.price_scaled,
                                  .volume = level.volume,
                                  .side = level.dir,
                                  .source_repl_id = level.repl_id,
                                  .source_repl_rev = level.repl_rev,
                                  .exchange_moment = level.moment,
                                  .exchange_moment_ns = level.moment_ns,
                                  .price = level.price});
        }
    }

    const auto& data = host.private_state();
    out.session_provenance =
        data.session_source_provenance(plaza2::generated::TableCode::kFortsRefdataReplSession, out.target_session_id)
            .value_or(ps::SourceRowProvenance{});
    const bool healthy =
        host.started() && impl_->state != ConnectorHostState::Failed && health.valid && health.connection == 3;
    const bool identity_current = out.target_session_id > 0 && target_instrument_refdata_current &&
                                  online_stream(data, health, StreamCode::kFortsRefdataRepl) &&
                                  online_stream(data, health, StreamCode::kFortsSessionstateRepl) &&
                                  online_stream(data, health, StreamCode::kFortsInstrumentstateRepl);
    out.refdata_metadata_current =
        !out.underlying_board.empty() && target_instrument_refdata_current && !operator_binding_conflict;
    out.session_tradable = identity_current && session_status == std::optional<std::int32_t>{1};
    out.instrument_tradable = identity_current && instrument_status == std::optional<std::int32_t>{1};
    out.source_consistent = healthy && identity_current && out.transport_active && out.snapshot_complete &&
                            out.aggr_online && status.valid && scoped.has_value();
    out.target_authoritative = out.source_consistent && out.refdata_metadata_current;
    out.book_snapshot_current = out.source_consistent;
    out.market_data_display_allowed = out.target_authoritative;
    out.market_data_live = out.source_consistent;
    out.valid = out.market_data_display_allowed;
    if (out.source_consistent && status.ready_event)
        out.session_ready_event = status.ready_event;
    // DTC order entry is outside this certificate and remains disabled.
    out.order_entry_allowed = false;
    if (out.valid)
        out.invalid_reason.clear();
    else if (!healthy)
        out.invalid_reason = "market-data callback/transport/recovery health is not current";
    else if (!out.transport_active)
        out.invalid_reason = "AGGR20 transport is not active";
    else if (!out.snapshot_complete)
        out.invalid_reason = "AGGR20 snapshot is incomplete";
    else if (!identity_current)
        out.invalid_reason = "current session identity/status/refdata corroboration is missing or expired";
    else if (!scoped.has_value())
        out.invalid_reason = "target AGGR20 snapshot is absent";
    else if (!status.valid)
        out.invalid_reason = "AGGR20 target source is not authoritative";
    else if (!out.refdata_metadata_current)
        out.invalid_reason = "target refdata symbol/board/min_step is not current";
    return out;
}

std::string render_snapshot(const ConnectorHostSnapshot& s, bool json) {
    std::ostringstream out;
    out << std::boolalpha;
    if (!json) {
        out << "state=" << host_state_name(s.state) << " target=" << s.target << " session=" << s.session_id
            << " order_entry_ready=" << s.new_order_allowed << " market_data_valid=" << s.aggr_ready
            << " last_error=" << s.last_error << '\n';
    } else {
        out << "{\"state\":" << quoted(host_state_name(s.state)) << ",\"target\":" << quoted(s.target)
            << ",\"session_id\":" << s.session_id << ",\"order_entry_ready\":" << s.new_order_allowed
            << ",\"market_data_valid\":" << s.aggr_ready << ",\"last_error\":" << quoted(s.last_error) << "}\n";
    }
    return out.str();
}
} // namespace moex::connector_host
