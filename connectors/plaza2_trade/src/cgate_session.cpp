#include "moex/plaza2_trade/cgate_session.hpp"
#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"
#include "moex/plaza2/cgate/cgate_logging.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#include <utility>

namespace moex::plaza2_trade {
namespace {
namespace cg = moex::plaza2::cgate;
using cg::Plaza2Error;
using cg::Plaza2ErrorCode;
using moex::plaza2::generated::StreamCode;
constexpr std::uint32_t Closed = 0, Error = 1, Opening = 2, Active = 3, Unsupported = 131074, Timeout = 131075;
Plaza2Error invalid(std::string message) {
    return {.code = Plaza2ErrorCode::InvalidConfiguration, .message = std::move(message)};
}
void replace(std::string& value, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = value.find(from, at)) != std::string::npos) {
        value.replace(at, from.size(), to);
        at += to.size();
    }
}
std::optional<std::string> secret(const cg::Plaza2CredentialConfig& config) {
    if (config.source == cg::Plaza2CredentialSource::None)
        return std::string{};
    if (config.source == cg::Plaza2CredentialSource::Env) {
        const auto* value = std::getenv(config.env_var.c_str());
        return value && *value ? std::optional<std::string>(value) : std::nullopt;
    }
    std::ifstream input(config.file_path);
    std::string value;
    if (!std::getline(input, value) || value.empty())
        return std::nullopt;
    return value;
}
std::string json_quote(std::string_view value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '\"')
            out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        if (c == '\r') {
            out += "\\r";
            continue;
        }
        if (c == '\t') {
            out += "\\t";
            continue;
        }
        if (static_cast<unsigned char>(c) < 32)
            continue;
        out += c;
    }
    return out + '\"';
}
std::uint64_t ns(std::chrono::steady_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}
class Replies final : public cg::Plaza2ListenerEventHandler {
  public:
    struct Pending {
        Plaza2TradeCommandKind kind;
        std::chrono::steady_clock::time_point deadline;
    };
    std::map<std::uint32_t, Pending> pending;
    void expire(std::chrono::steady_clock::time_point now) {
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->second.deadline > now) {
                ++it;
                continue;
            }
            if (log)
                log("reply_tracking_expired", "{\"user_id\":" + std::to_string(it->first) + "}");
            it = pending.erase(it);
        }
    }
    std::vector<CgateSession::ReplyEvent> events;
    bool external_owner{};
    std::function<void(std::uint32_t)> penalty;
    std::function<void(std::string_view, std::string_view)> log;
    Plaza2Error flood_reply(std::span<const std::byte> payload) {
        Plaza2TradeValidationResult validation;
        const auto decoded = Plaza2TradeCodec{}.decode_reply(99, payload, validation);
        if (!validation.ok() || !decoded.penalty_remain || *decoded.penalty_remain < 0)
            return {.code = Plaza2ErrorCode::DecodeFailed, .message = "malformed flood reply"};
        if (penalty)
            penalty(*decoded.penalty_remain);
        return {};
    }
    Plaza2Error on_plaza2_listener_event(const cg::Plaza2ListenerEvent& event) override {
        if (event.kind != cg::Plaza2ListenerEventKind::StreamData && event.kind != cg::Plaza2ListenerEventKind::Timeout)
            return {};
        auto found = pending.find(event.user_id);
        if (found == pending.end()) {
            if (log)
                log("unknown_reply", "{\"user_id\":" + std::to_string(event.user_id) + "}");
            if (external_owner && event.kind == cg::Plaza2ListenerEventKind::StreamData) {
                // The owner may retain an uncertain Move after this bounded
                // tracking map expires. Forward evidence without recreating
                // a Session submission or assuming it is a new command.
                if (event.message_id == 99)
                    if (const auto error = flood_reply(event.raw_payload); error)
                        return error;
                events.push_back({.user_id = event.user_id,
                                  .message_id = event.message_id,
                                  .raw_payload = {event.raw_payload.begin(), event.raw_payload.end()}});
            }
            return {};
        }
        events.push_back({.user_id = event.user_id,
                          .message_id = event.message_id,
                          .command_kind = found->second.kind,
                          .timed_out = event.kind == cg::Plaza2ListenerEventKind::Timeout,
                          .raw_payload = {event.raw_payload.begin(), event.raw_payload.end()}});
        const auto kind = found->second.kind;
        if (event.message_id == 99) {
            pending.erase(found);
            if (const auto error = flood_reply(event.raw_payload); error)
                return error;
        }
        const auto expected = kind == Plaza2TradeCommandKind::AddOrder    ? 179
                              : kind == Plaza2TradeCommandKind::DelOrder  ? 177
                              : kind == Plaza2TradeCommandKind::MoveOrder ? 176
                                                                          : 186;
        if (event.message_id != 99 && (event.kind == cg::Plaza2ListenerEventKind::Timeout ||
                                       event.message_id == expected || event.message_id == 100))
            pending.erase(found);
        return {};
    }
};
} // namespace
std::string_view plaza2_recovery_wait_state_name(Plaza2RecoveryWaitState state) noexcept {
    switch (state) {
    case Plaza2RecoveryWaitState::None:
        return "None";
    case Plaza2RecoveryWaitState::WaitingForRouter:
        return "WaitingForRouter";
    case Plaza2RecoveryWaitState::WaitingForPlaza:
        return "WaitingForPlaza";
    case Plaza2RecoveryWaitState::WaitingForService:
        return "WaitingForService";
    case Plaza2RecoveryWaitState::RecoveringBootstrap:
        return "RecoveringBootstrap";
    }
    return "None";
}
struct CgateSession::Impl {
    struct Listener {
        CgateStreamConfig config;
        cg::Plaza2Listener object;
        cg::Plaza2ListenerEventHandler* handler{};
        std::chrono::steady_clock::time_point retry{};
        bool scheme_incompatible{};
        std::optional<std::uint32_t> observed_state;
    };
    CgateSessionConfig config;
    cg::Plaza2RuntimeProbeReport probe;
    cg::Plaza2Env env;
    cg::Plaza2Connection connection;
    cg::Plaza2Publisher publisher;
    moex::plaza2::private_state::Plaza2PrivateStateProjector projection;
    cg::Plaza2PrivateStateBridge bridge{projection};
    cg::Plaza2Aggr20BookProjector book;
    cg::Plaza2Aggr20ListenerBridge aggr{book, 0};
    cg::Plaza2PublicDealsBridge deals;
    Replies replies;
    cg::Plaza2PublisherRateGate rate;
    std::vector<Listener> listeners;
    Plaza2RecoveryStatus recovery;
    std::chrono::steady_clock::time_point connection_retry{}, publisher_retry{};
    std::optional<std::chrono::steady_clock::time_point> key_check_warning_time;
    std::string app_name, last_error, credentials, software_key;
    bool initialized{}, streams_created{}, connection_was_active{};
    std::optional<CgateStreamConfig> deferred_trade;
    std::optional<Plaza2TradeReplayAnchor> anchor;
    std::optional<std::uint32_t> observed_connection, observed_publisher;
    explicit Impl(CgateSessionConfig c)
        : config(std::move(c)), book({}, config.market_data_isin_ids),
          deals(config.public_deals_target_isin_id, config.aggr20_target_session_id),
          rate(config.publisher_messages_per_second) {
        replies.penalty = [this](std::uint32_t ms) {
            if (config.publisher_rate_owner == PublisherRateOwner::Session)
                rate.penalize(now_ms(), ms);
        };
        replies.log = config.event_log;
        replies.external_owner = config.publisher_rate_owner == PublisherRateOwner::External;
    }
    auto now() const {
        return config.recovery_now ? config.recovery_now() : std::chrono::steady_clock::now();
    }
    std::uint64_t now_ms() const {
        return config.publisher_now_ms ? config.publisher_now_ms() : ns(now()) / 1000000;
    }
    void wait_for_recovery() const {
        // A zero process timeout suppresses CGate's wait, but a disconnected
        // owner must still yield between retries instead of consuming a core.
        const auto delay = std::chrono::milliseconds(config.process_timeout_ms ? config.process_timeout_ms : 50);
        std::this_thread::sleep_for(delay);
    }
    void log(std::string_view kind, std::string fields) {
        if (config.event_log)
            config.event_log(kind, fields);
    }
    void observe(std::optional<std::uint32_t>& previous, std::uint32_t state, std::string_view object,
                 StreamCode stream = cg::kNoStreamCode) {
        if (previous && *previous == state)
            return;
        log("cgate_state", "{\"object\":" + json_quote(object) +
                               ",\"stream_code\":" + std::to_string(static_cast<std::uint32_t>(stream)) +
                               ",\"from\":" + (previous ? std::to_string(*previous) : "null") +
                               ",\"to\":" + std::to_string(state) + "}");
        previous = state;
    }
    void operation(std::string_view object, std::string_view operation, const Plaza2Error& error,
                   StreamCode stream = cg::kNoStreamCode) {
        log("cgate_operation", "{\"object\":" + json_quote(object) +
                                   ",\"stream_code\":" + std::to_string(static_cast<std::uint32_t>(stream)) +
                                   ",\"operation\":" + json_quote(operation) +
                                   ",\"runtime_code\":" + std::to_string(error.runtime_code) +
                                   ",\"error\":" + json_quote(error.message) + "}");
    }
    Plaza2Error open_connection() {
        const auto error = connection.open(config.connection_open_settings);
        operation("connection", "open", error);
        if (error.runtime_code == Unsupported) {
            // CGate cg_conn_open documents this result as a user-key check
            // failure, with "Certificate check failed" in its client log.
            recovery.key_check_failed = true;
            const auto current = now();
            if (!key_check_warning_time || current - *key_check_warning_time >= std::chrono::minutes(1)) {
                key_check_warning_time = current;
                log("cgate_key_check_failed", "{\"operation\":\"cg_conn_open\",\"runtime_code\":131074,"
                                              "\"message\":\"CGate user-key verification failed; inspect Certificate "
                                              "check failed in the CGate client log\"}");
                std::cerr << "cgate_key_check_failed: CGate could not verify the user key "
                             "(cg_conn_open, CG_ERR_UNSUPPORTED, 131074). Check the CGate client log for "
                             "Certificate check failed, the key configuration, and router upstream connectivity. "
                             "Connection retries continue.\n";
            }
        }
        return error;
    }
    void close_connection() {
        if (!connection.is_created())
            return;
        const auto error = connection.close();
        operation("connection", "close", error);
        if (!error)
            observe(observed_connection, Closed, "connection");
    }
    void close_publisher() {
        if (!publisher.is_created())
            return;
        const auto error = publisher.close();
        operation("publisher", "close", error);
        if (!error)
            observe(observed_publisher, Closed, "publisher");
    }
    void close_listener(Listener& listener) {
        const auto error = listener.object.close();
        operation("listener", "close", error, listener.config.stream_code);
        if (!error)
            observe(listener.observed_state, Closed, "listener", listener.config.stream_code);
    }
    std::string render(std::string value) const {
        replace(value, "${MOEX_PLAZA2_CREDENTIALS}", credentials);
        replace(value, "${PLAZA2_CREDENTIALS}", credentials);
        replace(value, "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}", software_key);
        if (!config.credentials.env_var.empty())
            replace(value, "${" + config.credentials.env_var + "}", credentials);
        if (!config.software_key.env_var.empty())
            replace(value, "${" + config.software_key.env_var + "}", software_key);
        replace(value, "|FILE|scheme/forts_scheme.ini|", "|FILE|" + probe.layout.scheme_path.string() + "|");
        const auto start = value.find("ini=");
        if (start != std::string::npos) {
            const auto begin = start + 4, end = value.find(';', begin);
            auto path = std::filesystem::path(value.substr(begin, end - begin));
            if (!path.is_absolute()) {
                auto candidate = probe.layout.config_dir / path;
                if (!std::filesystem::exists(candidate))
                    candidate = probe.layout.config_dir / path.filename();
                value.replace(begin, end == std::string::npos ? value.size() - begin : end - begin, candidate.string());
            }
        }
        return value;
    }
    void waiting(Plaza2Error error, Plaza2RecoveryWaitState state, std::string service) {
        last_error = error.message;
        const bool changed = recovery.operation != Plaza2SessionOperation::Recovering || recovery.wait_state != state ||
                             recovery.involved_service != service;
        if (recovery.operation != Plaza2SessionOperation::Recovering) {
            recovery.wait_start_time_ns = ns(now());
            ++recovery.transitions;
        }
        if (changed)
            log("recovery", "{\"state\":\"" + std::string(plaza2_recovery_wait_state_name(state)) + "\"}");
        recovery.operation = Plaza2SessionOperation::Recovering;
        recovery.wait_state = state;
        recovery.cause = std::move(error);
        recovery.involved_service = std::move(service);
        recovery.error_time_ns = ns(now());
    }
    Plaza2Error start() {
        if (initialized)
            return invalid("CGate session is already started");
        if ((config.publisher_rate_owner == PublisherRateOwner::Session && !rate.valid()) ||
            config.process_timeout_ms > 50 || config.recovery_retry_interval < std::chrono::seconds(1))
            return invalid("CGate session requires rate 1..3000, idle timeout <=50ms and reopen delay >=1s");
        if (config.read_only_market_data &&
            (config.allow_orders || !config.publisher_settings.empty() || !config.p2mqreply_settings.empty()))
            return invalid("read-only CGate session cannot configure order entry");
        if (config.connection_settings.empty() || config.runtime.runtime_root.empty())
            return invalid("CGate runtime and connection are required");
        if (config.private_streams.empty())
            return invalid("at least one private replication stream is required");
        if (!config.read_only_market_data && config.publisher_settings.empty())
            return invalid("publisher settings are required for trading session");
        probe = cg::Plaza2RuntimeProbe::probe(config.runtime);
        if (probe.compatibility == cg::Plaza2Compatibility::Incompatible || !probe.runtime_library_loadable)
            return invalid("CGate runtime cannot be loaded");
        if (!config.read_only_market_data && !probe.trading_capable)
            return invalid("CGate runtime is missing required trading symbols");
        if (config.mode == CgateSessionMode::OfflineFake && !probe.fake_runtime_marker_present)
            return invalid("OfflineFake requires the test CGate runtime marker");
        const auto k = secret(config.software_key);
        if (!k)
            return invalid("configured CGate software key source is missing");
        credentials = secret(config.credentials).value_or("");
        software_key = *k;
        auto runtime = config.runtime;
        runtime.listener_event_log = config.listener_event_log;
        runtime.env_open_settings = render(runtime.env_open_settings);
        if (config.mode == CgateSessionMode::Live)
            try {
                cg::validate_cgate_logging(runtime.env_open_settings, probe.layout.config_dir);
            } catch (const std::exception& error) {
                return invalid(error.what());
            }
        if (auto error = env.open(runtime); error)
            return error;
        const auto settings = render(config.connection_settings);
        const auto app = settings.find("app_name=");
        if (app != std::string::npos)
            app_name = settings.substr(app + 9, settings.find(';', app + 9) - (app + 9));
        if (auto error = connection.create(env, settings); error)
            return error;
        initialized = true;
        recovery.operation = Plaza2SessionOperation::Starting;
        ++recovery.generation;
        log("runtime_identity", "{\"library_sha256\":\"" + probe.runtime_library_sha256 + "\",\"scheme_sha256\":\"" +
                                    probe.runtime_scheme_sha256 + "\"}");
        // OPEN is asynchronous; ACTIVE and listener creation belong to subsequent polls.
        if (auto error = open_connection(); error) {
            waiting(error, Plaza2RecoveryWaitState::WaitingForRouter, "connection");
            connection_retry = now() + config.recovery_retry_interval;
        }
        return {};
    }
    Plaza2Error add_listener(CgateStreamConfig c, cg::Plaza2ListenerEventHandler& handler) {
        Listener listener;
        listener.config = std::move(c);
        listener.handler = &handler;
        if (auto error = listener.object.create(connection, listener.config.stream_code,
                                                render(listener.config.settings), &handler);
            error)
            return error;
        listeners.push_back(std::move(listener));
        return {};
    }
    Plaza2Error create_streams() {
        auto streams = config.private_streams;
        if (config.status_streams.empty()) {
            config.status_streams = {
                {StreamCode::kFortsSessionstateRepl, "p2repl://FORTS_SESSIONSTATE_REPL", {}},
                {StreamCode::kFortsInstrumentstateRepl, "p2repl://FORTS_INSTRUMENTSTATE_REPL", {}}};
        }
        streams.insert(streams.end(), config.status_streams.begin(), config.status_streams.end());
        std::vector<StreamCode> codes;
        for (auto& s : streams)
            codes.push_back(s.stream_code);
        if (auto error = bridge.reset(codes); error)
            return error;
        if (auto error = bridge.begin_run(); error)
            return error;
        listeners.reserve(streams.size() + 3);
        for (auto& stream : streams) {
            if (config.trade_replay_from_pos_anchor && stream.stream_code == StreamCode::kFortsTradeRepl) {
                deferred_trade = stream;
                continue;
            }
            if (auto error = add_listener(stream, bridge); error)
                return error;
        }
        if (!config.aggr20_stream.settings.empty())
            if (auto error = add_listener(config.aggr20_stream, aggr); error)
                return error;
        if (!config.public_deals_stream.settings.empty())
            if (auto error = add_listener(config.public_deals_stream, deals); error)
                return error;
        if (!config.read_only_market_data) {
            if (auto error = publisher.create(connection, render(config.publisher_settings)); error)
                return error;
            CgateStreamConfig reply{cg::kNoStreamCode,
                                    config.p2mqreply_settings.empty() ? "p2mqreply://;ref=" + config.publisher_name
                                                                      : config.p2mqreply_settings,
                                    config.p2mqreply_open_settings};
            if (auto error = add_listener(std::move(reply), replies); error)
                return error;
        }
        streams_created = true;
        return {};
    }
    bool trading_ready(const Plaza2TradeEncodedCommand& command) const {
        for (auto code :
             {StreamCode::kFortsRefdataRepl, StreamCode::kFortsSessionstateRepl, StreamCode::kFortsInstrumentstateRepl})
            if (!stream_online(code))
                return false;
        const auto day = projection.current_session_id();
        if (!day)
            return false;
        const auto sessions = projection.sessions();
        if (std::none_of(sessions.begin(), sessions.end(), [day](const auto& row) {
                return row.sess_id == day && row.has_current_status && row.current_status == 1;
            }))
            return false;
        const auto instruments = projection.instruments();
        return std::any_of(instruments.begin(), instruments.end(), [=](const auto& row) {
            const bool auction_day_add = row.current_status == 6 &&
                                         command.command_kind == Plaza2TradeCommandKind::AddOrder &&
                                         command.order_type == Plaza2TradeOrderType::Limit;
            return row.kind == plaza2::private_state::InstrumentKind::kFuture && !row.is_spread &&
                   row.isin_id == command.isin_id && row.sess_id == day && row.current_session_member &&
                   row.has_current_status && (row.current_status == 1 || auction_day_add);
        });
    }
    bool stream_online(StreamCode code) const {
        const auto health = projection.stream_health();
        const auto found =
            std::find_if(health.begin(), health.end(), [&](const auto& s) { return s.stream_code == code; });
        return found != health.end() && found->online && found->snapshot_complete;
    }
    const plaza2::private_state::StreamHealthSnapshot* pos_anchor_health() const {
        const auto health = projection.stream_health();
        const auto found = std::find_if(health.begin(), health.end(),
                                        [](const auto& s) { return s.stream_code == StreamCode::kFortsPosRepl; });
        return found != health.end() && found->online && found->snapshot_complete && found->last_trades_rev > 0 &&
                       found->last_trades_lifenum > 0
                   ? &*found
                   : nullptr;
    }
    bool anchored_trade_ready() const {
        const auto* pos = pos_anchor_health();
        return anchor && pos && anchor->trades_rev == pos->last_trades_rev &&
               anchor->trades_lifenum == pos->last_trades_lifenum && stream_online(StreamCode::kFortsTradeRepl);
    }
    const plaza2::private_state::StreamHealthSnapshot* userbook_anchor_health() const {
        const auto health = projection.stream_health();
        const auto found = std::find_if(health.begin(), health.end(), [](const auto& s) {
            return s.stream_code == StreamCode::kFortsUserorderbookRepl;
        });
        return found != health.end() && found->online && found->snapshot_complete &&
                       found->periodic_snapshot_consistent && found->last_trades_rev >= 0 &&
                       found->last_trades_lifenum > 0
                   ? &*found
                   : nullptr;
    }
    bool order_book_snapshot_ready() const {
        const auto* book = userbook_anchor_health();
        const auto life = projection.stream_lifenum(StreamCode::kFortsTradeRepl);
        return book && anchor && anchored_trade_ready() && life &&
               *life == static_cast<std::uint64_t>(anchor->trades_lifenum) &&
               book->last_trades_lifenum == anchor->trades_lifenum && book->last_trades_rev <= anchor->orders_rev;
    }
    Plaza2Error open_anchored_trade(bool refresh_orders = false) {
        if (!deferred_trade)
            return {};
        const auto* found = pos_anchor_health();
        if (!found)
            return {};
        const bool same_pos = anchor && anchor->trades_rev == found->last_trades_rev &&
                              anchor->trades_lifenum == found->last_trades_lifenum;
        if (same_pos && !refresh_orders)
            return {};
        const auto* book = userbook_anchor_health();
        if (!book || book->last_trades_lifenum != found->last_trades_lifenum)
            return {};
        const Plaza2TradeReplayAnchor target{found->last_trades_rev, found->last_trades_lifenum,
                                             found->last_server_time, book->last_trades_rev};
        if (same_pos && anchor->orders_rev >= target.orders_rev)
            return {};
        if (anchor) {
            auto old = std::find_if(listeners.begin(), listeners.end(),
                                    [](const auto& l) { return l.config.stream_code == StreamCode::kFortsTradeRepl; });
            if (old != listeners.end()) {
                close_listener(*old);
                static_cast<void>(old->object.destroy());
                listeners.erase(old);
            }
        }
        auto trade = *deferred_trade;
        replace(trade.open_settings, "${POS_TRADES_REV}", std::to_string(target.trades_rev));
        replace(trade.open_settings, "${POS_TRADES_LIFENUM}", std::to_string(target.trades_lifenum));
        if (trade.open_settings.empty())
            trade.open_settings = "mode=snapshot+online";
        trade.open_settings += ";lifenum=" + std::to_string(target.trades_lifenum) +
                               ";rev.orders_log=" + std::to_string(target.orders_rev) +
                               ";rev.deal=" + std::to_string(target.trades_rev) +
                               ";rev.heart_beat=" + std::to_string(target.trades_rev);
        if (auto error = add_listener(std::move(trade), bridge); error)
            return error;
        anchor = target;
        return {};
    }
    void invalidate(Listener& listener) {
        static_cast<void>(listener.handler->on_plaza2_listener_event(
            {.kind = cg::Plaza2ListenerEventKind::Close, .stream_code = listener.config.stream_code}));
        close_listener(listener);
        // A pinned schema failure must remain visible until the connection
        // resets it; this listener deliberately does not retry that schema.
        if (!listener.scheme_incompatible)
            listener.object.clear_callback_error();
        listener.retry = now() + config.recovery_retry_interval;
    }
    void supervise_listener(Listener& listener) {
        std::uint32_t state = Closed;
        if (listener.scheme_incompatible)
            return;
        const auto error = listener.object.state(state);
        if (!error)
            observe(listener.observed_state, state, "listener", listener.config.stream_code);
        else
            operation("listener", "getstate", error, listener.config.stream_code);
        if (error || state == Error || listener.object.last_callback_error()) {
            if (listener.object.last_callback_error()) {
                const auto& callback_error = listener.object.last_callback_error();
                last_error = callback_error.message;
                listener.scheme_incompatible = callback_error.code == Plaza2ErrorCode::IncompatibleScheme;
            } else if (error)
                last_error = error.message;
            log("listener_recovery",
                "{\"stream_code\":" + std::to_string(static_cast<std::uint32_t>(listener.config.stream_code)) +
                    ",\"cause\":" + json_quote(last_error) + "}");
            invalidate(listener);
            return;
        }
        if (state == Closed && now() >= listener.retry) {
            if (listener.config.stream_code == StreamCode::kFortsTradeRepl && deferred_trade && !pos_anchor_health())
                return;
            // Each new snapshot replaces only its own stream's pending/committed domain.
            if (listener.handler == &bridge)
                projection.reset_stream_snapshot(listener.config.stream_code);
            if (listener.handler == &aggr)
                aggr.reset();
            if (listener.handler == &deals)
                deals.reset();
            const auto e = listener.object.open(render(listener.config.open_settings));
            operation("listener", "open", e, listener.config.stream_code);
            if (e) {
                last_error = e.message;
                listener.retry = now() + config.recovery_retry_interval;
            } else {
                ++recovery.attempts;
            }
        }
    }
    Plaza2TransportHealth health() const {
        Plaza2TransportHealth out;
        out.valid = initialized;
        if (connection.state(out.connection))
            out.valid = false;
        if (publisher.is_created())
            if (publisher.state(out.publisher))
                out.valid = false;
        out.private_active = streams_created;
        for (const auto& listener : listeners) {
            std::uint32_t state = Closed;
            static_cast<void>(listener.object.state(state));
            if (listener.object.last_callback_error())
                state = Error;
            const auto code = listener.config.stream_code;
            if (code == cg::kNoStreamCode)
                out.reply = state;
            else if (code == StreamCode::kFortsAggrRepl)
                out.aggr = state;
            else if (code == StreamCode::kFortsDealsRepl)
                out.public_deals = state;
            else {
                if (out.private_count >= out.private_states.size()) {
                    out.valid = false;
                    continue;
                }
                const auto i = out.private_count++;
                out.private_states[i] = state;
                out.private_streams[i] = code;
                out.private_active &= state == Active && stream_online(code);
            }
        }
        if (deferred_trade && !anchored_trade_ready())
            out.private_active = false;
        return out;
    }
    Plaza2Error poll(bool wait_for_data) {
        if (!initialized)
            return invalid("CGate session is not started");
        if (recovery.operation == Plaza2SessionOperation::Failed)
            return recovery.cause;
        replies.expire(now());
        std::uint32_t state = Closed;
        if (auto error = connection.state(state); error) {
            operation("connection", "getstate", error);
            waiting(error, Plaza2RecoveryWaitState::WaitingForRouter, "connection");
            wait_for_recovery();
            return {};
        }
        observe(observed_connection, state, "connection");
        if (state == Error) {
            for (auto& l : listeners) {
                l.scheme_incompatible = false;
                invalidate(l);
            }
            close_publisher();
            close_connection();
            connection_was_active = false;
            connection_retry = now() + config.recovery_retry_interval;
            waiting({.code = Plaza2ErrorCode::AdapterState, .message = "connection ERROR"},
                    Plaza2RecoveryWaitState::WaitingForRouter, "connection");
            wait_for_recovery();
            return {};
        }
        if (state == Closed) {
            if (connection_was_active) {
                for (auto& l : listeners) {
                    l.scheme_incompatible = false;
                    invalidate(l);
                }
                close_publisher();
                connection_was_active = false;
                connection_retry = now() + config.recovery_retry_interval;
                waiting({.code = Plaza2ErrorCode::AdapterState, .message = "connection CLOSED"},
                        Plaza2RecoveryWaitState::WaitingForRouter, "connection");
            }
            if (now() >= connection_retry) {
                ++recovery.attempts;
                if (auto error = open_connection(); error)
                    waiting(error, Plaza2RecoveryWaitState::WaitingForRouter, "connection");
                connection_retry = now() + config.recovery_retry_interval;
            }
            wait_for_recovery();
            return {};
        }
        if (state == Opening) {
            if (connection_was_active) {
                for (auto& l : listeners) {
                    l.scheme_incompatible = false;
                    invalidate(l);
                }
                close_publisher();
                connection_was_active = false;
            }
            waiting({.code = Plaza2ErrorCode::AdapterState, .message = "connection OPENING"},
                    Plaza2RecoveryWaitState::WaitingForPlaza, "connection");
        } else if (state == Active) {
            recovery.key_check_failed = false;
            key_check_warning_time.reset();
            connection_was_active = true;
            if (!streams_created) {
                if (now() < connection_retry)
                    return {};
                if (auto error = create_streams(); error) {
                    for (auto& l : listeners) {
                        static_cast<void>(l.object.destroy());
                    }
                    listeners.clear();
                    static_cast<void>(publisher.destroy());
                    waiting(error, Plaza2RecoveryWaitState::WaitingForService, "listener create");
                    if (error.runtime_code == 131073) {
                        last_error = "invalid CGate listener/publisher configuration: " + error.message;
                        error.message = last_error;
                        recovery.operation = Plaza2SessionOperation::Failed;
                        recovery.cause = error;
                        return error;
                    }
                    connection_retry = now() + config.recovery_retry_interval;
                    return {};
                }
            }
            if (auto error = open_anchored_trade(); error)
                last_error = error.message;
            for (auto& l : listeners)
                supervise_listener(l);
            if (!config.read_only_market_data) {
                std::uint32_t pub_state = Closed;
                auto error = publisher.state(pub_state);
                if (!error)
                    observe(observed_publisher, pub_state, "publisher");
                else
                    operation("publisher", "getstate", error);
                if (error || pub_state == Error) {
                    close_publisher();
                    publisher_retry = now() + config.recovery_retry_interval;
                } else if (pub_state == Closed && now() >= publisher_retry) {
                    const auto e = publisher.open(config.publisher_open_settings);
                    operation("publisher", "open", e);
                    if (e) {
                        last_error = e.message;
                        publisher_retry = now() + config.recovery_retry_interval;
                    }
                }
            }
        }
        // Bound work so replies, command queues and operator actions run even under sustained input.
        const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        std::uint32_t result = 0;
        for (std::size_t processed = 0; processed < 100; ++processed) {
            if (processed && std::chrono::steady_clock::now() >= drain_deadline)
                break;
            auto error = connection.process(0, &result);
            if (error) {
                operation("connection", "process", error);
                std::uint32_t error_state = Closed;
                if (!connection.state(error_state))
                    observe(observed_connection, error_state, "connection");
                waiting(error, Plaza2RecoveryWaitState::WaitingForRouter, "connection");
                for (auto& l : listeners) {
                    l.scheme_incompatible = false;
                    invalidate(l);
                }
                close_publisher();
                close_connection();
                connection_was_active = false;
                connection_retry = now() + config.recovery_retry_interval;
                break;
            }
            if (result == Timeout) {
                if (wait_for_data && config.process_timeout_ms) {
                    error = connection.process(config.process_timeout_ms, &result);
                    if (error) {
                        operation("connection", "process", error);
                        std::uint32_t error_state = Closed;
                        if (!connection.state(error_state))
                            observe(observed_connection, error_state, "connection");
                        waiting(error, Plaza2RecoveryWaitState::WaitingForRouter, "connection");
                        for (auto& l : listeners) {
                            l.scheme_incompatible = false;
                            invalidate(l);
                        }
                        close_publisher();
                        close_connection();
                        connection_was_active = false;
                        connection_retry = now() + config.recovery_retry_interval;
                    }
                    if (!error && result != Timeout)
                        continue;
                }
                break;
            }
        }
        auto h = health();
        recovery.health = h;
        std::int32_t current_session = config.aggr20_target_session_id;
        if (!current_session)
            current_session = projection.current_session_id();
        aggr.set_session_id(current_session);
        const bool ready = h.valid && h.connection == Active && h.private_active &&
                           (config.read_only_market_data || (h.publisher == Active && h.reply == Active));
        if (ready) {
            if (recovery.operation != Plaza2SessionOperation::Running) {
                ++recovery.transitions;
                log("recovery", "{\"state\":\"Running\"}");
            }
            recovery.operation = Plaza2SessionOperation::Running;
            recovery.wait_state = Plaza2RecoveryWaitState::None;
            recovery.alert_active = false;
        } else if (recovery.operation == Plaza2SessionOperation::Recovering) {
            recovery.wait_duration_ms = (ns(now()) - recovery.wait_start_time_ns) / 1000000;
            recovery.alert_active =
                recovery.wait_duration_ms >= static_cast<std::uint64_t>(config.recovery_alert_after.count());
        }
        return {};
    }
    Plaza2Error stop() {
        for (auto& l : listeners) {
            close_listener(l);
            static_cast<void>(l.object.destroy());
        }
        listeners.clear();
        close_publisher();
        static_cast<void>(publisher.destroy());
        close_connection();
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        if (initialized)
            static_cast<void>(bridge.end_run());
        initialized = false;
        streams_created = false;
        anchor.reset();
        deferred_trade.reset();
        replies.pending.clear();
        recovery.operation = Plaza2SessionOperation::Stopped;
        recovery.key_check_failed = false;
        key_check_warning_time.reset();
        return {};
    }
};
CgateSession::CgateSession(CgateSessionConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
CgateSession::~CgateSession() {
    if (impl_)
        static_cast<void>(impl_->stop());
}
CgateSession::CgateSession(CgateSession&&) noexcept = default;
CgateSession& CgateSession::operator=(CgateSession&&) noexcept = default;
Plaza2Error CgateSession::start() {
    auto e = impl_->start();
    if (e) {
        impl_->recovery.operation = Plaza2SessionOperation::Failed;
        impl_->recovery.cause = e;
    }
    return e;
}
Plaza2Error CgateSession::poll(bool wait_for_data) {
    return impl_->poll(wait_for_data);
}
Plaza2Error CgateSession::stop() {
    return impl_->stop();
}
bool CgateSession::started() const noexcept {
    return impl_->initialized;
}
bool CgateSession::recovering() const noexcept {
    return impl_->recovery.operation == Plaza2SessionOperation::Recovering;
}
const Plaza2RecoveryStatus& CgateSession::recovery_status() const noexcept {
    return impl_->recovery;
}
Plaza2TransportHealth CgateSession::runtime_health() const {
    return impl_->health();
}
const cg::Plaza2Error& CgateSession::listener_error(StreamCode stream) const noexcept {
    static const cg::Plaza2Error none;
    const auto found = std::find_if(impl_->listeners.begin(), impl_->listeners.end(),
                                    [&](const auto& listener) { return listener.config.stream_code == stream; });
    return found == impl_->listeners.end() ? none : found->object.last_callback_error();
}
const cg::Plaza2RuntimeProbeReport& CgateSession::probe_report() const noexcept {
    return impl_->probe;
}
const moex::plaza2::private_state::Plaza2PrivateStateProjector& CgateSession::private_state() const noexcept {
    return impl_->projection;
}
const cg::Plaza2Aggr20BookProjector& CgateSession::aggr20_projector() const noexcept {
    return impl_->book;
}
cg::Plaza2PublicDealsSnapshot CgateSession::public_deals_snapshot(std::uint64_t after) const {
    return impl_->deals.snapshot(after);
}
bool CgateSession::aggr_online() const noexcept {
    return impl_->aggr.online();
}
bool CgateSession::aggr_snapshot_complete() const noexcept {
    return impl_->aggr.snapshot_complete();
}
bool CgateSession::aggr_session_data_ready() const noexcept {
    return impl_->aggr.session_data_ready();
}
bool CgateSession::aggr_valid() const noexcept {
    return impl_->aggr.valid();
}
cg::Plaza2Aggr20Status CgateSession::aggr_status() const {
    return impl_->aggr.status();
}
bool CgateSession::p2mqreply_open() const noexcept {
    return runtime_health().reply == Active;
}
bool CgateSession::publisher_open() const noexcept {
    return runtime_health().publisher == Active;
}
const std::string& CgateSession::connection_app_name() const noexcept {
    return impl_->app_name;
}
cg::Plaza2PublisherCallCounts CgateSession::publisher_call_counts() const noexcept {
    return impl_->publisher.call_counts();
}
cg::Plaza2PublisherRateMetrics CgateSession::publisher_rate_metrics() const noexcept {
    return impl_->rate.metrics();
}
CgateSessionMode CgateSession::mode() const noexcept {
    return impl_->config.mode;
}
bool CgateSession::trade_replay_anchor_ready() const noexcept {
    return !impl_->config.trade_replay_from_pos_anchor || impl_->anchored_trade_ready();
}
std::optional<Plaza2TradeReplayAnchor> CgateSession::trade_replay_anchor_used() const noexcept {
    return impl_->anchor;
}
plaza2::cgate::Plaza2Error CgateSession::synchronize_order_book() {
    return impl_->open_anchored_trade(true);
}
bool CgateSession::order_book_snapshot_ready() const noexcept {
    return impl_->order_book_snapshot_ready();
}
std::vector<CgateSession::ReplyEvent> CgateSession::take_reply_events() {
    auto out = std::move(impl_->replies.events);
    impl_->replies.events.clear();
    return out;
}
moex::plaza2::private_state::PrivateRowChanges CgateSession::take_private_row_changes() {
    return impl_->projection.take_row_changes();
}
const std::string& CgateSession::last_callback_error() const noexcept {
    return impl_->last_error;
}
cg::Plaza2PublisherMessageResult CgateSession::post_command(const Plaza2TradeEncodedCommand& command,
                                                            std::uint32_t user_id) {
    auto h = runtime_health();
    cg::Plaza2PublisherMessageResult out;
    const auto expected_name = command.command_kind == Plaza2TradeCommandKind::AddOrder        ? "AddOrder"
                               : command.command_kind == Plaza2TradeCommandKind::DelOrder      ? "DelOrder"
                               : command.command_kind == Plaza2TradeCommandKind::DelUserOrders ? "DelUserOrders"
                               : command.command_kind == Plaza2TradeCommandKind::MoveOrder     ? "MoveOrder"
                                                                                               : "";
    if (command.command_name != expected_name || command.msgid != static_cast<int>(command.command_kind)) {
        out.validation_error = invalid("command name or id is outside declared command scope");
        return out;
    }
    bool required_private = true;
    if (command.command_kind == Plaza2TradeCommandKind::AddOrder ||
        command.command_kind == Plaza2TradeCommandKind::MoveOrder) {
        required_private = trade_replay_anchor_ready() && command.isin_id && impl_->trading_ready(command);
        for (auto code : {StreamCode::kFortsTradeRepl, StreamCode::kFortsPosRepl, StreamCode::kFortsPartRepl,
                          StreamCode::kFortsRefdataRepl, StreamCode::kFortsSessionstateRepl,
                          StreamCode::kFortsInstrumentstateRepl}) {
            const auto found = std::find(h.private_streams.begin(), h.private_streams.begin() + h.private_count, code);
            required_private &= found != h.private_streams.begin() + h.private_count &&
                                h.private_states[found - h.private_streams.begin()] == Active &&
                                impl_->stream_online(code);
        }
    }
    if (impl_->recovery.operation == Plaza2SessionOperation::Failed || !impl_->config.allow_orders ||
        impl_->config.read_only_market_data || !command.validation.ok() || !user_id || h.connection != Active ||
        h.publisher != Active || h.reply != Active || !required_private) {
        out.validation_error = invalid("order entry requires allow_orders, valid command and ONLINE private streams");
        return out;
    }
    if (impl_->replies.pending.contains(user_id)) {
        out.validation_error = invalid("user_id already reserved by a command");
        return out;
    }
    return post_validated(command.command_name, command.payload, user_id, true);
}
cg::Plaza2PublisherMessageResult CgateSession::post_validated(std::string_view name, std::span<const std::byte> payload,
                                                              std::uint32_t user_id, bool need_reply) {
    cg::Plaza2PublisherMessageResult out;
    if (impl_->config.publisher_rate_owner == PublisherRateOwner::Session && !impl_->rate.admit(impl_->now_ms())) {
        impl_->log("throttle", "{\"user_id\":" + std::to_string(user_id) + "}");
        out.validation_error = invalid("publisher rate limit reached");
        return out;
    }
    auto kind = name == "AddOrder"    ? Plaza2TradeCommandKind::AddOrder
                : name == "DelOrder"  ? Plaza2TradeCommandKind::DelOrder
                : name == "MoveOrder" ? Plaza2TradeCommandKind::MoveOrder
                                      : Plaza2TradeCommandKind::DelUserOrders;
    impl_->replies.pending.emplace(
        user_id, Replies::Pending{kind, impl_->now() + std::chrono::milliseconds(impl_->config.reply_timeout_ms)});
    out = impl_->publisher.post_by_message_name(name, payload, user_id, need_reply);
    if (out.certainty == cg::Plaza2SubmissionCertainty::DefinitelyNotSent)
        impl_->replies.pending.erase(user_id);
    return out;
}
} // namespace moex::plaza2_trade
