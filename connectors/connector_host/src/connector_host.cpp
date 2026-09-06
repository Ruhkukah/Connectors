#include "moex/connector_host/connector_host.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace moex::connector_host {
namespace {
namespace cg = plaza2::cgate;
namespace ps = plaza2::private_state;
using namespace plaza2_trade;
using plaza2::generated::StreamCode;

cg::Plaza2Error invalid(std::string message) {
    return {.code = cg::Plaza2ErrorCode::InvalidConfiguration, .message = std::move(message)};
}

std::string quoted(std::string_view value) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += static_cast<char>(ch);
        } else if (ch < 32) {
            result += "\\u00";
            result += hex[ch >> 4];
            result += hex[ch & 15];
        } else
            result += static_cast<char>(ch);
    }
    return result + '"';
}
} // namespace

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
    case ConnectorHostState::Failed:
        return "Failed";
    }
    return "Failed";
}

struct ConnectorHost::Impl {
    explicit Impl(Plaza2HostConfig value) : config(std::move(value)), transport(config.transport) {}
    Plaza2HostConfig config;
    Plaza2TestTradeTransport transport;
    ConnectorHostState state{ConnectorHostState::Created};
    std::string error;
    std::string authorized_sha;
    bool submitted{false};
    std::optional<OrderLifecycleResult> result;

