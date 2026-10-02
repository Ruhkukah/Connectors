#include "moex/connector_host/trading_host.hpp"

#include <algorithm>
#include <stdexcept>
#include <ostream>

namespace moex::connector_host {
namespace {
namespace cg = plaza2::cgate;
namespace tr = plaza2_trade;
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
            try {
                if (!log_error_.empty())
                    throw std::runtime_error(log_error_);
                journal_.flush_reservations();
            } catch (const std::exception& error) {
                log_error_ = error.what();
                return cg::Plaza2PublisherMessageResult{
                    .certainty = cg::Plaza2SubmissionCertainty::DefinitelyNotSent,
                    .validation_error = {.code = cg::Plaza2ErrorCode::RuntimeCallFailed, .message = log_error_}};
            }
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
        journal_.append(kind, fields);
    } catch (const std::exception& error) {
        log_error_ = error.what();
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
        std::string fields =
            "{\"stream\":" + std::to_string(static_cast<int>(event.stream_code)) +
            ",\"table\":" + std::to_string(static_cast<int>(event.table_code)) +
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
            fields += "{\"id\":" + std::to_string(static_cast<int>(field.field_code)) +
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
    journal_.append("startup", "{\"product\":\"MoexConnector\",\"version\":\"1.0.0\",\"instance_id\":" +
                                   json_string(config_.session.publisher_name) + ",\"clock_offset_us\":" +
                                   (config_.clock_offset_us ? std::to_string(*config_.clock_offset_us) : "null") +
                                   ",\"clock_offset_source\":" +
                                   json_string(config_.clock_offset_us ? "operator measurement" : "unavailable") + "}");
    return session_.start();
}
cg::Plaza2Error CgateTradingHost::poll() {
    assert_owner();
    const auto error = session_.poll();
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
    const auto changes = session_.take_private_row_changes();
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
    orders_->prove_absence(server_time, trade_online);
    // Reserve identities durably once per owner-loop batch, then submit the
    // queued commands. The event stream itself uses 250ms group commit.
    try {
        journal_.flush_reservations();
        journal_.flush_if_due();
    } catch (const std::exception& storage_error) {
        log_error_ = storage_error.what();
        orders_->set_kill_switch(true);
        return {.code = cg::Plaza2ErrorCode::RuntimeCallFailed, .message = log_error_};
    }
    orders_->poll(now, config_.utc_now ? config_.utc_now() : utc_seconds());
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
    return orders_->place(std::move(request));
}
std::string CgateTradingHost::cancel(std::string_view key) {
    assert_owner();
    return orders_->cancel(key);
}
std::string CgateTradingHost::move(std::string_view key, std::string price, std::int32_t quantity) {
    assert_owner();
    return orders_->move(key, std::move(price), quantity);
}
std::string CgateTradingHost::cancel_all(std::int32_t isin) {
    assert_owner();
    return orders_->cancel_all(isin);
}
void CgateTradingHost::set_kill_switch(bool enabled) {
    assert_owner();
    orders_->set_kill_switch(enabled);
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
    std::string result = "{\"version\":\"1.0.0\",\"queued\":" + std::to_string(orders_->queued()) +
                         ",\"reconstructing\":" + (rebuilding_ ? "true" : "false") +
                         ",\"sess_id\":" + std::to_string(current_session_id(data)) +
                         ",\"log_error\":" + json_string(log_error_) +
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
