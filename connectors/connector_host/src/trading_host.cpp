#include "moex/connector_host/trading_host.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <ostream>

namespace moex::connector_host {
namespace {
namespace cg = plaza2::cgate;
namespace tr = plaza2_trade;
namespace gen = plaza2::generated;
std::string_view stream_name(gen::StreamCode code) {
    // The descriptor is a depth template; this product opens AGGR20.
    if (code == gen::StreamCode::kFortsAggrRepl)
        return "FORTS_AGGR20_REPL";
    const auto* descriptor = gen::FindStreamByCode(code);
    return descriptor ? descriptor->stream_name : code == cg::kNoStreamCode ? "p2mqreply" : "unknown";
}
std::string masked_settings(std::string_view settings) {
    // CGate URLs use semicolon-separated settings. Preserve operational
    // parameters explicitly; credentials and unrecognized values stay masked.
    constexpr std::array public_keys{"app_name", "timeout", "name",    "category", "ref",
                                     "scheme",   "mode",    "lifenum", "log",      "ini"};
    std::string result;
    while (!settings.empty()) {
        const auto end = settings.find(';');
        const auto part = settings.substr(0, end);
        if (!result.empty())
            result += ';';
        const auto equals = part.find('=');
        const auto protocol = part.find("://");
        if (protocol != std::string_view::npos) {
            const auto scheme = part.substr(0, protocol);
            if (scheme == "p2tcp" || scheme == "p2repl" || scheme == "p2mq" || scheme == "p2mqreply") {
                const auto authority = part.substr(protocol + 3);
                const auto at = authority.find('@');
                result += part.substr(0, protocol + 3);
                if (at != std::string_view::npos)
                    result += "[REDACTED]@";
                const auto endpoint = authority.substr(at == std::string_view::npos ? 0 : at + 1);
                result += endpoint.substr(0, endpoint.find('?'));
            } else
                result += "[REDACTED_URL]";
        } else if (equals != std::string_view::npos) {
            const auto key = part.substr(0, equals);
            result += key;
            result += '=';
            const bool public_value =
                std::find(public_keys.begin(), public_keys.end(), key) != public_keys.end() || key.starts_with("rev.");
            result += public_value ? part.substr(equals + 1) : "[REDACTED]";
        } else if (!part.empty())
            result += "[REDACTED]";
        if (end == std::string_view::npos)
            break;
        settings.remove_prefix(end + 1);
    }
    return result;
}
std::string named_transport_fields(std::string_view fields) {
    std::string result(fields);
    constexpr std::string_view key = "\"stream_code\":";
    const auto offset = fields.find(key);
    if (offset != std::string_view::npos && !fields.empty() && fields.back() == '}') {
        std::uint32_t code{};
        const auto value = fields.substr(offset + key.size());
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), code);
        if (parsed.ec == std::errc{} && code != 0)
            result.insert(result.size() - 1,
                          ",\"stream\":" + json_string(stream_name(static_cast<gen::StreamCode>(code))));
    }
    return result;
}
std::int64_t utc_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::filesystem::path identity_path(const TradingHostConfig& config) {
    if (!config.identity_state_path.empty())
        return config.identity_state_path;
    const auto& instance = config.session.publisher_name;
    if (instance.empty() ||
        instance.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
            std::string::npos)
        throw std::invalid_argument("identity state requires a valid instance name");
    return config.journal_path.parent_path() / (instance + ".state");
}
std::string event_kind(cg::Plaza2ListenerEventKind kind) {
    switch (kind) {
    case cg::Plaza2ListenerEventKind::Open:
        return "listener_open";
    case cg::Plaza2ListenerEventKind::Close:
        return "listener_close";
    case cg::Plaza2ListenerEventKind::TransactionBegin:
        return "transaction_begin";
    case cg::Plaza2ListenerEventKind::TransactionCommit:
        return "transaction_commit";
    case cg::Plaza2ListenerEventKind::StreamData:
        return "stream_row";
    case cg::Plaza2ListenerEventKind::Online:
        return "listener_online";
    case cg::Plaza2ListenerEventKind::LifeNum:
        return "lifenum";
    case cg::Plaza2ListenerEventKind::ClearDeleted:
        return "clear_deleted";
    case cg::Plaza2ListenerEventKind::ReplState:
        return "replstate";
    case cg::Plaza2ListenerEventKind::Timeout:
        return "reply_timeout";
    }
    return "listener_event";
}
} // namespace
CgateTradingHost::CgateTradingHost(TradingHostConfig config)
    : config_(std::move(config)), owner_(std::this_thread::get_id()),
      journal_(config_.journal_path, identity_path(config_)), session_(session_config()) {
    if (config_.isin_ids.empty())
        throw std::invalid_argument("at least one trading instrument is required");
    auto orders = config_.orders;
    const auto reservations = journal_.reservations();
    orders.next_ext_id = std::max(orders.next_ext_id, reservations.next_ext_id);
    orders.next_user_id = std::max(orders.next_user_id, reservations.next_user_id);
    orders_ = std::make_unique<OrderManager>(
        std::move(orders),
        [this](const auto& command, auto id) {
            if (!log_error_.empty())
                return cg::Plaza2PublisherMessageResult{
                    .certainty = cg::Plaza2SubmissionCertainty::DefinitelyNotSent,
                    .validation_error = {.code = cg::Plaza2ErrorCode::RuntimeCallFailed, .message = log_error_}};
            return session_.post_command(command, id);
        },
        [this](auto isin) {
            return !rebuilding_ && log_error_.empty() &&
                   std::find(config_.isin_ids.begin(), config_.isin_ids.end(), isin) != config_.isin_ids.end() &&
                   moex::connector_host::order_entry_ready(session_, isin);
        },
        [this](auto isin) -> std::optional<plaza2::private_state::FutureSessionTerms> {
            const auto& data = session_.private_state();
            const auto life = data.refdata_lifenum();
            const auto sess = current_session_id(data);
            return life && sess ? data.find_future_session_terms(isin, sess, *life) : std::nullopt;
        },
        [this](auto kind, auto fields) noexcept { log_event(kind, fields); });
}
CgateTradingHost::~CgateTradingHost() noexcept {
    // Stop while callbacks can still use log_error_ and journal_. Member
    // destruction would otherwise tear down log_error_ before session_.
    try {
        const auto error = stop();
        (void)error;
    } catch (...) {
        // Explicit stop() reports errors; destruction must remain noexcept.
    }
}
void CgateTradingHost::assert_owner() const {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("CGate objects must be used on the owning thread");
}
tr::CgateSessionConfig CgateTradingHost::session_config() {
    auto result = config_.session;
    result.publisher_rate_owner = tr::PublisherRateOwner::External;
    result.event_log = [this](auto kind, auto fields) { log_event(kind, fields); };
    result.listener_event_log = [this](const auto& event) { log_listener_event(event); };
    return result;
}
void CgateTradingHost::log_event(std::string_view kind, std::string_view fields) noexcept {
    try {
        journal_.append(kind, kind == "cgate_state" || kind == "cgate_operation" ? named_transport_fields(fields)
                                                                                 : std::string(fields));
        if (kind == "cgate_state" && !stopped_)
            observe_link(session_.runtime_health());
    } catch (const std::exception& error) {
        log_error_ = error.what();
    }
}
void CgateTradingHost::observe_link(const tr::Plaza2TransportHealth& health) {
    const bool active = health.valid && health.connection == 3 && health.publisher == 3 && health.reply == 3;
    if (link_was_active_ && !active && !link_lost_) {
        link_lost_ = true;
        journal_.append("link_lost", "{\"connection_state\":" + std::to_string(health.connection) +
                                         ",\"publisher_state\":" + std::to_string(health.publisher) +
                                         ",\"reply_state\":" + std::to_string(health.reply) + "}");
    } else if (active) {
        if (link_lost_) {
            link_lost_ = false;
            journal_.append("link_restored", "{\"connection_state\":3,\"publisher_state\":3,\"reply_state\":3}");
        }
        link_was_active_ = true;
    }
}
void CgateTradingHost::log_listener_event(const cg::Plaza2ListenerEvent& event) noexcept {
    try {
        if (event.stream_code == plaza2::generated::StreamCode::kFortsAggrRepl) {
            // Book traffic and its transaction/replay boundaries are not order
            // interaction evidence. Keep lifecycle changes and the rare session
            // announcements required by the certification interaction log.
            const bool lifecycle = event.kind == cg::Plaza2ListenerEventKind::Open ||
                                   event.kind == cg::Plaza2ListenerEventKind::Close ||
                                   event.kind == cg::Plaza2ListenerEventKind::Online ||
                                   event.kind == cg::Plaza2ListenerEventKind::LifeNum ||
                                   event.kind == cg::Plaza2ListenerEventKind::ClearDeleted;
            const bool announcement = event.kind == cg::Plaza2ListenerEventKind::StreamData &&
                                      event.table_code == plaza2::generated::TableCode::kFortsAggrReplSysEvents;
            if (!lifecycle && !announcement)
                return;
        }
        const auto* table = gen::FindTableByCode(event.table_code);
        std::string fields =
            "{\"stream\":" + json_string(stream_name(event.stream_code)) +
            ",\"table\":" + json_string(table ? table->table_name : std::string_view{}) +
            ",\"stream_code\":" + std::to_string(static_cast<std::uint32_t>(event.stream_code)) +
            ",\"table_code\":" + std::to_string(static_cast<std::uint32_t>(event.table_code)) +
            ",\"name\":" + json_string(event.message_name) + ",\"message_id\":" + std::to_string(event.message_id) +
            ",\"user_id\":" + std::to_string(event.user_id) + ",\"value\":" + std::to_string(event.unsigned_value) +
            ",\"signed_value\":" + std::to_string(event.signed_value) +
            ",\"table_index\":" + std::to_string(event.table_index) +
            ",\"clear_deleted_flags\":" + std::to_string(event.clear_deleted_flags) +
            ",\"text\":" + json_string(event.text_value) +
            ",\"payload_hex\":" + json_string(tr::bytes_to_hex(event.raw_payload)) + ",\"fields\":[";
        bool first = true;
        for (const auto& field : event.fields) {
            if (!first)
                fields += ',';
            first = false;
            const auto* descriptor = gen::FindFieldByCode(field.field_code);
            fields += "{\"name\":" + json_string(descriptor ? descriptor->field_name : std::string_view{}) +
                      ",\"id\":" + std::to_string(static_cast<std::uint32_t>(field.field_code)) +
                      ",\"kind\":" + std::to_string(static_cast<int>(field.kind)) +
                      ",\"signed\":" + std::to_string(field.signed_value) +
                      ",\"unsigned\":" + std::to_string(field.unsigned_value) +
                      ",\"timestamp_ns\":" + std::to_string(field.timestamp_ns) +
                      ",\"text\":" + json_string(field.text_value) + "}";
        }
        fields += "]}";
        journal_.append(event_kind(event.kind), fields);
    } catch (const std::exception& error) {
        log_error_ = error.what();
    }
}
cg::Plaza2Error CgateTradingHost::start() {
    assert_owner();
    if (stopped_)
        return {.code = cg::Plaza2ErrorCode::InvalidConfiguration,
                .message = "trading host has stopped; create a fresh host to restart"};
    if (config_.session.mode != tr::CgateSessionMode::OfflineFake)
        validate_cgate_logging(config_.session.runtime.env_open_settings, config_.session.runtime.config_dir);
    const auto& session = config_.session;
    std::string urls;
    const auto add_url = [&](std::string_view name, std::string_view settings, std::string_view open_settings) {
        if (settings.empty())
            return;
        if (!urls.empty())
            urls += ',';
        urls += "{\"name\":" + json_string(name) + ",\"url\":" + json_string(masked_settings(settings)) +
                ",\"open_settings\":" + json_string(masked_settings(open_settings)) + "}";
    };
    add_url("connection", session.connection_settings, session.connection_open_settings);
    for (const auto& stream : session.private_streams)
        add_url(stream_name(stream.stream_code), stream.settings, stream.open_settings);
    for (const auto& stream : session.status_streams)
        add_url(stream_name(stream.stream_code), stream.settings, stream.open_settings);
    add_url(stream_name(session.aggr20_stream.stream_code), session.aggr20_stream.settings,
            session.aggr20_stream.open_settings);
    add_url(stream_name(session.public_deals_stream.stream_code), session.public_deals_stream.settings,
            session.public_deals_stream.open_settings);
    add_url("publisher", session.publisher_settings, session.publisher_open_settings);
    add_url("p2mqreply", session.p2mqreply_settings, session.p2mqreply_open_settings);
    auto router = masked_settings(session.connection_settings.substr(0, session.connection_settings.find(';')));
    if (router.starts_with("p2tcp://"))
        router.erase(0, 8);
    journal_.append(
        "startup",
        "{\"product\":\"MoexConnector\",\"version\":\"1.0.0\",\"instance_id\":" + json_string(session.publisher_name) +
            ",\"source_git_sha\":" + json_string(config_.source_git_sha) +
            ",\"binary_sha256\":" + json_string(config_.binary_sha256) + ",\"router\":" + json_string(router) +
            ",\"rate\":" + std::to_string(config_.orders.max_commands_per_second) +
            ",\"allow_orders\":" + (session.allow_orders ? "true" : "false") +
            ",\"risk\":{\"max_quantity\":" + std::to_string(config_.orders.risk.max_quantity) +
            ",\"max_open_orders\":" + std::to_string(config_.orders.risk.max_open_orders) +
            ",\"max_notional_scaled\":" + std::to_string(config_.orders.risk.max_notional_scaled) +
            ",\"kill_switch\":" + (config_.orders.risk.kill_switch ? "true" : "false") + "},\"urls\":[" + urls +
            "],\"env_settings\":" + json_string(masked_settings(session.runtime.env_open_settings)) +
            ",\"clock_offset_us\":" + (config_.clock_offset_us ? std::to_string(*config_.clock_offset_us) : "null") +
            ",\"clock_offset_source\":" +
            json_string(config_.clock_offset_us ? "operator measurement" : "unavailable") + "}");
    return session_.start();
}
cg::Plaza2Error CgateTradingHost::poll() {
    assert_owner();
    dispatch_commands();
    if (!log_error_.empty())
        return {.code = cg::Plaza2ErrorCode::RuntimeCallFailed, .message = log_error_};
    const auto error = session_.poll(orders_->queued() == 0);
    try {
        observe_link(session_.runtime_health());
    } catch (const std::exception& log_error) {
        log_error_ = log_error.what();
    }
    if (!log_error_.empty()) {
        orders_->set_kill_switch(true);
        return {.code = cg::Plaza2ErrorCode::RuntimeCallFailed, .message = log_error_};
    }
    const auto now = config_.session.recovery_now ? config_.session.recovery_now() : OrderManager::Clock::now();
    const auto& data = session_.private_state();
    bool trade_online{}, user_book_online{};
    std::int64_t server_time{};
    std::uint64_t trade_commit_sequence{};
    for (const auto& stream : data.stream_health())
        if (stream.stream_name == "FORTS_TRADE_REPL") {
            trade_online = stream.online && stream.snapshot_complete;
            server_time = stream.last_server_time;
            trade_commit_sequence = stream.last_commit_sequence;
        } else if (stream.stream_name == "FORTS_USERORDERBOOK_REPL")
            user_book_online = stream.online && stream.snapshot_complete;
    const auto changes = session_.take_private_row_changes();
    if (changes.regular_trade_history_truncated)
        orders_->invalidate_execution_baselines();
    for (const auto& event : session_.take_reply_events()) {
        if (event.timed_out) {
            orders_->on_timeout(event.user_id, now);
            continue;
        }
        plaza2_trade::Plaza2TradeValidationResult validation;
        const auto reply =
            plaza2_trade::Plaza2TradeCodec{}.decode_reply(event.message_id, event.raw_payload, validation);
        if (validation.ok())
            orders_->on_reply(event.user_id, reply, now, trade_commit_sequence);
        else {
            log_event("malformed_reply", "{\"user_id\":" + std::to_string(event.user_id) +
                                             ",\"error\":" + json_string(validation.message) + "}");
            orders_->on_timeout(event.user_id, now);
        }
    }
    // Use the startup snapshot barrier again after a lost delta batch or TRADE
    // disconnect. Keep the existing logical orders and command correlations;
    // the current committed snapshots reconcile their identities and exposure.
    if (!rebuilding_ && (changes.resync_required || !trade_online)) {
        rebuilding_ = true;
        log_event("private_history_gap", "{\"recovering\":true}");
    }
    if (rebuilding_) {
        // The bootstrap barrier reconciles full committed snapshots. Delta
        // accumulation while waiting for USERORDERBOOK serves no consumer.
        if (trade_online && user_book_online) {
            orders_->observe_orders(data.own_orders(), true);
            orders_->observe_trades(data.own_trades());
            rebuilding_ = false;
        }
    } else {
        orders_->observe_orders(changes.orders);
        orders_->observe_trades(changes.trades);
    }
    // Empty TRADE commits also establish the post-186 reconciliation boundary.
    // Apply their rows first so individual fallbacks see only surviving orders.
    orders_->observe_trade_commit(trade_commit_sequence);
    if (!rebuilding_)
        orders_->prove_absence(server_time, trade_online);
    // ID blocks are already durable. Sync interaction records on the owner
    // loop's 250ms schedule, outside append and transport submission.
    try {
        journal_.flush_if_due();
    } catch (const std::exception& storage_error) {
        log_error_ = storage_error.what();
        orders_->set_kill_switch(true);
        return {.code = cg::Plaza2ErrorCode::RuntimeCallFailed, .message = log_error_};
    }
    dispatch_commands();
    if (!log_error_.empty()) {
        orders_->set_kill_switch(true);
        return {.code = cg::Plaza2ErrorCode::RuntimeCallFailed, .message = log_error_};
    }
    return error;
}
cg::Plaza2Error CgateTradingHost::stop() {
    assert_owner();
    if (stopped_)
        return stop_error_;
    stopped_ = true;
    orders_->set_kill_switch(true);
    try {
        journal_.append("shutdown");
    } catch (const std::exception& error) {
        log_error_ = error.what();
    }
    // Storage failures must never leave gateway handles open. Close first,
    // then attempt to flush the shutdown and close records together.
    const auto session_error = session_.stop();
    try {
        journal_.flush();
    } catch (const std::exception& error) {
        log_error_ = error.what();
    }
    stop_error_ = log_error_.empty()
                      ? session_error
                      : cg::Plaza2Error{.code = cg::Plaza2ErrorCode::RuntimeCallFailed,
                                        .message = "journal storage failure during shutdown: " + log_error_};
    return stop_error_;
}
std::string CgateTradingHost::place(OrderRequest request) {
    assert_owner();
    auto error = orders_->place(std::move(request));
    if (error.empty())
        dispatch_commands();
    return error;
}
std::string CgateTradingHost::cancel(std::string_view key) {
    assert_owner();
    auto error = orders_->cancel(key);
    if (error.empty())
        dispatch_commands();
    return error;
}
std::string CgateTradingHost::move(std::string_view key, std::string price, std::int32_t quantity) {
    assert_owner();
    auto error = orders_->move(key, std::move(price), quantity);
    if (error.empty())
        dispatch_commands();
    return error;
}
std::string CgateTradingHost::cancel_all(std::int32_t isin) {
    assert_owner();
    auto error = orders_->cancel_all(isin);
    if (error.empty())
        dispatch_commands();
    return error;
}
void CgateTradingHost::dispatch_commands() {
    if (log_error_.empty()) {
        const auto now = config_.session.recovery_now ? config_.session.recovery_now() : OrderManager::Clock::now();
        orders_->poll(now, config_.utc_now ? config_.utc_now() : utc_seconds());
    }
    if (!log_error_.empty())
        orders_->set_kill_switch(true);
}
void CgateTradingHost::set_kill_switch(bool enabled) {
    assert_owner();
    orders_->set_kill_switch(enabled);
}
void CgateTradingHost::record_operator_input(std::string_view line, std::string_view channel) {
    assert_owner();
    log_event("operator_input", "{\"channel\":" + json_string(channel) + ",\"line\":" + json_string(line) + "}");
}
void CgateTradingHost::record_local_refusal(std::string_view line, std::string_view error, std::string_view channel) {
    assert_owner();
    log_event("local_refusal", "{\"channel\":" + json_string(channel) + ",\"line\":" + json_string(line) +
                                   ",\"error\":" + json_string(error) + "}");
}
bool CgateTradingHost::has_pending_cancellations() const {
    assert_owner();
    return orders_->cancellations_pending();
}
bool CgateTradingHost::has_working_orders() const {
    assert_owner();
    return std::any_of(orders_->orders().begin(), orders_->orders().end(),
                       [](const auto& entry) { return !terminal(entry.second.state); });
}
void CgateTradingHost::report_outstanding_orders(std::ostream& output) const {
    assert_owner();
    bool warned{};
    for (const auto& [key, order] : orders_->orders()) {
        if (terminal(order.state))
            continue;
        if (!warned) {
            output << "moexctl: trading driver is stopping. Working orders may remain on the exchange; "
                      "a disconnect does not cancel them. Confirm venue state through the broker/exchange emergency "
                      "channel.\n";
            warned = true;
        }
        output << "outstanding order client_order_id=" << json_string(key) << " order_id=" << order.order_id
               << " ext_id=" << order.ext_id << " isin_id=" << order.request.isin_id << " sess_id=" << order.sess_id
               << " state=" << order_state_name(order.state) << " remaining=" << order.remaining << '\n';
    }
    output << std::flush;
}
std::string CgateTradingHost::status() const {
    assert_owner();
    const auto& data = session_.private_state();
    std::string result =
        "{\"version\":\"1.0.0\",\"queued\":" + std::to_string(orders_->queued()) +
        ",\"reconstructing\":" + (rebuilding_ ? "true" : "false") +
        ",\"cgate_key_check_failed\":" + (session_.recovery_status().key_check_failed ? "true" : "false") +
        ",\"sess_id\":" + std::to_string(current_session_id(data)) + ",\"log_error\":" + json_string(log_error_) +
        ",\"operator_action_required\":" + (orders_->operator_action_required() ? "true" : "false") +
        ",\"instruments\":[";
    bool first = true;
    for (const auto isin : config_.isin_ids) {
        if (!first)
            result += ',';
        first = false;
        result += "{\"isin_id\":" + std::to_string(isin) + ",\"order_entry_ready\":" +
                  (!rebuilding_ && log_error_.empty() && order_entry_ready(session_, isin) ? "true" : "false") + "}";
    }
    result += "],\"positions\":[";
    first = true;
    for (const auto& position : data.positions()) {
        if (position.account_code != config_.orders.broker_code + config_.orders.client_code)
            continue;
        if (!first)
            result += ',';
        first = false;
        result +=
            "{\"isin_id\":" + std::to_string(position.isin_id) + ",\"xpos\":" + std::to_string(position.xpos) + "}";
    }
    result += "],\"orders\":[";
    first = true;
    for (const auto& [key, order] : orders_->orders()) {
        if (!first)
            result += ',';
        first = false;
        result += "{\"client_order_id\":" + json_string(key) + ",\"order_id\":" + std::to_string(order.order_id) +
                  ",\"ext_id\":" + std::to_string(order.ext_id) + ",\"sess_id\":" + std::to_string(order.sess_id) +
                  ",\"isin_id\":" + std::to_string(order.request.isin_id) +
                  ",\"state\":" + json_string(order_state_name(order.state)) +
                  ",\"remaining\":" + std::to_string(order.remaining) +
                  ",\"executed\":" + std::to_string(order.executed) +
                  ",\"execution_baseline_known\":" + (order.execution_baseline_known ? "true" : "false") +
                  ",\"operator_action_required\":" + (order.operator_action_required ? "true" : "false") +
                  ",\"last_error\":" + json_string(order.last_error) + "}";
    }
    return result + "]}";
}
} // namespace moex::connector_host