    ConnectorHostSnapshot snapshot() const {
        ConnectorHostSnapshot out;
        const auto& host = transport.host();
        const auto& data = host.private_state();
        out.state = state;
        out.environment = config.transport.host.runtime.environment;
        out.mode = config.transport.host.mode;
        out.target_isin_id = config.transport.target_isin_id;
        out.session_id = config.transport.target_session_id;
        out.runtime_compatibility = cg::plaza2_compatibility_name(host.probe_report().compatibility);
        out.runtime_scheme_sha256 = host.probe_report().scheme_drift.runtime_scheme_sha256;
        out.publisher_ready = host.publisher_open();
        out.reply_ready = host.p2mqreply_open();
        out.publisher_calls = host.publisher_call_counts();
        out.streams.assign(data.stream_health().begin(), data.stream_health().end());
        constexpr std::array required{StreamCode::kFortsTradeRepl,
                                      StreamCode::kFortsUserorderbookRepl,
                                      StreamCode::kFortsPosRepl,
                                      StreamCode::kFortsPartRepl,
                                      StreamCode::kFortsRefdataRepl,
                                      StreamCode::kFortsSessionstateRepl,
                                      StreamCode::kFortsInstrumentstateRepl};
        out.private_streams_ready = std::all_of(required.begin(), required.end(), [&](auto code) {
            return std::count_if(out.streams.begin(), out.streams.end(), [&](const auto& row) {
                       return row.stream_code == code && row.online && row.snapshot_complete;
                   }) == 1;
        });
        for (const auto& row : out.streams) {
            if (row.stream_code == StreamCode::kFortsUserorderbookRepl)
                out.uob_periodic_consistent = row.periodic_snapshot_consistent;
        }
        const auto evidence = transport.inspect_target_evidence(config.order.client_code);
        out.refdata_lifenum = evidence.target_refdata_lifenum;
        out.target_refdata_provenance_ready = evidence.target_refdata_provenance_ready;
        out.fut_instruments_provenance = evidence.target_fut_instruments_provenance;
        out.fut_sess_contents_provenance = evidence.target_fut_sess_contents_provenance;
        out.session_provenance = evidence.target_session_provenance;
        out.position_evidence_class = evidence.position_evidence_class;
        out.zero_starting_position_proven = evidence.zero_starting_position_proven;
        out.pos_trades_rev = evidence.position_trades_rev;
        out.pos_trades_lifenum = evidence.position_trades_lifenum;
        out.trade_anchor = evidence.trade_replay_anchor_used;
        out.trade_replay_complete = evidence.trade_replay_complete;
        out.active_own_order_count = evidence.active_own_order_count;
        bool membership = false;
        for (const auto& row : data.instruments()) {
            if (row.isin_id != out.target_isin_id)
                continue;
            out.target = row.isin;
            out.min_step = row.min_step;
            if (row.has_current_status)
                out.instrument_status = row.current_status;
            membership = row.kind == ps::InstrumentKind::kFuture && row.current_session_member &&
                         row.sess_id == out.session_id && row.trade_mode_id != 0 && !row.min_step.empty();
        }
        for (const auto& row : data.sessions()) {
            if (row.sess_id == out.session_id && row.has_current_status)
                out.session_status = row.current_status;
        }
        const auto participant = config.order.broker_code + config.order.client_code;
        out.limits_set =
            !participant.empty() && std::count_if(data.limits().begin(), data.limits().end(), [&](const auto& row) {
                                        return row.scope == ps::PositionScope::kClient &&
                                               row.account_code == participant && row.limits_set;
                                    }) == 1;
        if (const auto bbo = host.aggr20_projector().snapshot_for_isin(out.target_isin_id)) {
            if (bbo->top_bid)
                out.bid = bbo->top_bid->price;
            if (bbo->top_ask)
                out.ask = bbo->top_ask->price;
            out.target_aggr20_uncrossed =
                bbo->top_bid && bbo->top_ask && bbo->top_bid->price_scaled < bbo->top_ask->price_scaled;
            out.bbo_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                   bbo->committed_at)
                                 .count();
        }
        const auto max_age = config.order.policy.max_aggr20_age_ms;
        out.observation_ready = host.started() && state != ConnectorHostState::Failed && out.publisher_ready &&
                                out.reply_ready && host.probe_report().trading_capable && out.private_streams_ready &&
                                out.uob_periodic_consistent && out.target_refdata_provenance_ready && membership &&
                                out.session_status == 1 && out.instrument_status == 1 && out.limits_set &&
                                out.trade_replay_complete && out.zero_starting_position_proven &&
                                out.active_own_order_count == 0 && host.aggr_online() &&
                                host.aggr_snapshot_complete() && out.target_aggr20_uncrossed && out.bbo_age_ms >= 0 &&
                                max_age > 0 && max_age <= 5000 && static_cast<std::uint64_t>(out.bbo_age_ms) <= max_age;
        out.last_error = error;
        if (out.state == ConnectorHostState::Ready && !out.observation_ready)
            out.state = ConnectorHostState::Started;
        if (out.last_error.empty() && host.started() && !out.observation_ready)
            out.last_error = "OBSERVATION_NOT_READY";
        if (result) {
            out.lifecycle_state = result->state;
            out.market_safe = result->market_safe_terminal;
            out.evidence_consistent = result->evidence_consistent;
            if (result->observation) {
                const auto& row = *result->observation;
                out.order_id = row.public_order_id != 0 ? row.public_order_id : row.private_order_id;
                out.original_quantity = row.original_quantity;
                out.remaining_quantity = row.remaining_quantity;
                out.executed_quantity = row.executed_quantity;
            }
        }
        return out;
    }

    OrderLifecycleConfig current_order() const {
        auto value = config.order;
        const auto view = snapshot();
        value.smoke = {};
        value.smoke.instrument_exists = view.target_refdata_provenance_ready;
        value.smoke.tradable_session = view.session_status == 1 && view.instrument_status == 1;
        value.smoke.aggr20_two_sided = view.target_aggr20_uncrossed;
        value.smoke.limits_snapshot_applicable = view.limits_set;
        value.smoke.tick_size = view.min_step;
        value.smoke.top_bid = view.bid;
        value.smoke.top_ask = view.ask;
        value.smoke.aggr20_age_ms = view.bbo_age_ms < 0 ? 5001 : static_cast<std::uint64_t>(view.bbo_age_ms);
        value.smoke.market_data_source = "FORTS_AGGR20_REPL";
        if (const auto bbo = transport.host().aggr20_projector().snapshot_for_isin(view.target_isin_id)) {
            value.smoke.aggr20_source_sequence = bbo->last_repl_id;
            value.smoke.aggr20_source_revision = bbo->last_repl_rev;
        }
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
        gmtime_r(&now, &utc);
        std::ostringstream timestamp;
        timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        value.smoke.aggr20_observed_at_utc = timestamp.str();
        // Date of this observation, not an inferred exchange session ID.
        value.smoke.trading_day = timestamp.str().substr(0, 10);
        value.smoke.session_id = std::to_string(view.session_id);
        value.smoke.session_state = "current-status";
        value.smoke.refdata_source = "FORTS_REFDATA_REPL.target_rows";
        value.smoke.refdata_source_sequence = view.refdata_lifenum;
        value.smoke.refdata_source_revision = view.fut_instruments_provenance.repl_rev;
        value.smoke.limits_source = "FORTS_PART_REPL";
        for (const auto& stream : view.streams) {
            if (stream.stream_code == StreamCode::kFortsPartRepl)
                value.smoke.limits_commit_sequence = stream.last_commit_sequence;
        }
        return value;
    }
};

