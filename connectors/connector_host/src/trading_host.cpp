#include "moex/connector_host/trading_host.hpp"
#include "moex/plaza2/cgate/plaza2_field_read_audit.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <ostream>
#include <iostream>

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
std::string exchange_message_fields(const plaza2::private_state::SystemMessageSnapshot& message) {
    return "{\"repl_id\":" + std::to_string(message.repl_id) + ",\"repl_rev\":" + std::to_string(message.repl_rev) +
           ",\"lifenum\":" + std::to_string(message.source.lifenum) + ",\"msg_id\":" + std::to_string(message.msg_id) +
           ",\"lang_code\":" + json_string(message.lang_code) + ",\"type_id\":" + std::to_string(message.type_id) +
           ",\"moment\":" + std::to_string(message.moment) + ",\"urgency\":" + std::to_string(message.urgency) +
           ",\"status\":" + std::to_string(message.status) + ",\"text\":" + json_string(message.text) +
           ",\"message_body\":" + json_string(message.message_body) + "}";
}
std::string state_fields(std::uint32_t state) {
    constexpr std::array names{"CLOSED", "ERROR", "OPENING", "ACTIVE"};
    return "{\"state\":" + std::to_string(state) +
           ",\"name\":" + json_string(state < names.size() ? names[state] : "UNKNOWN") + "}";
}
std::string_view operation_name(tr::Plaza2SessionOperation operation) {
    switch (operation) {
    case tr::Plaza2SessionOperation::Stopped:
        return "Stopped";
    case tr::Plaza2SessionOperation::Starting:
        return "Starting";
    case tr::Plaza2SessionOperation::Running:
        return "Running";
    case tr::Plaza2SessionOperation::Recovering:
        return "Recovering";
    case tr::Plaza2SessionOperation::Failed:
        return "Failed";
    }
    return "Unknown";
}
std::string error_fields(const cg::Plaza2Error& error) {
    return "{\"code\":" + std::to_string(static_cast<unsigned>(error.code)) +
           ",\"incompatible_scheme\":" + (error.code == cg::Plaza2ErrorCode::IncompatibleScheme ? "true" : "false") +
           ",\"runtime_code\":" + std::to_string(error.runtime_code) + ",\"message\":" + json_string(error.message) +
           "}";
}
std::string transport_fields(const tr::CgateSession& session, const tr::CgateSessionConfig& config) {
    const auto health = session.runtime_health();
    const auto& recovery = session.recovery_status();
    const auto streams = session.private_state().stream_health();
    std::string result = "{\"valid\":" + std::string(health.valid ? "true" : "false") +
                         ",\"connection\":" + state_fields(health.connection) +
                         ",\"publisher\":" + state_fields(health.publisher) + ",\"listeners\":[";
    bool first = true;
    const auto listener = [&](gen::StreamCode code, std::uint32_t state) {
        if (!first)
            result += ',';
        first = false;
        const auto projected =
            std::find_if(streams.begin(), streams.end(), [&](const auto& row) { return row.stream_code == code; });
        const bool aggr = code == gen::StreamCode::kFortsAggrRepl;
        const bool reply = code == cg::kNoStreamCode;
        const bool online = aggr    ? session.aggr_online()
                            : reply ? state == 3
                                    : projected != streams.end() && projected->online;
        const bool complete = aggr    ? session.aggr_snapshot_complete()
                              : reply ? state == 3
                                      : projected != streams.end() && projected->snapshot_complete;
        result += "{\"stream\":" + json_string(stream_name(code)) + ",\"native\":" + state_fields(state) +
                  ",\"online\":" + (online ? "true" : "false") +
                  ",\"snapshot_complete\":" + (complete ? "true" : "false") +
                  ",\"error\":" + error_fields(session.listener_error(code)) + "}";
    };
    const auto private_listener = [&](const auto& stream) {
        const auto found = std::find(health.private_streams.begin(),
                                     health.private_streams.begin() + health.private_count, stream.stream_code);
        listener(stream.stream_code, found == health.private_streams.begin() + health.private_count
                                         ? 0
                                         : health.private_states[found - health.private_streams.begin()]);
    };
    for (const auto& stream : config.private_streams)
        private_listener(stream);
    for (const auto& stream : config.status_streams)
        private_listener(stream);
    if (!config.p2mqreply_settings.empty())
        listener(cg::kNoStreamCode, health.reply);
    if (!config.aggr20_stream.settings.empty())
        listener(config.aggr20_stream.stream_code, health.aggr);
    if (!config.public_deals_stream.settings.empty())
        listener(config.public_deals_stream.stream_code, health.public_deals);
    return result + "],\"recovery\":{\"operation\":" + json_string(operation_name(recovery.operation)) +
           ",\"wait_state\":" + json_string(tr::plaza2_recovery_wait_state_name(recovery.wait_state)) +
           ",\"service\":" + json_string(recovery.involved_service) +
           ",\"generation\":" + std::to_string(recovery.generation) +
           ",\"attempts\":" + std::to_string(recovery.attempts) +
           ",\"transitions\":" + std::to_string(recovery.transitions) +
           ",\"wait_duration_ms\":" + std::to_string(recovery.wait_duration_ms) +
           ",\"alert_active\":" + (recovery.alert_active ? "true" : "false") +
           ",\"cause\":" + error_fields(recovery.cause) + "}}";
}
bool journal_replication_row(const cg::Plaza2ListenerEvent& event, const TradingHostConfig& config) {
    using enum gen::FieldCode;
    using enum gen::TableCode;
    const auto field = [&](gen::FieldCode code) -> const cg::Plaza2DecodedFieldValue* {
#ifdef MOEX_CGATE_FIELD_READ_AUDIT
        gen::AuditFieldRead(code);
#endif
        const auto found = std::find_if(event.fields.begin(), event.fields.end(),
                                        [code](const auto& value) { return value.field_code == code; });
        return found == event.fields.end() ? nullptr : &*found;
    };
    const auto deleted = [&](gen::FieldCode code) {
        const auto* value = field(code);
        return value && value->signed_value != 0;
    };
    const auto own = [&](gen::FieldCode code, gen::FieldCode repl_act) {
        const auto* value = field(code);
        // Native tombstones may omit ownership columns. Keep that rare
        // deletion evidence without an additional order-history index.
        return (value && value->text_value == config.orders.broker_code + config.orders.client_code) ||
               ((!value || value->text_value.empty()) && deleted(repl_act));
    };
    const auto target = [&](gen::FieldCode code, gen::FieldCode repl_act) {
        const auto* value = field(code);
        return (value && std::find(config.isin_ids.begin(), config.isin_ids.end(), value->signed_value) !=
                             config.isin_ids.end()) ||
               ((!value || value->kind == cg::Plaza2DecodedValueKind::None) && deleted(repl_act));
    };
    switch (event.table_code) {
    case kFortsTradeReplOrdersLog:
        return own(kFortsTradeReplOrdersLogClientCode, kFortsTradeReplOrdersLogReplAct);
    case kFortsUserorderbookReplOrders:
        return own(kFortsUserorderbookReplOrdersClientCode, kFortsUserorderbookReplOrdersReplAct);
    case kFortsTradeReplUserDeal:
        return own(kFortsTradeReplUserDealCodeBuy, kFortsTradeReplUserDealReplAct) ||
               own(kFortsTradeReplUserDealCodeSell, kFortsTradeReplUserDealReplAct);
    case kFortsPosReplPosition: {
        const auto* type = field(kFortsPosReplPositionAccountType);
        return own(kFortsPosReplPositionClientCode, kFortsPosReplPositionReplAct) &&
               target(kFortsPosReplPositionIsinId, kFortsPosReplPositionReplAct) &&
               ((type && type->signed_value == (config.orders.client_code.empty() ? 1 : 2)) ||
                deleted(kFortsPosReplPositionReplAct));
    }
    case kFortsInstrumentstateReplInstrumentState:
        return target(kFortsInstrumentstateReplInstrumentStateIsinId, kFortsInstrumentstateReplInstrumentStateReplAct);
    case kFortsRefdataReplFutSessContents:
        return target(kFortsRefdataReplFutSessContentsIsinId, kFortsRefdataReplFutSessContentsReplAct);
    case kFortsRefdataReplFutInstruments:
        return target(kFortsRefdataReplFutInstrumentsIsinId, kFortsRefdataReplFutInstrumentsReplAct);
    case kFortsTradeReplHeartbeat:
    case kFortsPosReplInfo:
    case kFortsUserorderbookReplInfo:
    case kFortsRefdataReplSession:
    case kFortsSessionstateReplSessionState:
    case kFortsTradeReplSysEvents:
    case kFortsDealsReplSysEvents:
    case kFortsPartReplSysEvents:
    case kFortsAggrReplSysEvents:
    case kFortsRefdataReplSysMessages:
        return true;
    default:
        return false;
    }
}
std::string configured_risk_fields(const TradingHostConfig& config) {
    const auto& risk = config.orders.risk;
    std::string result = "{\"max_quantity\":" + std::to_string(risk.max_quantity) +
                         ",\"max_open_orders\":" + std::to_string(risk.max_open_orders) +
                         ",\"max_notional_scaled\":" + std::to_string(risk.max_notional_scaled) + ",\"instruments\":[";
    bool first = true;
    for (const auto isin : config.isin_ids) {
        if (!first)
            result += ',';
        first = false;
        const auto notional = risk.max_notional_by_isin.find(isin), position = risk.max_position_by_isin.find(isin);
        result += "{\"isin_id\":" + std::to_string(isin) + ",\"max_quote_notional_scaled\":" +
                  (notional == risk.max_notional_by_isin.end() ? "null" : std::to_string(notional->second)) +
                  ",\"max_position\":" +
                  (position == risk.max_position_by_isin.end() ? "null" : std::to_string(position->second)) + "}";
    }
    return result + "]}";
}
} // namespace
CgateTradingHost::CgateTradingHost(TradingHostConfig config)
    : config_(std::move(config)), owner_(std::this_thread::get_id()),
      journal_(config_.journal_path, identity_path(config_)), session_(session_config()) {
    if (config_.isin_ids.empty())
        throw std::invalid_argument("at least one trading instrument is required");
    if (config_.session.allow_orders) {
        if (config_.orders.login_from.empty() || !config_.orders.ext_id_range_configured)
            throw std::invalid_argument("--allow-orders requires explicit --login-env and --ext-id-range");
        const auto& risk = config_.orders.risk;
        if (!risk.quantity_configured || !risk.open_orders_configured)
            throw std::invalid_argument("--allow-orders requires explicit "
                                        "--max-quantity and --max-open-orders");
        for (const auto isin : config_.isin_ids)
            if (!risk.max_notional_by_isin.contains(isin) || !risk.max_position_by_isin.contains(isin))
                throw std::invalid_argument("--allow-orders requires --max-notional ISIN=QUOTE_VALUE and "
                                            "--max-position ISIN=N for every target instrument");
    }
    auto orders = config_.orders;
    const auto reservations = journal_.reservations();
    orders.next_ext_id = std::max({orders.next_ext_id, orders.ext_id_begin, reservations.next_ext_id});
    orders.next_user_id = std::max(orders.next_user_id, reservations.next_user_id);
    orders_ = std::make_unique<OrderManager>(
        std::move(orders),
        [this](const auto& command, auto id) {
            const bool cancel = command.command_kind == tr::Plaza2TradeCommandKind::DelOrder ||
                                command.command_kind == tr::Plaza2TradeCommandKind::DelUserOrders;
            if (!journal_.user_id_reserved(id) || (!log_error_.empty() && !cancel))
                return cg::Plaza2PublisherMessageResult{
                    .certainty = cg::Plaza2SubmissionCertainty::DefinitelyNotSent,
                    .validation_error = {.code = cg::Plaza2ErrorCode::RuntimeCallFailed,
                                         .message = "storage failure or command ID not durably reserved"}};
            return session_.post_command(command, id);
        },
        [this](auto isin) {
            // Clearing-rejected cancels share this exchange readiness check.
            // Storage protection blocks entry through the kill/send guards.
            return !rebuilding_ &&
                   std::find(config_.isin_ids.begin(), config_.isin_ids.end(), isin) != config_.isin_ids.end() &&
                   moex::connector_host::order_entry_ready(session_, isin, 0, true);
        },
        [this](auto isin) -> std::optional<plaza2::private_state::FutureSessionTerms> {
            const auto& data = session_.private_state();
            const auto life = data.refdata_lifenum();
            const auto sess = current_session_id(data);
            return life && sess ? data.find_future_session_terms(isin, sess, *life) : std::nullopt;
        },
        [this](auto kind, auto fields) noexcept { log_event(kind, fields); },
        [this](auto isin) -> std::optional<std::int64_t> {
            const auto& data = session_.private_state();
            const auto pos =
                std::find_if(data.stream_health().begin(), data.stream_health().end(),
                             [](const auto& row) { return row.stream_code == gen::StreamCode::kFortsPosRepl; });
            if (pos == data.stream_health().end() || !pos->online || !pos->snapshot_complete)
                return std::nullopt;
            const auto* row = data.find_position(config_.orders.broker_code + config_.orders.client_code, isin,
                                                 config_.orders.client_code.empty() ? 1 : 2);
            return row ? row->xpos : 0;
        });
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
        // Preserve only the latest ID cursor while a failed writer cannot log.
        if (kind == "reservation")
            identifier_reservation_ = fields;
        const auto named = kind == "cgate_state" || kind == "cgate_operation" || kind == "listener_recovery"
                               ? named_transport_fields(fields)
                               : std::string(fields);
        if (kind == "cgate_state" || kind == "recovery" || kind == "listener_recovery")
            std::cerr << kind << ": " << named << '\n';
        if (journal_failed_)
            return; // Never buffer ordinary traffic after a failed writer.
        journal_.append(kind, named);
        if (kind == "cgate_state" && !stopped_)
            observe_link(session_.runtime_health());
    } catch (const std::exception& error) {
        storage_failure(error.what());
    }
}
void CgateTradingHost::observe_link(const tr::Plaza2TransportHealth& health) {
    if (journal_failed_)
        return;
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
        if (event.stream_code == gen::StreamCode::kFortsRefdataRepl) {
            // The callback precedes projection. Drain the preceding committed
            // message updates before another transaction or listener reset.
            if (event.kind != cg::Plaza2ListenerEventKind::StreamData)
                observe_exchange_messages();
            if (event.kind == cg::Plaza2ListenerEventKind::LifeNum)
                exchange_message_commit_ = UINT64_MAX;
            if (event.kind == cg::Plaza2ListenerEventKind::Open && !event.text_value.empty()) {
                std::cerr << "moexctl: " << event.text_value << '\n';
                log_event("exchange_messages_unavailable", "{\"warning\":" + json_string(event.text_value) + "}");
            }
        }
        if (journal_failed_)
            return;
        const bool replication = event.stream_code != cg::kNoStreamCode;
        if (replication && event.kind == cg::Plaza2ListenerEventKind::StreamData &&
            !journal_replication_row(event, config_))
            return;
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
            ",\"text\":" + json_string(event.text_value);
        if (!replication)
            fields += ",\"payload_hex\":" + json_string(tr::bytes_to_hex(event.raw_payload));
        fields += ",\"fields\":[";
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
        storage_failure(error.what());
    }
}
void CgateTradingHost::observe_exchange_messages() {
    const auto& data = session_.private_state();
    const auto health = data.stream_health();
    const auto ref = std::find_if(health.begin(), health.end(),
                                  [](const auto& row) { return row.stream_name == "FORTS_REFDATA_REPL"; });
    if (ref == health.end() || !ref->online || !ref->snapshot_complete ||
        ref->last_commit_sequence == exchange_message_commit_)
        return;
    exchange_message_commit_ = ref->last_commit_sequence;
    decltype(exchange_message_revisions_) current;
    for (const auto& message : data.system_messages()) {
        const auto revision = std::pair{message.source.lifenum, message.repl_rev};
        const auto previous = exchange_message_revisions_.find(message.repl_id);
        if (previous == exchange_message_revisions_.end() || previous->second != revision) {
            const auto fields = exchange_message_fields(message);
            log_event("exchange_message", fields);
            std::cerr << "exchange_message: " << fields << '\n';
            if (exchange_messages_.size() == 20)
                exchange_messages_.erase(exchange_messages_.begin());
            exchange_messages_.push_back(message);
        }
        current.emplace(message.repl_id, revision);
    }
    exchange_message_revisions_ = std::move(current);
}
void CgateTradingHost::storage_failure(std::string_view error, bool writer_failed) {
    if (journal_failed_ || (!log_error_.empty() && !writer_failed))
        return;
    journal_failed_ = writer_failed;
    log_error_ = error;
    if (orders_)
        orders_->set_kill_switch(true);
    std::cerr << "moexctl: storage protection; cancel-only mode with durably reserved command IDs; " << log_error_
              << '\n';
    if (!journal_failed_)
        log_event("storage_guard", "{\"error\":" + json_string(log_error_) + "}");
}
std::string CgateTradingHost::check_storage_space() {
    const auto now = config_.session.recovery_now ? config_.session.recovery_now() : OrderManager::Clock::now();
    next_space_check_ = now + std::chrono::minutes(1);
    std::uintmax_t available = UINTMAX_MAX;
    for (const auto& file : {config_.journal_path, identity_path(config_)}) {
        std::error_code error;
        const auto space = std::filesystem::space(file.parent_path().empty() ? "." : file.parent_path(), error);
        if (error)
            return "cannot check journal/identity storage capacity: " + error.message();
        available =
            std::min(available, config_.storage_space_probe ? config_.storage_space_probe(file) : space.available);
    }
    constexpr std::uintmax_t minimum = 64 * 1024 * 1024;
    if (available < minimum)
        return "journal/identity storage has less than the required 64 MiB free";
    log_event("storage_capacity", "{\"available_bytes\":" + std::to_string(available) +
                                      ",\"minimum_bytes\":" + std::to_string(minimum) + "}");
    return {};
}
cg::Plaza2Error CgateTradingHost::start() {
    assert_owner();
    if (stopped_)
        return {.code = cg::Plaza2ErrorCode::InvalidConfiguration,
                .message = "trading host has stopped; create a fresh host to restart"};
    if (const auto error = check_storage_space(); !error.empty()) {
        storage_failure(error, false);
        return {.code = cg::Plaza2ErrorCode::InvalidConfiguration, .message = error};
    }
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
    auto risk = configured_risk_fields(config_);
    risk.insert(risk.size() - 1, ",\"kill_switch\":" + std::string(config_.orders.risk.kill_switch ? "true" : "false"));
    log_event(
        "startup",
        "{\"product\":\"MoexConnector\",\"version\":\"1.0.0\",\"instance_id\":" + json_string(session.publisher_name) +
            ",\"source_git_sha\":" + json_string(config_.source_git_sha) +
            ",\"binary_sha256\":" + json_string(config_.binary_sha256) + ",\"router\":" + json_string(router) +
            ",\"ext_id_begin\":" + std::to_string(config_.orders.ext_id_begin) +
            ",\"ext_id_end\":" + std::to_string(config_.orders.ext_id_end) +
            ",\"login_from\":\"[REDACTED]\"" +
            ",\"rate\":" + std::to_string(config_.orders.max_commands_per_second) +
            ",\"allow_orders\":" + (session.allow_orders ? "true" : "false") + ",\"risk\":" + risk + ",\"urls\":[" +
            urls + "],\"env_settings\":" + json_string(masked_settings(session.runtime.env_open_settings)) +
            ",\"clock_offset_us\":" + (config_.clock_offset_us ? std::to_string(*config_.clock_offset_us) : "null") +
            ",\"clock_offset_source\":" +
            json_string(config_.clock_offset_us ? "operator measurement" : "unavailable") + "}");
    return session_.start();
}
cg::Plaza2Error CgateTradingHost::poll() {
    assert_owner();
    const auto now = config_.session.recovery_now ? config_.session.recovery_now() : OrderManager::Clock::now();
    if (log_error_.empty() && now >= next_space_check_)
        if (const auto error = check_storage_space(); !error.empty())
            storage_failure(error, false);
    dispatch_commands();
    const auto error = session_.poll(orders_->queued() == 0);
    observe_exchange_messages();
    try {
        observe_link(session_.runtime_health());
    } catch (const std::exception& log_error) {
        storage_failure(log_error.what());
    }
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
    // Use the startup snapshot barrier again after a lost delta batch or either
    // private order stream disconnects. Keep logical orders and correlations;
    // the current committed snapshots reconcile their identities and exposure.
    if (!rebuilding_ &&
        (changes.resync_required || changes.regular_trade_history_truncated || !trade_online || !user_book_online)) {
        rebuilding_ = true;
        log_event("private_history_gap", "{\"recovering\":true}");
    }
    if (rebuilding_) {
        // The bootstrap barrier reconciles full committed snapshots. Delta
        // accumulation while waiting for USERORDERBOOK serves no consumer.
        if (const auto sync_error = session_.synchronize_order_book(); sync_error)
            return sync_error;
        if (trade_online && user_book_online && session_.order_book_snapshot_ready()) {
            orders_->observe_orders(data.own_orders(), true);
            orders_->observe_trades(data.own_trades());
            orders_->reconcile_snapshot(data.own_orders(), config_.utc_now ? config_.utc_now() : utc_seconds());
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
    if (!journal_failed_) {
        try {
            journal_.flush_if_due();
        } catch (const std::exception& storage_error) {
            storage_failure(storage_error.what());
        }
    }
    dispatch_commands();
    return error;
}
cg::Plaza2Error CgateTradingHost::stop() {
    assert_owner();
    if (stopped_)
        return stop_error_;
    stopped_ = true;
    orders_->set_kill_switch(true);
    log_event("shutdown", "{}");
    // Storage failures must never leave gateway handles open. Close first,
    // then attempt to flush the shutdown and close records together.
    const auto session_error = session_.stop();
    try {
        journal_.flush();
    } catch (const std::exception& error) {
        storage_failure(error.what());
    }
    stop_error_ = log_error_.empty()
                      ? session_error
                      : cg::Plaza2Error{.code = cg::Plaza2ErrorCode::RuntimeCallFailed,
                                        .message = "journal storage failure during shutdown: " + log_error_};
    return stop_error_;
}
std::string CgateTradingHost::place(OrderRequest request) {
    assert_owner();
    if (!log_error_.empty())
        return "storage failure; cancel-only mode blocks Add";
    if (request.type == tr::Plaza2TradeOrderType::Ioc && !order_entry_ready(session_, request.isin_id))
        return "IOC requires continuous trading; opening-auction IOC is prohibited";
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
    if (!log_error_.empty())
        return "storage failure; cancel-only mode blocks Move";
    const auto order = orders_->orders().find(std::string(key));
    if (order != orders_->orders().end() && !order_entry_ready(session_, order->second.request.isin_id))
        return "Move requires continuous trading; opening-auction Move is prohibited";
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
    const auto now = config_.session.recovery_now ? config_.session.recovery_now() : OrderManager::Clock::now();
    if (!log_error_.empty())
        orders_->set_kill_switch(true);
    orders_->poll(now, config_.utc_now ? config_.utc_now() : utc_seconds());
}
void CgateTradingHost::set_kill_switch(bool enabled) {
    assert_owner();
    if (!enabled && !log_error_.empty())
        throw std::invalid_argument("storage failure; cancel-only mode keeps the kill switch on");
    orders_->set_kill_switch(enabled);
}
std::string CgateTradingHost::storage_ok() {
    assert_owner();
    if (stopped_)
        return "trading host has stopped";
    if (const auto error = check_storage_space(); !error.empty()) {
        storage_failure(error, false);
        return error;
    }
    if (log_error_.empty())
        return {};
    try {
        journal_.flush();
        if (!identifier_reservation_.empty())
            journal_.append("reservation", identifier_reservation_);
        journal_.append("storage_recovered", "{\"kill_switch\":true}");
        journal_.flush();
    } catch (const std::exception& error) {
        storage_failure(error.what());
        return error.what();
    }
    journal_failed_ = false;
    log_error_.clear();
    orders_->set_kill_switch(true);
    return {};
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
        ",\"journal_failed\":" + (journal_failed_ ? "true" : "false") +
        ",\"cancel_only\":" + (log_error_.empty() ? "false" : "true") +
        ",\"operator_action_required\":" + (orders_->operator_action_required() ? "true" : "false") +
        ",\"transport\":" + transport_fields(session_, config_.session) +
        ",\"configuration\":{\"allow_orders\":" + (config_.session.allow_orders ? "true" : "false") +
        ",\"rate\":" + std::to_string(config_.orders.max_commands_per_second) +
        ",\"risk\":" + configured_risk_fields(config_) + "}" + ",\"instruments\":[";
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
    result += "],\"exchange_messages\":[";
    first = true;
    for (const auto& message : exchange_messages_) {
        if (!first)
            result += ',';
        first = false;
        result += exchange_message_fields(message);
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