ConnectorHost::ConnectorHost(Plaza2HostConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
ConnectorHost::~ConnectorHost() {
    (void)stop();
}

cg::Plaza2Error ConnectorHost::start() {
    auto& p = *impl_;
    if (p.state != ConnectorHostState::Created)
        return invalid("host start is one-shot");
    const auto& c = p.config;
    const auto& arms = c.transport.host.arm_state;
    if (c.transport.host.runtime.environment != cg::Plaza2Environment::Test ||
        c.order.environment != cg::Plaza2Environment::Test || c.transport.target_isin_id != c.order.isin_id ||
        c.order.isin_id <= 0 || c.transport.target_session_id <= 0 || c.transport.authorized_intent ||
        !c.transport.host.trade_replay_from_pos_anchor ||
        c.transport.observation_client_code != c.order.broker_code + c.order.client_code ||
        c.order.broker_code.empty() || c.order.client_code.empty()) {
        p.state = ConnectorHostState::Failed;
        p.error = "invalid TEST host target/account/anchor configuration";
        return invalid(p.error);
    }
    if ((c.purpose == HostPurpose::Qualify &&
         (arms.test_order_send_armed || c.transport.host.mode == Plaza2TestSessionHostMode::LiveTestAuthorizedSend)) ||
        (c.purpose == HostPurpose::OrderTest &&
         (c.transport.host.mode != Plaza2TestSessionHostMode::LiveTestAuthorizedSend || !arms.test_network_armed ||
          !arms.test_session_armed || !arms.test_plaza2_armed || !arms.test_order_send_armed))) {
        p.state = ConnectorHostState::Failed;
        p.error = "host purpose/send arms mismatch";
        return invalid(p.error);
    }
    if (auto error = p.transport.host().start()) {
        p.state = ConnectorHostState::Failed;
        p.error = "runtime start failed (code=" + std::to_string(static_cast<unsigned>(error.code)) + ")";
        return invalid(p.error);
    }
    p.state = ConnectorHostState::Started;
    return {};
}

cg::Plaza2Error ConnectorHost::poll() {
    auto& p = *impl_;
    if (p.state != ConnectorHostState::Started && p.state != ConnectorHostState::Ready)
        return invalid("poll requires a running host");
    if (auto error = p.transport.host().poll()) {
        p.state = ConnectorHostState::Failed;
        p.error = "runtime poll failed (code=" + std::to_string(static_cast<unsigned>(error.code)) + ")";
        return invalid(p.error);
    }
    p.state = p.snapshot().observation_ready ? ConnectorHostState::Ready : ConnectorHostState::Started;
    return {};
}

cg::Plaza2Error ConnectorHost::stop() {
    auto& p = *impl_;
    if (p.state == ConnectorHostState::Stopped)
        return {};
    p.state = ConnectorHostState::Stopping;
    if (auto error = p.transport.host().stop()) {
        p.state = ConnectorHostState::Failed;
        p.error = "runtime stop failed";
        return invalid(p.error);
    }
    p.state = ConnectorHostState::Stopped;
    return {};
}

ConnectorHostSnapshot ConnectorHost::snapshot() const {
    return impl_->snapshot();
}

PreSendPlan ConnectorHost::plan() const {
    if (!impl_->snapshot().observation_ready)
        return {.failure = PreSendFailure::SessionNotTradable, .message = "OBSERVATION_NOT_READY"};
    auto config = impl_->current_order();
    config.dry_run = true;
    config.send_test_order = false;
    config.any_arm_flag = false;
    return build_pre_send_plan(config);
}

cg::Plaza2Error ConnectorHost::authorize(std::string_view canonical, std::string_view sha) {
    auto& p = *impl_;
    if (p.config.purpose != HostPurpose::OrderTest || p.submitted || !p.authorized_sha.empty())
        return invalid("authorization is unavailable for this host");
    const auto candidate = plan();
    if (!candidate.ok || candidate.canonical_json != canonical || candidate.sha256 != sha) {
        p.error = "exact current canonical plan and authorization SHA required";
        return invalid(p.error);
    }
    const auto& c = p.config.order;
    Plaza2AuthorizedOrderIntent intent;
    intent.sha256 = candidate.sha256;
    intent.canonical_json = candidate.canonical_json;
    intent.profile_id = c.profile_id;
    intent.profile_fingerprint = c.profile_fingerprint;
    intent.add_payload_sha256 = cg::plaza2_sha256_hex(candidate.add_command.payload);
    intent.recovery_payload_sha256 = cg::plaza2_sha256_hex(candidate.exact_ext_id_recovery_command.payload);
    intent.isin_id = c.isin_id;
    intent.base_contract_code = c.base_contract_code;
    intent.side = c.side;
    intent.price = c.price;
    intent.quantity = c.quantity;
    intent.ext_id = c.ext_id;
    intent.add_user_id = c.add_user_id;
    intent.cancel_user_id = c.cancel_user_id;
    intent.recovery_user_id = c.recovery_user_id;
    intent.instrument_mask = c.instrument_mask;
    intent.broker_code = c.broker_code;
    intent.client_code = c.client_code;
    intent.broker_code_sha256 = cg::plaza2_sha256_hex(c.broker_code);
    intent.client_code_sha256 = cg::plaza2_sha256_hex(c.client_code);
    intent.policy_version = c.policy.version;
    intent.policy_sha256 = c.policy.sha256;
    intent.max_distance_ticks = c.policy.max_distance_ticks;
    intent.max_aggr20_age_ms = c.policy.max_aggr20_age_ms;
    intent.require_zero_starting_position = c.policy.require_zero_starting_position;
    if (auto error = p.transport.install_authorized_intent(std::move(intent)))
        return error;
    if (auto error = p.transport.bind_authorized_plan(candidate))
        return error;
    p.authorized_sha = candidate.sha256;
    p.error.clear();
    return {};
}

OrderLifecycleResult ConnectorHost::submit() {
    auto& p = *impl_;
    if (p.config.purpose != HostPurpose::OrderTest || p.authorized_sha.empty() || p.submitted)
        return {.message = "authorized one-shot order-test required"};
    if (poll() || !p.snapshot().observation_ready)
        return {.message = "OBSERVATION_NOT_READY"};
    p.submitted = true;
    auto config = p.current_order();
    config.dry_run = false;
    config.send_test_order = true;
    config.any_arm_flag = true;
    config.authorized_plan_sha256 = p.authorized_sha;
    SystemOrderLifecycleClock clock;
    OrderLifecycleController controller(std::move(config), p.transport, clock);
    p.result = controller.run();
    if (!p.result->ok) {
        p.state = ConnectorHostState::Failed;
        p.error = "order lifecycle failed; inspect local journal";
    } else
        p.state = ConnectorHostState::Started;
    return *p.result;
}

RestartReconciliationResult ConnectorHost::reconcile() {
    if (poll())
        return {.ok = false, .message = "reconciliation requires a healthy running host"};
    const auto view = snapshot();
    if (!view.private_streams_ready || !view.trade_replay_complete)
        return {.ok = false, .message = "reconciliation requires current private replication and anchored TRADE"};
    const auto& state = impl_->transport.host().private_state();
    return reconcile_unfinished_run(impl_->config.order, state.own_orders(), state.own_trades());
}

std::string render_snapshot(const ConnectorHostSnapshot& s, bool json) {
    std::ostringstream out;
    out << std::boolalpha;
    if (!json) {
        out << "state=" << host_state_name(s.state) << " target=" << s.target << " session=" << s.session_id
            << "\nobservation_ready=" << s.observation_ready << " publisher=" << s.publisher_ready
            << " reply=" << s.reply_ready << "\nbbo=" << s.bid << '/' << s.ask << " age_ms=" << s.bbo_age_ms
            << "\nposition=" << position_evidence_class_name(s.position_evidence_class)
            << " active_own_orders=" << s.active_own_order_count << " uob_periodic=" << s.uob_periodic_consistent
            << "\nmarket_safe=" << s.market_safe << " evidence_consistent=" << s.evidence_consistent
            << "\nlast_error=" << s.last_error << '\n';
        return out.str();
    }
    out << "{\"schema\":\"moex.connector-host.v1\",\"state\":" << quoted(host_state_name(s.state))
        << ",\"environment\":" << quoted(s.environment == cg::Plaza2Environment::Test ? "test" : "prod")
        << ",\"mode\":" << static_cast<unsigned>(s.mode)
        << ",\"runtime_compatibility\":" << quoted(s.runtime_compatibility)
        << ",\"runtime_scheme_sha256\":" << quoted(s.runtime_scheme_sha256)
        << ",\"publisher_ready\":" << s.publisher_ready << ",\"reply_ready\":" << s.reply_ready
        << ",\"private_streams_ready\":" << s.private_streams_ready << ",\"observation_ready\":" << s.observation_ready
        << ",\"target\":" << quoted(s.target) << ",\"target_isin_id\":" << s.target_isin_id
        << ",\"session_id\":" << s.session_id
        << ",\"session_status\":" << (s.session_status ? std::to_string(*s.session_status) : "null")
        << ",\"instrument_status\":" << (s.instrument_status ? std::to_string(*s.instrument_status) : "null")
        << ",\"min_step\":" << quoted(s.min_step) << ",\"bid\":" << quoted(s.bid) << ",\"ask\":" << quoted(s.ask)
        << ",\"bbo_age_ms\":" << s.bbo_age_ms << ",\"refdata_lifenum\":" << s.refdata_lifenum
        << ",\"target_refdata_provenance_ready\":" << s.target_refdata_provenance_ready
        << ",\"target_aggr20_uncrossed\":" << s.target_aggr20_uncrossed
        << ",\"position_evidence_class\":" << quoted(position_evidence_class_name(s.position_evidence_class))
        << ",\"zero_starting_position_proven\":" << s.zero_starting_position_proven
        << ",\"pos_trades_rev\":" << s.pos_trades_rev << ",\"pos_trades_lifenum\":" << s.pos_trades_lifenum
        << ",\"trade_replay_complete\":" << s.trade_replay_complete
        << ",\"active_own_order_count\":" << s.active_own_order_count
        << ",\"uob_periodic_consistent\":" << s.uob_periodic_consistent << ",\"limits_set\":" << s.limits_set
        << ",\"lifecycle_state\":"
        << (s.lifecycle_state ? quoted(order_lifecycle_state_name(*s.lifecycle_state)) : "null")
        << ",\"order_id\":" << s.order_id << ",\"original_quantity\":" << s.original_quantity
        << ",\"remaining_quantity\":" << s.remaining_quantity << ",\"executed_quantity\":" << s.executed_quantity
        << ",\"market_safe\":" << s.market_safe << ",\"evidence_consistent\":" << s.evidence_consistent
        << ",\"cg_pub_msgnew\":" << s.publisher_calls.msgnew << ",\"cg_pub_post\":" << s.publisher_calls.post
        << ",\"last_error\":" << quoted(s.last_error) << ",\"trade_anchor\":";
    if (s.trade_anchor)
        out << "{\"trades_rev\":" << s.trade_anchor->trades_rev
            << ",\"trades_lifenum\":" << s.trade_anchor->trades_lifenum
            << ",\"server_time\":" << s.trade_anchor->server_time << '}';
    else
        out << "null";
    out << ",\"target_provenance\":[";
    bool first = true;
    for (const auto& row : {s.fut_instruments_provenance, s.fut_sess_contents_provenance, s.session_provenance}) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"present\":" << row.present << ",\"table_code\":" << static_cast<unsigned>(row.table_code)
            << ",\"lifenum\":" << row.lifenum << ",\"repl_rev\":" << row.repl_rev << '}';
    }
    out << "],\"streams\":[";
    first = true;
    for (const auto& row : s.streams) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"name\":" << quoted(row.stream_name) << ",\"online\":" << row.online
            << ",\"snapshot_complete\":" << row.snapshot_complete
            << ",\"periodic_snapshot_consistent\":" << row.periodic_snapshot_consistent << '}';
    }
    out << "]}\n";
    return out.str();
}

} // namespace moex::connector_host
