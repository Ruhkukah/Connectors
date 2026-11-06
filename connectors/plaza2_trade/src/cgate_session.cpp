#include "moex/plaza2_trade/cgate_session.hpp"
#include "fol_auxiliary.hpp"
#include "moex/plaza2/cgate/plaza2_replication_tables.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_fixed_point.hpp"
#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"
#include "moex/plaza2/cgate/cgate_logging.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <thread>
#include <utility>

namespace moex::plaza2_trade {
namespace {
using fol_auxiliary::integer_value;
using fol_auxiliary::value;
namespace cg = moex::plaza2::cgate;
using cg::Plaza2Error;
using cg::Plaza2ErrorCode;
using moex::plaza2::generated::StreamCode;
constexpr std::uint32_t Closed = 0, Error = 1, Opening = 2, Active = 3, Unsupported = 131074, Timeout = 131075;
Plaza2Error invalid(std::string message) {
    return {.code = Plaza2ErrorCode::InvalidConfiguration, .message = std::move(message)};
}
bool client_scheme_has_spectra_environment(const std::filesystem::path& path) {
    std::ifstream scheme(path);
    const auto trim = [](std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r");
        return first == text.npos ? std::string_view{} : text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    };
    bool refdata{};
    std::string line;
    while (std::getline(scheme, line)) {
        const auto text = trim(std::string_view(line).substr(0, line.find(';')));
        if (text.starts_with('['))
            refdata = text == "[dbscheme:REFDATA]";
        const auto equal = text.find('=');
        if (refdata && equal != text.npos && trim(text.substr(0, equal)) == "table" &&
            trim(text.substr(equal + 1)) == "spectra_environment")
            return true;
    }
    return false;
}
void replace(std::string& value, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = value.find(from, at)) != std::string::npos) {
        value.replace(at, from.size(), to);
        at += to.size();
    }
}
bool set_lifenum(std::string& settings, std::int64_t life) {
    std::size_t found = std::string::npos;
    for (std::size_t start = 0; start < settings.size();) {
        const auto end = settings.find(';', start);
        if (std::string_view(settings).substr(start, end - start).starts_with("lifenum=")) {
            if (found != std::string::npos)
                return false;
            found = start;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    const auto value = "lifenum=" + std::to_string(life);
    if (found == std::string::npos)
        settings += ";" + value;
    else
        settings.replace(found, settings.find(';', found) - found, value);
    return true;
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
std::uint64_t utc_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
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
    bool measure_timings{};
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
        const auto receipt = measure_timings ? ns(std::chrono::steady_clock::now()) : 0;
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
                                  .raw_payload = {event.raw_payload.begin(), event.raw_payload.end()},
                                  .callback_receipt_steady_ns = receipt});
            }
            return {};
        }
        events.push_back({.user_id = event.user_id,
                          .message_id = event.message_id,
                          .command_kind = found->second.kind,
                          .timed_out = event.kind == cg::Plaza2ListenerEventKind::Timeout,
                          .raw_payload = {event.raw_payload.begin(), event.raw_payload.end()},
                          .callback_receipt_steady_ns = receipt});
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
class AnchoredTradeEvents final : public cg::Plaza2ListenerEventHandler {
  public:
    explicit AnchoredTradeEvents(cg::Plaza2PrivateStateBridge& bridge) : bridge_(bridge) {}
    void opening(std::uint64_t requested_lifenum) {
        requested_lifenum_ = requested_lifenum;
        explicit_lifenum_ = false;
    }
    Plaza2Error on_plaza2_listener_event(const cg::Plaza2ListenerEvent& event) override {
        if (event.kind == cg::Plaza2ListenerEventKind::LifeNum)
            explicit_lifenum_ = true;
        if (event.kind == cg::Plaza2ListenerEventKind::Open && !explicit_lifenum_) {
            // CGate omits LifeNum when the supplied epoch is current. OPEN
            // confirms that request; an earlier server LifeNum takes precedence.
            if (const auto error = bridge_.on_plaza2_listener_event({.kind = cg::Plaza2ListenerEventKind::LifeNum,
                                                                     .stream_code = event.stream_code,
                                                                     .unsigned_value = requested_lifenum_});
                error)
                return error;
        }
        return bridge_.on_plaza2_listener_event(event);
    }

  private:
    cg::Plaza2PrivateStateBridge& bridge_;
    std::uint64_t requested_lifenum_{};
    bool explicit_lifenum_{};
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
    struct FullOrderLogEvents final : cg::Plaza2ListenerEventHandler {
        Impl& owner;
        std::optional<std::chrono::steady_clock::time_point> activity;
        std::array<std::uint64_t, 11> event_counts{};
        std::array<std::uint64_t, 4> snapshot_rows{};
        std::uint64_t first_info_position{}, last_info_position{};
        std::chrono::steady_clock::time_point cycle_started{};
        std::uint64_t cycle{}, event_position{};
        bool cycle_open{}, online_seen{}, failed_this_cycle{};
        unsigned strikes{}, post_online_strikes{}, post_online_position{};
        bool trace_after_online{}, post_online_rows{};
        std::string last_failure, post_online_failure;
        Plaza2Error fatal;
        void diagnostic(const cg::Plaza2ListenerEvent& event) {
            using enum cg::Plaza2ListenerEventKind;
            if (event.kind == Open || (!cycle_open && event.kind != Close)) {
                if (cycle_open)
                    summary();
                ++cycle;
                event_counts.fill(0);
                snapshot_rows.fill(0);
                first_info_position = last_info_position = 0;
                cycle_started = owner.now();
                event_position = 0;
                cycle_open = true;
                online_seen = failed_this_cycle = trace_after_online = post_online_rows = false;
                post_online_position = 0;
            }
            if (!cycle_open)
                return; // Native and synthetic CLOSE may both arrive for one cycle.
            const auto kind = static_cast<unsigned>(event.kind);
            ++event_counts[std::min<std::size_t>(kind, event_counts.size() - 1)];
            if (++event_position <= 64)
                owner.log("full_order_log_event",
                          "{\"cycle\":" + std::to_string(cycle) + ",\"position\":" + std::to_string(event_position) +
                              ",\"kind\":" + std::to_string(kind) +
                              ",\"table\":" + json_quote(event.raw_table ? event.raw_table->name : "unspecified") +
                              ",\"data_size\":" + std::to_string(event.native_data_size) + "}");
            if (!online_seen && event.kind == StreamData && event.raw_table) {
                constexpr std::array<std::string_view, 4> tables{"orders", "info", "orders_log", "sys_events"};
                const auto at = std::ranges::find(tables, event.raw_table->name);
                if (at != tables.end())
                    ++snapshot_rows[at - tables.begin()];
                if (event.raw_table->name == "info") {
                    if (!first_info_position)
                        first_info_position = event_position;
                    last_info_position = event_position;
                }
            }
            if (trace_after_online && post_online_position < 32) {
                ++post_online_position;
                owner.log("full_order_log_online_event",
                          "{\"cycle\":" + std::to_string(cycle) + ",\"position\":" + std::to_string(event_position) +
                              ",\"post_online_position\":" + std::to_string(post_online_position) +
                              ",\"kind\":" + std::to_string(kind) +
                              ",\"table\":" + json_quote(event.raw_table ? event.raw_table->name : "unspecified") +
                              ",\"data_size\":" + std::to_string(event.native_data_size) + "}");
            }
            if (event.kind == Close)
                summary();
        }
        void summary() {
            std::string counts;
            for (auto count : event_counts)
                counts += (counts.empty() ? "" : ",") + std::to_string(count);
            owner.log("full_order_log_cycle_totals",
                      "{\"cycle\":" + std::to_string(cycle) + ",\"events\":" + std::to_string(event_position) +
                          ",\"counts\":[" + counts + "]" + ",\"snapshot_rows\":[" + std::to_string(snapshot_rows[0]) +
                          "," + std::to_string(snapshot_rows[1]) + "," + std::to_string(snapshot_rows[2]) + "," +
                          std::to_string(snapshot_rows[3]) + "]" +
                          ",\"first_info_position\":" + std::to_string(first_info_position) +
                          ",\"last_info_position\":" + std::to_string(last_info_position) + "}");
            cycle_open = false;
        }
        std::string service_pair;
        explicit FullOrderLogEvents(Impl& value) : owner(value) {}
        bool wants_raw_replication(std::string_view table) const noexcept override {
            return owner.config.full_order_log_handler->wants_raw_replication(table);
        }
        std::span<const plaza2::generated::FieldDescriptor>
        replication_fields(plaza2::generated::TableCode table) const noexcept override {
            return owner.config.full_order_log_handler->replication_fields(table);
        }
        bool should_log_listener_event(const cg::Plaza2ListenerEvent& event) const noexcept override {
            return owner.config.full_order_log_handler->should_log_listener_event(event);
        }
        bool needs_fresh_snapshot() const noexcept override {
            return owner.config.full_order_log_handler->needs_fresh_snapshot();
        }
        void on_plaza2_listener_error(const Plaza2Error& error) noexcept override {
            activity.reset();
            owner.config.full_order_log_handler->on_plaza2_listener_error(error);
        }
        Plaza2Error on_plaza2_listener_event(const cg::Plaza2ListenerEvent& event) override {
            using enum cg::Plaza2ListenerEventKind;
            if (event.kind == Open || event.kind == Close)
                activity.reset();
            diagnostic(event);
            const auto error = [&] {
                if (event.kind != Open)
                    return owner.config.full_order_log_handler->on_plaza2_listener_event(event);
                auto bound = event;
                bound.text_value = service_pair;
                return owner.config.full_order_log_handler->on_plaza2_listener_event(bound);
            }();
            if (!error && event.kind == Online) {
                owner.log(
                    "full_order_log_snapshot",
                    "{\"cycle\":" + std::to_string(cycle) + ",\"time_to_online_ns\":" +
                        std::to_string(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(owner.now() - cycle_started).count()) +
                        ",\"orders_rows\":" + std::to_string(snapshot_rows[0]) + ",\"info_rows\":" +
                        std::to_string(snapshot_rows[1]) + ",\"orders_log_rows\":" + std::to_string(snapshot_rows[2]) +
                        ",\"sys_events_rows\":" + std::to_string(snapshot_rows[3]) +
                        ",\"first_info_position\":" + std::to_string(first_info_position) +
                        ",\"last_info_position\":" + std::to_string(last_info_position) + "}");
                online_seen = trace_after_online = true;
                post_online_position = 0;
                strikes = 0;
                last_failure.clear();
            }
            if (!error && online_seen && event.kind == TransactionBegin)
                post_online_rows = false;
            if (!error && online_seen && event.kind == StreamData)
                post_online_rows = true;
            if (!error && online_seen && event.kind == TransactionCommit && post_online_rows) {
                post_online_strikes = 0;
                post_online_failure.clear();
            }
            if (error && !failed_this_cycle) {
                failed_this_cycle = true;
                if (event.kind == StreamData && error.code == Plaza2ErrorCode::DecodeFailed) {
                    // Sequence positions may vary with filtered rows; compare the stable validation reason.
                    const auto signature = error.message.substr(0, error.message.find(" event_position="));
                    auto& count = online_seen ? post_online_strikes : strikes;
                    auto& previous = online_seen ? post_online_failure : last_failure;
                    count = signature == previous ? count + 1 : 1;
                    previous = signature;
                    owner.log("full_order_log_row_failure",
                              "{\"strikes\":" + std::to_string(count) + ",\"reason\":" + json_quote(signature) + "}");
                    if (count == 3) {
                        fatal = {.code = Plaza2ErrorCode::DecodeFailed,
                                 .message =
                                     "Full Order Log stopped after 3 identical row-validation failures: " + signature};
                        owner.log("full_order_log_fatal", "{\"reason\":" + json_quote(fatal.message) + "}");
                    }
                } else {
                    strikes = post_online_strikes = 0;
                    last_failure.clear();
                    post_online_failure.clear();
                }
            }
            if (!error && (event.kind == StreamData || event.kind == TransactionCommit))
                activity = owner.now();
            return error;
        }
    } full_order_log_events{*this};
    struct Listener {
        CgateStreamConfig config;
        cg::Plaza2Listener object;
        cg::Plaza2ListenerEventHandler* handler{};
        std::chrono::steady_clock::time_point retry{};
        std::optional<std::chrono::steady_clock::time_point> snapshot_wait;
        bool scheme_incompatible{};
        std::optional<std::uint32_t> observed_state;
        bool advisory{}, matching_scoped{};
    };
    struct Auxiliary {
        std::string_view name, url;
        bool matching_scoped{};
        fol_auxiliary::Projection data;
        std::chrono::steady_clock::time_point retry{};
        Plaza2Error create_error;
    };
    static constexpr auto auxiliary_code = static_cast<StreamCode>(0xF0110002u);
    CgateSessionConfig config;
    std::vector<std::unique_ptr<Auxiliary>> auxiliary;
    cg::Plaza2RuntimeProbeReport probe;
    cg::Plaza2Env env;
    cg::Plaza2Connection connection;
    cg::Plaza2Publisher publisher;
    moex::plaza2::private_state::Plaza2PrivateStateProjector projection;
    cg::Plaza2PrivateStateBridge bridge{projection};
    AnchoredTradeEvents anchored_trade_events{bridge};
    std::optional<std::pair<std::uint64_t, std::int32_t>> matching_refdata;
    std::optional<std::int8_t> matching_id;
    bool matching_current{};
    std::chrono::steady_clock::time_point matching_retry{}, fol_deals_retry{};
    Plaza2Error publisher_layout_failure;
    cg::Plaza2Aggr20BookProjector book;
    cg::Plaza2Aggr20ListenerBridge aggr{book, 0};
    struct PublicDealsTarget {
        std::int64_t isin_id{};
        std::int32_t session_id{};
        std::uint64_t refdata_lifenum{}, session_lifenum{};
        bool operator==(const PublicDealsTarget&) const = default;
    };
    std::unique_ptr<cg::Plaza2PublicDealsBridge> deals;
    std::optional<PublicDealsTarget> deals_target;
    // A replacement bridge begins at its own epoch one. Keep exported epochs
    // increasing so a DTC subscription can never mistake a new target for its
    // previous stream, including stop/start on the same session object.
    std::uint64_t deals_epoch_base{};
    bool deals_restart{}, deals_epoch_exhausted{};
    std::chrono::steady_clock::time_point deals_retry{};
    Replies replies;
    cg::Plaza2PublisherRateGate rate;
    std::vector<Listener> listeners;
    Plaza2RecoveryStatus recovery;
    std::chrono::steady_clock::time_point connection_retry{}, publisher_retry{};
    std::optional<std::chrono::steady_clock::time_point> key_check_warning_time;
    std::string app_name, last_error, credentials, software_key;
    bool initialized{}, streams_created{}, connection_was_active{}, publisher_warmed{};
    CgatePollMetrics poll_metrics;
    bool collecting_poll{};
    std::optional<CgateStreamConfig> deferred_trade;
    std::optional<Plaza2TradeReplayAnchor> anchor;
    std::optional<std::uint32_t> observed_connection, observed_publisher;
    explicit Impl(CgateSessionConfig c)
        : config(std::move(c)), book({}, config.market_data_isin_ids), rate(config.publisher_messages_per_second) {
        replies.penalty = [this](std::uint32_t ms) {
            if (config.publisher_rate_owner == PublisherRateOwner::Session)
                rate.penalize(now_ms(), ms);
        };
        replies.log = config.event_log;
        replies.external_owner = config.publisher_rate_owner == PublisherRateOwner::External;
        replies.measure_timings = config.collect_poll_metrics;
        if (config.collect_poll_metrics) {
            poll_metrics.events.reserve(128);
            poll_metrics.callbacks.reserve(128);
        }
    }
    void observe_callback(const cg::Plaza2CallbackTiming& timing) {
        if (!collecting_poll)
            return;
        poll_metrics.callback_ns += timing.elapsed_ns;
        auto found = std::find_if(poll_metrics.callbacks.begin(), poll_metrics.callbacks.end(), [&](const auto& entry) {
            return entry.stream_code == timing.stream_code && entry.table_code == timing.table_code &&
                   entry.native_message_type == timing.native_message_type;
        });
        if (found == poll_metrics.callbacks.end()) {
            if (poll_metrics.callbacks.size() == 128) {
                ++poll_metrics.unattributed_callbacks;
                poll_metrics.unattributed_callback_ns += timing.elapsed_ns;
                return;
            }
            poll_metrics.callbacks.push_back({.stream_code = timing.stream_code,
                                              .table_code = timing.table_code,
                                              .native_message_type = timing.native_message_type});
            found = std::prev(poll_metrics.callbacks.end());
        }
        ++found->count;
        found->total_ns += timing.elapsed_ns;
        found->max_ns = std::max(found->max_ns, timing.elapsed_ns);
    }
    void observe_event(const cg::Plaza2ListenerEvent& event) {
        if (!collecting_poll)
            return;
        auto found = std::find_if(poll_metrics.events.begin(), poll_metrics.events.end(), [&](const auto& entry) {
            return entry.stream_code == event.stream_code && entry.table_code == event.table_code &&
                   entry.kind == event.kind;
        });
        if (found == poll_metrics.events.end()) {
            if (poll_metrics.events.size() == 128) {
                ++poll_metrics.unattributed_events;
                return;
            }
            poll_metrics.events.push_back(
                {.stream_code = event.stream_code, .table_code = event.table_code, .kind = event.kind});
            found = std::prev(poll_metrics.events.end());
        }
        ++found->count;
        found->last_receipt_utc_ns = utc_ns();
        found->timestamp_field = cg::kNoFieldCode;
        found->exchange_timestamp_ns = 0;
        for (const auto& field : event.fields)
            if (field.kind == cg::Plaza2DecodedValueKind::UnsignedInteger) {
                const auto* descriptor = plaza2::generated::FindFieldByCode(field.field_code);
                if (descriptor && descriptor->field_name == "moment_ns") {
                    found->timestamp_field = field.field_code;
                    found->exchange_timestamp_ns = field.unsigned_value;
                    return;
                }
            }
        for (const auto& field : event.fields)
            if (field.kind == cg::Plaza2DecodedValueKind::Timestamp) {
                found->timestamp_field = field.field_code;
                found->exchange_timestamp_ns = field.timestamp_ns;
                break;
            }
    }
    Plaza2Error process(std::uint32_t timeout_ms, std::uint32_t* result) {
        if (!collecting_poll)
            return connection.process(timeout_ms, result);
        ++poll_metrics.process_calls;
        const auto before = std::chrono::steady_clock::now();
        const auto error = connection.process(timeout_ms, result);
        const auto elapsed = ns(std::chrono::steady_clock::now()) - ns(before);
        poll_metrics.process_ns += elapsed;
        if (timeout_ms)
            poll_metrics.blocking_process_ns += elapsed;
        return error;
    }
    std::chrono::steady_clock::time_point now() const {
        return config.recovery_now ? config.recovery_now() : std::chrono::steady_clock::now();
    }
    std::uint64_t now_ms() const {
        return config.publisher_now_ms ? config.publisher_now_ms() : ns(now()) / 1000000;
    }
    void wait_for_recovery() const {
        // A zero process timeout suppresses CGate's wait, but a disconnected
        // owner must still yield between retries instead of consuming a core.
        const auto delay =
            std::chrono::milliseconds(config.process_timeout_ms ? std::min(config.process_timeout_ms, 2u) : 2);
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
        publisher_warmed = false;
        publisher_layout_failure = {};
        if (!publisher.is_created())
            return;
        const auto error = publisher.close();
        operation("publisher", "close", error);
        if (!error)
            observe(observed_publisher, Closed, "publisher");
    }
    void close_listener(Listener& listener) {
        if (listener.config.stream_code == cg::kFullOrderLogStreamCode && full_order_log_events.cycle_open)
            full_order_log_events.diagnostic({.kind = cg::Plaza2ListenerEventKind::Close});
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
        if (config.full_order_log_partitioned &&
            (config.market_data_isin_ids.empty() ||
             (!config.full_order_log_handler && !config.full_order_log_cancel_only)))
            return invalid("partitioned FullOrderLog requires selected instruments and an explicit FOL mode");
        if (config.full_order_log_cancel_only &&
            (config.runtime.environment != cg::Plaza2Environment::Test || !config.allow_orders ||
             config.read_only_market_data || !config.full_order_log_stream.settings.empty() ||
             config.full_order_log_handler || config.full_order_log_oi_handler || config.full_order_log_authority ||
             !config.aggr20_stream.settings.empty() || !config.public_deals_stream.settings.empty()))
            return invalid(
                "FullOrderLog cancel-only requires explicit TEST order authority and no market-data listeners");
        if (config.full_order_log_oi_handler &&
            (!config.full_order_log_handler || config.full_order_log_stream.settings.empty() ||
             !config.public_deals_stream.settings.empty() || !config.aggr20_stream.settings.empty() ||
             !config.public_deals_stream.open_settings.empty()))
            return invalid("FullOrderLog OI requires FOL authority and no duplicate DEALS/AGGR stream");
        if (!config.public_deals_stream.settings.empty() &&
            (!config.read_only_market_data || config.public_deals_target_isin_id <= 0 ||
             config.public_deals_target_isin_id > std::numeric_limits<std::int32_t>::max()))
            return invalid("public DEALS requires read-only market data and a positive int32 target ISIN");
        if (config.connection_settings.empty() || config.runtime.runtime_root.empty())
            return invalid("CGate runtime and connection are required");
        if (!config.full_order_log_stream.settings.empty()) {
            try {
                const bool writable = config.allow_orders &&
                                      config.runtime.environment == cg::Plaza2Environment::Test &&
                                      config.full_order_log_authority;
                if ((!config.read_only_market_data && !writable) || !config.full_order_log_handler ||
                    config.market_data_isin_ids.empty() ||
                    config.full_order_log_stream.stream_code != cg::kFullOrderLogStreamCode ||
                    config.full_order_log_stream.settings !=
                        cg::full_order_log_listener_settings(config.runtime.scheme_dir) ||
                    config.full_order_log_stream.open_settings.find("replstate") != std::string::npos)
                    return invalid(
                        "FullOrderLog requires its canonical two-schema listener, read-only or explicit TEST order "
                        "authority, and no replstate");
                for (const auto name : {"online.ini", "snapshot.ini"}) {
                    const auto path = config.runtime.scheme_dir / "full_order_log" / name;
                    std::error_code error;
                    if (!std::filesystem::is_regular_file(path, error) || !std::ifstream(path).good())
                        return invalid("FullOrderLog client schema is missing or unreadable: " + std::string(name));
                }
            } catch (const std::invalid_argument& error) {
                return invalid(error.what());
            }
        }
        if (config.full_order_log_authority && config.full_order_log_stream.settings.empty())
            return invalid("FullOrderLog order authority requires the composite listener");
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
        runtime.listener_callback_timing = {};
        runtime.listener_event_log = config.listener_event_log;
        if (config.collect_poll_metrics) {
            runtime.listener_callback_timing = [this](const auto& timing) { observe_callback(timing); };
            runtime.listener_event_log = [this](const auto& event) {
                observe_event(event);
                if (!config.listener_event_log)
                    return;
                if (!collecting_poll) {
                    config.listener_event_log(event);
                    return;
                }
                const auto before = std::chrono::steady_clock::now();
                try {
                    config.listener_event_log(event);
                } catch (...) {
                    poll_metrics.observer_ns += ns(std::chrono::steady_clock::now()) - ns(before);
                    throw;
                }
                poll_metrics.observer_ns += ns(std::chrono::steady_clock::now()) - ns(before);
            };
        }
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
        if (config.full_order_log_advisory)
            configure_auxiliary(client_scheme_has_spectra_environment(probe.layout.scheme_path));
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
    void configure_auxiliary(bool environment_supported) {
        using fol_auxiliary::Table;
        auxiliary.clear();
        const auto add = [&](std::string_view name, std::string_view url, bool match, std::vector<Table> tables,
                             std::string account = {}) {
            auxiliary.push_back(std::unique_ptr<Auxiliary>(new Auxiliary{
                name, url, match, {std::move(tables), std::move(account), config.market_data_isin_ids, [this] {
                                       return now();
                                   }}}));
        };
        add("common", "p2repl://FORTS_COMMON_REPL;tables=common", true,
            {{"common",
              {{"sess_id", "i4"},
               {"best_buy_native", "d16.5"},
               {"best_sell_native", "d16.5"},
               {"xamount_buy_native", "i8"},
               {"xamount_sell_native", "i8"},
               {"xpos", "i8"},
               {"settlement_price", "d16.5", true},
               {"settlement_price_open", "d16.5", true}},
              {},
              true}});
        add("schedule", "p2repl://FORTS_REFDATA_REPL;tables=trade_periods,discrete_auction", false,
            {{"trade_periods",
              {{"sess_id", "i4"},
               {"trade_period_id", "i8"},
               {"trade_period_type", "i8"},
               {"trade_period_begin", "t"},
               {"trade_period_end", "t"},
               {"trade_period_name", "c255"}}},
             {"discrete_auction",
              {{"sess_id", "i4"},
               {"trade_period_id", "i8"},
               {"add_order_start", "t"},
               {"add_order_finish_from", "t"},
               {"add_order_finish_till", "t"}}}});
        add("rates", "p2repl://MOEX_RATES_REPL;tables=curr_online", false,
            {{"curr_online", {{"rate_id", "i4"}, {"value", "d16.5"}, {"moment", "t"}}}});
        add("index", "p2repl://RTS_INDEX_REPL;tables=rts_index", false,
            {{"rts_index", {{"name", "c25"}, {"value_highprec", "d18.6"}, {"moment", "t"}}}});
        if (environment_supported)
            add("spectra_environment", "p2repl://FORTS_REFDATA_REPL;tables=spectra_environment", false,
                {{"spectra_environment", {{"environment_id", "i4"}, {"environment_name", "c8"}}}});
        else
            log("full_order_log_advisory_unavailable",
                R"({"source":"spectra_environment","error":"client REFDATA scheme does not advertise the table"})");
        if (!config.full_order_log_login.empty())
            add("cod", "p2repl://FORTS_REFDATA_REPL;tables=user", false,
                {{"user", {{"login", "c20"}, {"sma_flags", "i4"}, {"start_date", "t"}, {"end_date", "t"}}, "login"}},
                config.full_order_log_login);
        if (!config.full_order_log_login.empty())
            add("transaction_fees", "p2repl://FORTS_TNPENALTY_REPL;tables=fee_tn,fee_all", false,
                {{"fee_tn",
                  {{"p2login", "c64"},
                   {"sess_id", "i4"},
                   {"time", "i8"},
                   {"tn_type", "i4"},
                   {"err_code", "i4"},
                   {"count", "i4"},
                   {"num_orders", "i4"}},
                  "p2login"},
                 {"fee_all",
                  {{"p2login", "c64"}, {"sess_id", "i4"}, {"time", "i8"}, {"points", "i4"}, {"fee", "d16.2"}},
                  "p2login"}},
                config.full_order_log_login);
        if (config.full_order_log_account.empty())
            return;
        add("part_balance", "p2repl://FORTS_PART_REPL;tables=part", false,
            {{"part",
              {{"client_code", "c7"}, {"money_free", "d26.2"}, {"money_blocked", "d26.2"}, {"money_amount", "d26.2"}},
              "client_code"}},
            config.full_order_log_account);
        add("variation_margin", "p2repl://FORTS_VM_REPL;tables=fut_vm", true,
            {{"fut_vm",
              {{"client_code", "c7"}, {"sess_id", "i4"}, {"vm", "d16.5"}, {"vm_at_settl", "d16.5"}},
              "client_code",
              true}},
            config.full_order_log_account);
        add("prohibitions", "p2repl://FORTS_PROHIBITION_REPL;tables=prohibition", false,
            {{"prohibition", {{"client_code", "c7"}, {"type", "i4"}}, "client_code"}}, config.full_order_log_account);
        add("margin_without_orders", "p2repl://FORTS_RMT_REPL;tables=rmt_im", false,
            {{"rmt_im",
              {{"client_code", "c7"},
               {"sess_id", "i4"},
               {"im_time", "t"},
               {"rmt_im", "d16.2"},
               {"rmt_posrisk", "d16.2"},
               {"rmt_posrisk_at_settl", "d16.2"}},
              "client_code"}},
            config.full_order_log_account);
        add("clearing", "p2repl://FORTS_CLR_REPL;tables=fut_sess_settl,money_clearing,fut_pos,clr_rate,sys_events",
            false,
            {{"fut_sess_settl", {{"sess_id", "i4"}, {"settl_price", "d16.5"}, {"date_clr", "t"}}, {}, true},
             {"money_clearing",
              {{"client_code", "c7"}, {"amount_end", "d21.2"}, {"vm", "d16.2"}, {"date_money_clearing", "t"}},
              "client_code"},
             {"fut_pos",
              {{"client_code", "c7"}, {"sess_id", "i4"}, {"xpos_end", "i8"}, {"date_money_clearing", "t"}},
              "client_code",
              true},
             {"clr_rate", {{"sess_id", "i4"}, {"rate_id", "i4"}, {"rate", "d16.5"}, {"moment", "t"}}},
             {"sys_events", {{"event_id", "i8"}, {"sess_id", "i4"}, {"event_type", "i4"}, {"server_time", "t"}}}},
            config.full_order_log_account);
    }
    Plaza2Error add_listener(CgateStreamConfig c, cg::Plaza2ListenerEventHandler& handler, bool advisory = false,
                             bool matching_scoped = false) {
        Listener listener;
        listener.config = std::move(c);
        listener.handler = &handler;
        listener.advisory = advisory;
        listener.matching_scoped = matching_scoped;
        const auto layouts = config.validate_order_message_layouts && &handler == &replies
                                 ? trade_reply_layouts()
                                 : std::span<const cg::Plaza2MessageLayout>{};
        if (auto error = listener.object.create(connection, listener.config.stream_code,
                                                render(listener.config.settings), &handler, layouts);
            error)
            return error;
        const auto& url = listener.config.settings;
        if (&handler == &full_order_log_events)
            full_order_log_events.service_pair = url.substr(0, url.find(";online.scheme="));
        if (config.full_order_log_partitioned && url.starts_with("p2repl://"))
            log("full_order_log_subscription", "{\"settings\":" + json_quote(url) + "}");
        listeners.push_back(std::move(listener));
        return {};
    }
    void ensure_auxiliary_streams() {
        if (auxiliary.empty())
            return;
        for (auto& source : auxiliary) {
            if ((source->matching_scoped && !matching_current) || now() < source->retry ||
                std::ranges::any_of(listeners, [&](const auto& l) { return l.handler == &source->data; }))
                continue;
            auto url = std::string(source->url);
            if (source->matching_scoped)
                url = match_service(std::move(url), *matching_id);
            const auto error = add_listener({auxiliary_code, std::move(url), "mode=snapshot+online"}, source->data,
                                            true, source->matching_scoped);
            if (error) {
                source->retry = now() + config.recovery_retry_interval;
                if (source->create_error.message != error.message)
                    log("full_order_log_advisory_unavailable",
                        "{\"source\":" + json_quote(source->name) + ",\"error\":" + json_quote(error.message) + "}");
            }
            source->create_error = error;
        }
    }
    static bool partitioned_stream(StreamCode code) {
        return code == StreamCode::kFortsTradeRepl || code == StreamCode::kFortsPosRepl ||
               code == StreamCode::kFortsUserorderbookRepl || code == StreamCode::kFortsDealsRepl ||
               code == cg::kFullOrderLogStreamCode;
    }
    static std::string match_service(std::string settings, std::int8_t match) {
        const auto end = settings.find(';');
        settings.insert(end == std::string::npos ? settings.size() : end, "_MATCH" + std::to_string(match));
        return settings;
    }
    Plaza2Error create_streams() {
        auto streams = config.private_streams;
        if (config.status_streams.empty()) {
            config.status_streams = {{StreamCode::kFortsSessionstateRepl,
                                      cg::required_replication_listener_settings(StreamCode::kFortsSessionstateRepl),
                                      {}},
                                     {StreamCode::kFortsInstrumentstateRepl,
                                      cg::required_replication_listener_settings(StreamCode::kFortsInstrumentstateRepl),
                                      {}}};
        }
        streams.insert(streams.end(), config.status_streams.begin(), config.status_streams.end());
        std::vector<StreamCode> codes;
        for (auto& s : streams)
            codes.push_back(s.stream_code);
        if (auto error = bridge.reset(codes); error)
            return error;
        bridge.require_full_order_log_refdata(config.full_order_log_handler || config.full_order_log_partitioned);
        if (auto error = bridge.begin_run(); error)
            return error;
        listeners.reserve(streams.size() + 3);
        for (auto& stream : streams) {
            if ((config.full_order_log_handler || config.full_order_log_partitioned) &&
                stream.stream_code == StreamCode::kFortsRefdataRepl)
                stream.settings =
                    cg::required_replication_listener_settings(stream.stream_code) + ",instr2matching_map";
            if (config.full_order_log_partitioned && partitioned_stream(stream.stream_code))
                continue;
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
        if (config.full_order_log_oi_handler && !config.full_order_log_partitioned) {
            const CgateStreamConfig oi{StreamCode::kFortsDealsRepl,
                                       cg::required_replication_listener_settings(StreamCode::kFortsDealsRepl),
                                       {}};
            if (auto error = add_listener(oi, *config.full_order_log_oi_handler); error)
                return error;
        }
        // DEALS has immutable target IDs. Its listener must wait for committed
        // current-session metadata instead of opening a zero-session bridge.
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
    Plaza2Error retire_matching_streams() {
        fol_deals_retry = {};
        deferred_trade.reset();
        anchor.reset();
        for (auto& listener : listeners) {
            if (!listener.matching_scoped && !partitioned_stream(listener.config.stream_code))
                continue;
            invalidate(listener);
            if (auto error = listener.object.destroy(); error)
                return error;
        }
        std::erase_if(listeners, [](const auto& listener) {
            return listener.matching_scoped || partitioned_stream(listener.config.stream_code);
        });
        return {};
    }
    Plaza2Error create_matching_streams(std::int8_t match) {
        for (auto stream : config.private_streams) {
            if (!partitioned_stream(stream.stream_code))
                continue;
            stream.settings = match_service(std::move(stream.settings), match);
            if (config.trade_replay_from_pos_anchor && stream.stream_code == StreamCode::kFortsTradeRepl) {
                deferred_trade = std::move(stream);
                continue;
            }
            if (auto error = add_listener(std::move(stream), bridge); error)
                return error;
        }
        if (!config.full_order_log_stream.settings.empty()) {
            auto composite = config.full_order_log_stream;
            composite.settings = cg::full_order_log_listener_settings(config.runtime.scheme_dir, match);
            log("full_order_log_composite_subscription", "{\"settings\":" + json_quote(composite.settings) + "}");
            if (auto error = add_listener(std::move(composite), full_order_log_events); error)
                return error;
        }
        log("full_order_log_matching_services", "{\"matching_id\":" + std::to_string(match) + ",\"session_id\":" +
                                                    std::to_string(projection.current_session_id()) + "}");
        return {};
    }
    void ensure_matching_deals() {
        if (!config.full_order_log_partitioned || !config.full_order_log_oi_handler || !matching_current ||
            now() < fol_deals_retry || std::ranges::any_of(listeners, [&](const auto& l) {
                return l.handler == config.full_order_log_oi_handler;
            }))
            return;
        const auto code = StreamCode::kFortsDealsRepl;
        if (const auto error =
                add_listener({code, match_service(cg::required_replication_listener_settings(code), *matching_id), {}},
                             *config.full_order_log_oi_handler);
            error) {
            fol_deals_retry = now() + config.recovery_retry_interval;
            log("full_order_log_advisory_unavailable",
                "{\"source\":\"deals\",\"error\":" + json_quote(error.message) + "}");
        }
    }
    bool refdata_current() const noexcept {
        if (!stream_online(StreamCode::kFortsRefdataRepl))
            return false;
        const auto listener = std::ranges::find_if(
            listeners, [](const auto& l) { return l.config.stream_code == StreamCode::kFortsRefdataRepl; });
        std::uint32_t state = Closed;
        return listener != listeners.end() && !listener->object.last_callback_error() &&
               !listener->object.state(state) && state == Active;
    }
    Plaza2Error resolve_full_order_log() {
        if (!config.full_order_log_partitioned &&
            (!config.full_order_log_handler || config.full_order_log_stream.settings.empty()))
            return {};
        const auto fence = [&] {
            matching_current = false;
            if (!config.full_order_log_partitioned && matching_id) {
                matching_id.reset();
                for (auto& listener : listeners)
                    if (listener.config.stream_code == cg::kFullOrderLogStreamCode && listener.object.is_created())
                        invalidate(listener);
            }
        };
        if (!refdata_current()) {
            matching_refdata.reset();
            fence();
            return {};
        }
        const auto day = projection.current_session_id();
        const auto token = std::pair{projection.refdata_revision(), day};
        if (matching_refdata == token || now() < matching_retry)
            return {};
        matching_refdata = token;
        const auto waiting = [&](std::string reason) {
            fence();
            log("full_order_log_matching_wait", "{\"reason\":" + json_quote(reason) + "}");
            return Plaza2Error{};
        };
        const auto fail_full = [&](std::string reason) {
            if (config.full_order_log_partitioned && matching_id)
                return waiting(std::move(reason));
            fence();
            return invalid(std::move(reason));
        };
        std::optional<std::int8_t> resolved;
        for (auto isin : config.market_data_isin_ids) {
            const auto rows = projection.instruments();
            const auto contract = std::lower_bound(rows.begin(), rows.end(), isin,
                                                   [](const auto& row, auto id) { return row.isin_id < id; });
            if (!day || contract == rows.end() || contract->isin_id != isin || contract->sess_id != day ||
                !contract->current_session_member || !contract->future_session_terms)
                return waiting("missing current committed fut_sess_contents for ISIN " + std::to_string(isin));
            if (contract->future_vcb_join_status == plaza2::private_state::FutureVcbJoinStatus::Ambiguous)
                return fail_full("FullOrderLog ambiguous fut_vcb for " + contract->base_contract_code);
            if (contract->future_vcb_join_status != plaza2::private_state::FutureVcbJoinStatus::Resolved)
                return waiting("missing committed fut_vcb for " + contract->base_contract_code);
            const auto matches =
                std::ranges::equal_range(projection.matching_map(), contract->base_contract_id, {},
                                         &plaza2::private_state::MatchingMapSnapshot::base_contract_id);
            if (matches.empty())
                return waiting("missing instr2matching_map for " + contract->base_contract_code);
            for (const auto& row : matches) {
                if (row.matching_id <= 0)
                    return fail_full("FullOrderLog matching ID is not positive");
                if (resolved && *resolved != row.matching_id)
                    return fail_full("FullOrderLog instruments span more than one matching ID");
                resolved = row.matching_id;
            }
        }
        if (!resolved)
            return waiting("no configured instrument matching");
        if (matching_id && matching_id != resolved) {
            fence();
            if (config.full_order_log_partitioned) {
                matching_id.reset();
                if (auto error = retire_matching_streams(); error)
                    return error;
            }
        }
        if (config.full_order_log_partitioned && !matching_id) {
            if (auto error = create_matching_streams(*resolved); error) {
                if (auto cleanup = retire_matching_streams(); cleanup)
                    return cleanup;
                if (error.runtime_code == 131073)
                    return error;
                matching_refdata.reset();
                matching_retry = now() + config.recovery_retry_interval;
                this->waiting(error, Plaza2RecoveryWaitState::WaitingForService, "matching streams");
                return {};
            }
        }
        if (matching_id != resolved)
            log("full_order_log_matching", "{\"matching_id\":" + std::to_string(*resolved) + "}");
        matching_id = resolved;
        matching_current = true;
        if (!config.full_order_log_partitioned && std::none_of(listeners.begin(), listeners.end(), [](const auto& l) {
                return l.config.stream_code == cg::kFullOrderLogStreamCode;
            }))
            return add_listener(config.full_order_log_stream, full_order_log_events);
        return {};
    }
    const Auxiliary* auxiliary_source(std::string_view name) const noexcept {
        const auto source = std::ranges::find_if(auxiliary, [&](const auto& entry) { return entry->name == name; });
        return source == auxiliary.end() ? nullptr : source->get();
    }
    const Listener* auxiliary_listener(const Auxiliary& source) const noexcept {
        const auto found = std::ranges::find_if(listeners, [&](const auto& l) { return l.handler == &source.data; });
        return found == listeners.end() ? nullptr : &*found;
    }
    bool auxiliary_current(const Auxiliary* source) const noexcept {
        if (!source || !source->data.online())
            return false;
        const auto* listener = auxiliary_listener(*source);
        if (!listener || listener->object.last_callback_error())
            return false;
        std::uint32_t state = Closed;
        if (connection.state(state) || state != Active)
            return false;
        return !listener->object.state(state) && state == Active;
    }
    std::string_view cod_state() const {
        if (config.full_order_log_login.empty())
            return "not_configured";
        const auto* source = auxiliary_source("cod");
        if (!auxiliary_current(source))
            return "unknown";
        const auto utc =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        return fol_auxiliary::login_cod_state(source->data, utc);
    }
    bool fresh(const Auxiliary* source) const noexcept {
        if (!source || !source->data.online() ||
            source->data.committed_at() == fol_auxiliary::Projection::Clock::time_point{})
            return false;
        const auto current = now();
        return current >= source->data.committed_at() &&
               current - source->data.committed_at() <= std::chrono::seconds(30);
    }
    std::optional<CgateFullOrderLogBalance> balance() const noexcept {
        const auto* source = auxiliary_source("part_balance");
        if (!auxiliary_current(source) || source->data.rows().size() != 1)
            return std::nullopt;
        const auto& row = source->data.rows().begin()->second;
        const auto free = cg::parse_fixed_point(value(row, "money_free"), 2, true);
        const auto blocked = cg::parse_fixed_point(value(row, "money_blocked"), 2, true);
        const auto total = cg::parse_fixed_point(value(row, "money_amount"), 2, true);
        if (!free || !blocked || !total)
            return std::nullopt;
        return CgateFullOrderLogBalance{*free, *blocked, *total};
    }
    std::string_view environment_refusal() const noexcept {
        if (!config.full_order_log_advisory &&
            !(config.mode == CgateSessionMode::Live && config.allow_orders && config.full_order_log_authority))
            return {};
        const auto* environment = auxiliary_source("spectra_environment");
        if (!auxiliary_current(environment) || environment->data.rows().empty())
            return "REFDATA Spectra environment lacks current snapshot authority";
        const auto& rows = environment->data.rows();
        const auto& row = rows.begin()->second;
        if (rows.size() != 1 || !fol_auxiliary::environment_matches(row, config.runtime.environment))
            return "REFDATA Spectra environment conflicts with configured environment";
        return {};
    }
    std::string_view account_risk_refusal() const noexcept {
        if (const auto reason = environment_refusal(); !reason.empty())
            return reason;
        if (config.full_order_log_min_free_kopecks) {
            const auto amount = balance();
            if (!amount)
                return "PART balance lacks current snapshot authority";
            if (amount->free_kopecks < *config.full_order_log_min_free_kopecks)
                return "PART free money is below the configured floor";
        }
        if (config.full_order_log_check_prohibitions) {
            const auto* source = auxiliary_source("prohibitions");
            // A complete empty snapshot is authoritative until the listener
            // loses its generation. These account streams can be quiet indefinitely.
            if (!auxiliary_current(source))
                return "account prohibition feed lacks current snapshot authority";
            // Deliberately conservative until all venue group and priority rules are supported.
            for (const auto& [key, row] : source->data.rows())
                if (value(row, "type") != "0")
                    return "account has a prohibition row";
        }
        return {};
    }
    std::string auxiliary_status() const {
        const auto risk_refusal = account_risk_refusal();
        std::string out = "{\"venue_cod\":" + json_quote(cod_state()) +
                          ",\"account_risk_refusal\":" + (risk_refusal.empty() ? "null" : json_quote(risk_refusal));
        if (config.full_order_log_partitioned)
            out += std::string(",\"matching_current\":") + (matching_current && refdata_current() ? "true" : "false");
        if (config.full_order_log_advisory && !auxiliary_source("spectra_environment"))
            out +=
                R"(,"spectra_environment":{"online":false,"stale":true,"life_num":0,"rows":[],"supported_by_client_scheme":false})";
        const auto session = projection.current_session_id();
        for (const auto& source : auxiliary) {
            if (source->name == "cod")
                continue;
            const auto& data = source->data;
            const bool current = auxiliary_current(source.get());
            const bool quiet_authority = source->name == "part_balance" || source->name == "prohibitions" ||
                                         source->name == "spectra_environment";
            const fol_auxiliary::Projection::Row* clearing_marker = nullptr;
            if (current && source->name == "clearing")
                for (const auto& [key, row] : data.rows())
                    if (data.table_name(key.first) == "sys_events" && value(row, "event_type") == "3" && session > 0 &&
                        integer_value(value(row, "sess_id")) == session && integer_value(value(row, "event_id")) > 0 &&
                        (!clearing_marker || row.revision > clearing_marker->revision))
                        clearing_marker = &row;
            const auto* listener = auxiliary_listener(*source);
            const auto& error = listener ? listener->object.last_callback_error() : source->create_error;
            out += "," + json_quote(source->name) + ":{\"online\":" + (current ? "true" : "false") +
                   ",\"stale\":" + (current && (quiet_authority || fresh(source.get())) ? "false" : "true") +
                   ",\"life_num\":" + std::to_string(data.life());
            if (error)
                out += ",\"error\":" + json_quote(error.message);
            if (source->name == "clearing")
                out += ",\"ready_event_id\":" +
                       (clearing_marker ? std::string(value(*clearing_marker, "event_id")) : "null");
            out += ",\"rows\":[";
            bool first = true;
            if (current && (source->name != "clearing" || clearing_marker))
                for (const auto& [key, row] : data.rows()) {
                    const auto table = data.table_name(key.first);
                    if (table == "sys_events" ||
                        (!value(row, "sess_id").empty() && integer_value(value(row, "sess_id")) != session))
                        continue;
                    if (clearing_marker) {
                        const auto stamp_field = table == "clr_rate"         ? "moment"
                                                 : table == "fut_sess_settl" ? "date_clr"
                                                                             : "date_money_clearing";
                        const auto stamp = integer_value(value(row, stamp_field));
                        const auto marker_time = integer_value(value(*clearing_marker, "server_time"));
                        constexpr auto seconds_per_day = 86400, moscow_offset_seconds = 10800;
                        if (row.commit_sequence > clearing_marker->commit_sequence || stamp <= 0 || marker_time <= 0 ||
                            stamp > marker_time ||
                            (stamp + moscow_offset_seconds) / seconds_per_day !=
                                (marker_time + moscow_offset_seconds) / seconds_per_day)
                            continue;
                    }
                    if (!first)
                        out += ',';
                    first = false;
                    out += "{\"table\":" + json_quote(table) + ",\"revision\":" + std::to_string(row.revision);
                    for (const auto& [field, field_value] : row.values)
                        out += "," + json_quote(field) + ":" + json_quote(field_value);
                    if (source->name == "common") {
                        const auto current = value(row, "settlement_price");
                        const auto legacy = value(row, "settlement_price_open");
                        const auto effective = current.empty() ? legacy : current;
                        if (!effective.empty() && (legacy.empty() || current.empty() || current == legacy))
                            out += ",\"effective_settlement_price\":" + json_quote(effective);
                        else if (!effective.empty())
                            out += ",\"settlement_price_conflict\":true";
                    }
                    if (source->name == "common" && config.full_order_log_authority) {
                        const auto authority = config.full_order_log_authority(
                            static_cast<std::int32_t>(integer_value(value(row, "isin_id"))));
                        const auto bid = plaza2::private_state::parse_session_decimal(value(row, "best_buy_native"));
                        const auto ask = plaza2::private_state::parse_session_decimal(value(row, "best_sell_native"));
                        if (authority.valid && authority.session_id == session && bid && ask)
                            out += std::string(",\"native_touch_equal_at_sample\":") +
                                   (authority.best_bid == bid->units && authority.best_ask == ask->units ? "true"
                                                                                                         : "false");
                    }
                    out += '}';
                }
            out += "]}";
        }
        return out + '}';
    }
    bool trading_ready(const Plaza2TradeEncodedCommand& command) const {
        for (auto code :
             {StreamCode::kFortsRefdataRepl, StreamCode::kFortsSessionstateRepl, StreamCode::kFortsInstrumentstateRepl})
            if (!stream_online(code))
                return false;
        const auto day = projection.current_session_id();
        if (!day)
            return false;
        const auto* session = projection.find_session(day);
        if (!session || !session->has_current_status || session->current_status != 1)
            return false;
        const auto* instrument = command.isin_id ? projection.find_instrument(*command.isin_id) : nullptr;
        if (!instrument)
            return false;
        const bool auction_day_add = instrument->current_status == 6 &&
                                     command.command_kind == Plaza2TradeCommandKind::AddOrder &&
                                     command.order_type == Plaza2TradeOrderType::Limit;
        return plaza2::private_state::is_outright_future(*instrument) && instrument->sess_id == day &&
               instrument->current_session_member && instrument->has_current_status &&
               instrument->current_status_refdata_bound && (instrument->current_status == 1 || auction_day_add);
    }
    bool stream_online(StreamCode code) const {
        const auto health = projection.stream_health();
        const auto found =
            std::find_if(health.begin(), health.end(), [&](const auto& s) { return s.stream_code == code; });
        return found != health.end() && found->online && found->snapshot_complete;
    }
    std::optional<PublicDealsTarget> current_public_deals_target() const {
        if (config.public_deals_stream.settings.empty() || !stream_online(StreamCode::kFortsRefdataRepl) ||
            !stream_online(StreamCode::kFortsSessionstateRepl))
            return std::nullopt;
        const auto refdata_life = projection.stream_lifenum(StreamCode::kFortsRefdataRepl);
        const auto session_life = projection.stream_lifenum(StreamCode::kFortsSessionstateRepl);
        const auto session_id = projection.current_session_id();
        if (!refdata_life || !*refdata_life || !session_life || !*session_life || session_id <= 0 ||
            (config.aggr20_target_session_id && config.aggr20_target_session_id != session_id))
            return std::nullopt;
        const auto* instrument =
            projection.find_instrument(static_cast<std::int32_t>(config.public_deals_target_isin_id));
        if (!instrument || !instrument->current_session_member || instrument->sess_id != session_id ||
            !instrument->definition_source_provenance.present ||
            instrument->definition_source_provenance.lifenum != *refdata_life)
            return std::nullopt;
        // The merged instrument can also carry a different session's row.
        // Require the exact committed current-life session membership cache.
        const auto terms = projection.find_future_session_terms(
            static_cast<std::int32_t>(config.public_deals_target_isin_id), session_id, *refdata_life);
        if (!terms || terms->isin_id != config.public_deals_target_isin_id || terms->sess_id != session_id)
            return std::nullopt;
        return PublicDealsTarget{config.public_deals_target_isin_id, session_id, *refdata_life, *session_life};
    }
    Plaza2Error retire_public_deals() {
        if (!deals)
            return {};
        // Hide the old stream before attempting native teardown. If destroy
        // fails its callback still owns this bridge; retain both and retry.
        deals_restart = true;
        const auto listener = std::find_if(listeners.begin(), listeners.end(), [](const auto& l) {
            return l.config.stream_code == StreamCode::kFortsDealsRepl;
        });
        if (listener != listeners.end()) {
            close_listener(*listener);
            if (auto error = listener->object.destroy(); error) {
                operation("listener", "destroy", error, listener->config.stream_code);
                deals_retry = now() + config.recovery_retry_interval;
                return error;
            }
            listeners.erase(listener);
        }
        const auto epoch = deals->snapshot(std::numeric_limits<std::uint64_t>::max()).stream_epoch;
        if (epoch > std::numeric_limits<std::uint64_t>::max() - deals_epoch_base)
            deals_epoch_exhausted = true;
        else
            deals_epoch_base += epoch;
        deals.reset();
        deals_target.reset();
        deals_restart = false;
        return {};
    }
    Plaza2Error synchronize_public_deals() {
        if (config.public_deals_stream.settings.empty())
            return {};
        const auto target = current_public_deals_target();
        if (now() < deals_retry)
            return {};
        if (deals && (deals_restart || !target || target != deals_target)) {
            if (auto error = retire_public_deals(); error)
                return error;
        }
        if (!target || deals)
            return {};
        if (deals_epoch_exhausted || deals_epoch_base == std::numeric_limits<std::uint64_t>::max())
            return invalid("public DEALS stream epoch exhausted");
        auto candidate = std::make_unique<cg::Plaza2PublicDealsBridge>(target->isin_id, target->session_id);
        if (auto error = add_listener(config.public_deals_stream, *candidate); error) {
            deals_retry = now() + config.recovery_retry_interval;
            return error;
        }
        deals = std::move(candidate);
        deals_target = target;
        log("public_deals_target", "{\"isin_id\":" + std::to_string(target->isin_id) +
                                       ",\"session_id\":" + std::to_string(target->session_id) +
                                       ",\"refdata_lifenum\":" + std::to_string(target->refdata_lifenum) +
                                       ",\"session_lifenum\":" + std::to_string(target->session_lifenum) + "}");
        return {};
    }
    cg::Plaza2PublicDealsSnapshot public_deals_snapshot(std::uint64_t after) const {
        if (deals_epoch_exhausted)
            return {.stream_epoch = deals_epoch_base, .error = "public DEALS stream epoch exhausted"};
        if (!deals || deals_restart || deals_target != current_public_deals_target())
            return {.stream_epoch = deals_epoch_base,
                    .error = config.public_deals_stream.settings.empty()
                                 ? std::string{}
                                 : "public DEALS is waiting for complete current target metadata"};
        auto result = deals->snapshot(after);
        if (result.stream_epoch > std::numeric_limits<std::uint64_t>::max() - deals_epoch_base)
            return {.stream_epoch = deals_epoch_base, .error = "public DEALS stream epoch exhausted"};
        result.stream_epoch += deals_epoch_base;
        for (auto& trade : result.trades) {
            if (trade.stream_epoch > std::numeric_limits<std::uint64_t>::max() - deals_epoch_base)
                return {.stream_epoch = deals_epoch_base, .error = "public DEALS stream epoch exhausted"};
            trade.stream_epoch += deals_epoch_base;
        }
        return result;
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
               anchor->trades_lifenum == pos->last_trades_lifenum &&
               projection.stream_lifenum(StreamCode::kFortsTradeRepl) ==
                   static_cast<std::uint64_t>(anchor->trades_lifenum) &&
               stream_online(StreamCode::kFortsTradeRepl);
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
        if (!set_lifenum(trade.open_settings, target.trades_lifenum))
            return invalid("TRADE open settings contain duplicate lifenum keys");
        trade.open_settings += ";rev.user_deal=" + std::to_string(target.trades_rev) +
                               ";rev.heartbeat=" + std::to_string(target.trades_rev);
        if (auto error = add_listener(std::move(trade), anchored_trade_events); error)
            return error;
        anchor = target;
        return {};
    }
    void invalidate(Listener& listener) {
        if (deals && (listener.config.stream_code == StreamCode::kFortsRefdataRepl ||
                      listener.config.stream_code == StreamCode::kFortsSessionstateRepl)) {
            // Keep the callback target alive for any already-queued native
            // events. The hidden buffer is discarded after native teardown.
            deals_restart = true;
        }
        listener.snapshot_wait.reset();
        static_cast<void>(listener.handler->on_plaza2_listener_event(
            {.kind = cg::Plaza2ListenerEventKind::Close, .stream_code = listener.config.stream_code}));
        close_listener(listener);
        if (listener.config.stream_code == cg::kFullOrderLogStreamCode && !listener.scheme_incompatible)
            static_cast<void>(listener.object.destroy());
        // A pinned schema failure must remain visible until the connection
        // resets it; this listener deliberately does not retry that schema.
        if (!listener.scheme_incompatible)
            listener.object.clear_callback_error();
        listener.retry =
            now() + (listener.config.stream_code == cg::kFullOrderLogStreamCode ? std::chrono::milliseconds(1000)
                                                                                : config.recovery_retry_interval);
    }
    void supervise_listener(Listener& listener) {
        if (listener.handler == deals.get() && (deals_restart || deals_target != current_public_deals_target()))
            return;
        std::uint32_t state = Closed;
        if (listener.scheme_incompatible)
            return;
        const bool awaits_matching = config.full_order_log_partitioned && !matching_current &&
                                     (listener.matching_scoped || partitioned_stream(listener.config.stream_code));
        if (!listener.object.is_created() && listener.config.stream_code == cg::kFullOrderLogStreamCode) {
            if (awaits_matching || !matching_id || now() < listener.retry)
                return;
            if (const auto error = listener.object.create(connection, listener.config.stream_code,
                                                          render(listener.config.settings), listener.handler);
                error) {
                last_error = error.message;
                listener.retry = now() + std::chrono::seconds(1);
                return;
            }
        }
        const auto error = listener.object.state(state);
        if (!error)
            observe(listener.observed_state, state, "listener", listener.config.stream_code);
        else
            operation("listener", "getstate", error, listener.config.stream_code);
        if (error || state == Error || listener.object.last_callback_error()) {
            if (listener.object.last_callback_error()) {
                const auto& callback_error = listener.object.last_callback_error();
                last_error = callback_error.message;
                listener.scheme_incompatible =
                    !listener.advisory && callback_error.code == Plaza2ErrorCode::IncompatibleScheme;
            } else if (error)
                last_error = error.message;
            log("listener_recovery",
                "{\"stream_code\":" + std::to_string(static_cast<std::uint32_t>(listener.config.stream_code)) +
                    ",\"cause\":" + json_quote(last_error) + "}");
            invalidate(listener);
            return;
        }
        if (listener.config.stream_code == cg::kFullOrderLogStreamCode && state == Active) {
            if (!listener.handler->needs_fresh_snapshot())
                listener.snapshot_wait.reset();
            else if (!listener.snapshot_wait)
                listener.snapshot_wait = now();
            else if (now() - *listener.snapshot_wait > std::chrono::seconds(30)) {
                log("full_order_log_snapshot_watchdog", "{\"reason\":\"fresh snapshot absent for over 30 seconds\"}");
                invalidate(listener);
                return;
            }
        }
        if (state == Closed && now() >= listener.retry) {
            if (awaits_matching || (listener.config.stream_code == cg::kFullOrderLogStreamCode && !matching_id))
                return;
            if (listener.config.stream_code == StreamCode::kFortsTradeRepl && deferred_trade && !pos_anchor_health())
                return;
            // Each new snapshot replaces only its own stream's pending/committed domain.
            if (listener.advisory)
                static_cast<void>(
                    listener.handler->on_plaza2_listener_event({.kind = cg::Plaza2ListenerEventKind::Close}));
            if (listener.handler == &bridge || listener.handler == &anchored_trade_events)
                projection.reset_stream_snapshot(listener.config.stream_code);
            if (listener.handler == &aggr)
                aggr.reset();
            if (listener.handler == deals.get())
                deals->reset();
            if (listener.handler == &anchored_trade_events)
                anchored_trade_events.opening(static_cast<std::uint64_t>(anchor->trades_lifenum));
            auto settings = render(listener.config.open_settings);
            if (config.full_order_log_partitioned && deferred_trade &&
                listener.config.stream_code == StreamCode::kFortsDealsRepl &&
                listener.handler == config.full_order_log_oi_handler) {
                const auto* pos = pos_anchor_health();
                if (!pos)
                    return;
                // Only deal is subscribed, so no heartbeat revision is requested.
                settings = "mode=snapshot+online;lifenum=" + std::to_string(pos->last_trades_lifenum) +
                           ";rev.deal=" + std::to_string(pos->last_trades_rev);
            }
            const auto e = listener.object.open(settings);
            operation("listener", "open", e, listener.config.stream_code);
            if (e) {
                last_error = e.message;
                listener.retry = now() + (listener.config.stream_code == cg::kFullOrderLogStreamCode
                                              ? std::chrono::milliseconds(1000)
                                              : config.recovery_retry_interval);
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
        if (out.publisher == Active && config.validate_order_message_layouts)
            out.publisher = publisher_layout_failure ? Error : publisher_warmed ? Active : Opening;
        out.private_active = streams_created && (!config.full_order_log_partitioned || matching_id.has_value());
        for (const auto& listener : listeners) {
            std::uint32_t state = Closed;
            static_cast<void>(listener.object.state(state));
            if (listener.object.last_callback_error())
                state = Error;
            const auto code = listener.config.stream_code;
            if (listener.advisory)
                continue;
            if (code == cg::kNoStreamCode)
                out.reply = state == Active && config.validate_order_message_layouts &&
                                    !listener.object.message_layouts_validated()
                                ? Opening
                                : state;
            else if (code == StreamCode::kFortsAggrRepl)
                out.aggr = state;
            else if (code == StreamCode::kFortsDealsRepl)
                out.public_deals = state;
            else if (code == cg::kFullOrderLogStreamCode)
                out.full_order_log = state;
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
        const auto before =
            config.collect_poll_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        collecting_poll = config.collect_poll_metrics;
        if (collecting_poll) {
            ++poll_metrics.sequence;
            poll_metrics.started_utc_ns = utc_ns();
            poll_metrics.finished_utc_ns = poll_metrics.total_ns = poll_metrics.process_ns =
                poll_metrics.blocking_process_ns = poll_metrics.observer_ns = poll_metrics.process_calls =
                    poll_metrics.unattributed_events = poll_metrics.callback_ns =
                        poll_metrics.unattributed_callback_ns = poll_metrics.unattributed_callbacks = 0;
            poll_metrics.events.clear();
            poll_metrics.callbacks.clear();
        }
        struct FinishMetrics {
            Impl& owner;
            std::chrono::steady_clock::time_point before;
            ~FinishMetrics() {
                if (owner.collecting_poll) {
                    owner.poll_metrics.total_ns = ns(std::chrono::steady_clock::now()) - ns(before);
                    owner.poll_metrics.finished_utc_ns = utc_ns();
                    owner.collecting_poll = false;
                }
            }
        } finish{*this, before};
        if (!initialized)
            return invalid("CGate session is not started");
        if (recovery.operation == Plaza2SessionOperation::Failed)
            return recovery.cause;
        replies.expire(now());
        std::uint32_t state = Closed;
        if (auto error = connection.state(state); error) {
            if (auto retire_error = retire_public_deals(); retire_error)
                last_error = retire_error.message;
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
            if (config.full_order_log_partitioned) {
                if (auto error = resolve_full_order_log(); error) {
                    recovery.operation = Plaza2SessionOperation::Failed;
                    recovery.cause = error;
                    return error;
                }
            }
            ensure_matching_deals();
            ensure_auxiliary_streams();
            if (auto error = open_anchored_trade(); error)
                last_error = error.message;
            if (auto error = synchronize_public_deals(); error)
                last_error = error.message;
            for (auto& l : listeners) {
                supervise_listener(l);
                if (full_order_log_events.fatal) {
                    recovery.operation = Plaza2SessionOperation::Failed;
                    recovery.cause = full_order_log_events.fatal;
                    return recovery.cause;
                }
                if (config.full_order_log_handler &&
                    l.object.last_callback_error().code == Plaza2ErrorCode::IncompatibleScheme &&
                    (l.handler == &bridge || l.handler == config.full_order_log_handler ||
                     l.handler == &full_order_log_events || l.handler == &anchored_trade_events)) {
                    recovery.operation = Plaza2SessionOperation::Failed;
                    recovery.cause = l.object.last_callback_error();
                    return recovery.cause;
                }
            }
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
                    publisher_warmed = false;
                    publisher_layout_failure = {};
                    const auto e = publisher.open(config.publisher_open_settings);
                    operation("publisher", "open", e);
                    if (e) {
                        last_error = e.message;
                        publisher_retry = now() + config.recovery_retry_interval;
                    } else if (const auto state_error = publisher.state(pub_state); state_error) {
                        operation("publisher", "getstate", state_error);
                        close_publisher();
                        publisher_retry = now() + config.recovery_retry_interval;
                        pub_state = Closed;
                    }
                }
                if (pub_state == Active && !publisher_warmed && !publisher_layout_failure) {
                    if (config.validate_order_message_layouts) {
                        publisher_layout_failure = publisher.validate_message_layouts(trade_request_layouts());
                        operation("publisher", "validate_layout", publisher_layout_failure);
                    }
                    const auto e =
                        publisher_layout_failure ? Plaza2Error{} : publisher.prewarm_by_message_name("AddOrder");
                    if (!publisher_layout_failure)
                        operation("publisher", "prewarm", e);
                    if (e) {
                        last_error = e.message;
                        close_publisher();
                        publisher_retry = now() + config.recovery_retry_interval;
                    } else if (!publisher_layout_failure) {
                        publisher_warmed = true;
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
            auto error = process(0, &result);
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
                // A drained eventful call must return its replies/commits to
                // the owner immediately; block only on a wholly idle call.
                if (processed == 0 && wait_for_data && config.process_timeout_ms) {
                    error = process(std::min(config.process_timeout_ms, 2u), &result);
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
        if (full_order_log_events.fatal) {
            recovery.operation = Plaza2SessionOperation::Failed;
            recovery.cause = full_order_log_events.fatal;
            return recovery.cause;
        }
        if (auto error = synchronize_public_deals(); error)
            last_error = error.message;
        if (auto error = resolve_full_order_log(); error) {
            recovery.operation = Plaza2SessionOperation::Failed;
            recovery.cause = error;
            return error;
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
        if (auto error = retire_public_deals(); error)
            return error;
        for (auto& l : listeners) {
            if (l.advisory)
                static_cast<void>(l.handler->on_plaza2_listener_event({.kind = cg::Plaza2ListenerEventKind::Close}));
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
        matching_id.reset();
        fol_deals_retry = {};
        matching_current = false;
        matching_retry = {};
        matching_refdata.reset();
        full_order_log_events.activity.reset();
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
const CgatePollMetrics& CgateSession::last_poll_metrics() const noexcept {
    return impl_->poll_metrics;
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
    return impl_->public_deals_snapshot(after);
}
std::string CgateSession::full_order_log_auxiliary_status() const {
    return impl_->auxiliary_status();
}
std::optional<CgateFullOrderLogBalance> CgateSession::full_order_log_balance() const noexcept {
    return impl_->balance();
}
std::uint64_t CgateSession::market_data_metadata_revision() const noexcept {
    return impl_->projection.metadata_revision();
}
bool CgateSession::full_order_log_entry_ready(std::int64_t isin_id) const noexcept {
    if (impl_->config.full_order_log_cancel_only)
        return false;
    if (!impl_->config.full_order_log_login.empty() && impl_->cod_state() != "disabled")
        return false;
    if (!impl_->account_risk_refusal().empty())
        return false;
    if (impl_->config.full_order_log_stream.settings.empty())
        return true;
    if (isin_id <= 0 || isin_id > INT32_MAX || !impl_->config.full_order_log_authority)
        return false;
    try {
        if (runtime_health().full_order_log != Active ||
            (impl_->config.full_order_log_partitioned && (!impl_->matching_current || !impl_->refdata_current())))
            return false;
        if (impl_->config.full_order_log_partitioned && impl_->config.allow_orders) {
            const auto life = impl_->projection.refdata_lifenum();
            const auto day = impl_->projection.current_session_id();
            const auto terms =
                life ? impl_->projection.find_future_session_terms(static_cast<std::int32_t>(isin_id), day, *life)
                     : std::nullopt;
            if (!terms || !terms->lot_volume_units)
                return false;
        }
        const auto now = impl_->now();
        const auto activity = impl_->full_order_log_events.activity;
        if (!activity || now < *activity || now - *activity > std::chrono::seconds(30))
            return false;
        const auto authority = impl_->config.full_order_log_authority(static_cast<std::int32_t>(isin_id));
        const auto utc = impl_->config.full_order_log_utc_now_ns
                             ? impl_->config.full_order_log_utc_now_ns()
                             : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        return authority.valid && !authority.crossed && authority.epoch && authority.revision &&
               authority.session_id > 0 && authority.session_id == impl_->projection.current_session_id() &&
               authority.best_bid > 0 && authority.best_bid < authority.best_ask && authority.best_bid_quantity > 0 &&
               authority.best_ask_quantity > 0 && authority.exchange_moment_ns &&
               (authority.exchange_moment_ns <= utc || authority.exchange_moment_ns - utc <= 1000000000ULL);
    } catch (...) {
        return false;
    }
}
std::optional<std::int8_t> CgateSession::full_order_log_matching_id() const noexcept {
    return impl_->matching_id;
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
bool CgateSession::publisher_prepared() const noexcept {
    return impl_->publisher_warmed;
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
    const auto admission_started = impl_->config.collect_poll_metrics ? ns(std::chrono::steady_clock::now()) : 0;
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
    // Advisory account gates below fence new exposure. Risk-reducing
    // cancellation still uses the existing transport and ownership checks.
    bool required_private = true;
    if (command.command_kind == Plaza2TradeCommandKind::AddOrder ||
        command.command_kind == Plaza2TradeCommandKind::MoveOrder) {
        const auto proof_started = impl_->config.collect_poll_metrics ? ns(std::chrono::steady_clock::now()) : 0;
        if (!impl_->config.full_order_log_login.empty() && impl_->cod_state() != "disabled") {
            out.validation_error = invalid("FullOrderLog entry requires current login COD disabled; state=" +
                                           std::string(impl_->cod_state()));
            return out;
        }
        if (const auto risk_refusal = impl_->account_risk_refusal(); !risk_refusal.empty()) {
            out.validation_error = invalid(std::string(risk_refusal));
            return out;
        }
        if (command.isin_id && !full_order_log_entry_ready(*command.isin_id)) {
            out.validation_error =
                invalid("FullOrderLog entry requires a live stream and valid committed two-sided current-session book");
            return out;
        }
        required_private = trade_replay_anchor_ready() && command.isin_id && impl_->trading_ready(command);
        if (proof_started)
            out.session_instrument_proof_ns = ns(std::chrono::steady_clock::now()) - proof_started;
        for (auto code : {StreamCode::kFortsTradeRepl, StreamCode::kFortsPosRepl, StreamCode::kFortsPartRepl,
                          StreamCode::kFortsRefdataRepl, StreamCode::kFortsSessionstateRepl,
                          StreamCode::kFortsInstrumentstateRepl}) {
            const auto found = std::find(h.private_streams.begin(), h.private_streams.begin() + h.private_count, code);
            required_private &= found != h.private_streams.begin() + h.private_count &&
                                h.private_states[found - h.private_streams.begin()] == Active &&
                                impl_->stream_online(code);
        }
    }
    if (impl_->publisher_layout_failure) {
        out.validation_error = impl_->publisher_layout_failure;
        return out;
    }
    if (impl_->recovery.operation == Plaza2SessionOperation::Failed || !impl_->config.allow_orders ||
        impl_->config.read_only_market_data || !command.validation.ok() || !user_id || h.connection != Active ||
        h.publisher != Active || !impl_->publisher_warmed || h.reply != Active || !required_private) {
        out.validation_error = invalid("order entry requires allow_orders, valid command and ONLINE private streams");
        return out;
    }
    if (impl_->replies.pending.contains(user_id)) {
        out.validation_error = invalid("user_id already reserved by a command");
        return out;
    }
    auto result = post_validated(command.command_name, command.payload, user_id, true, admission_started);
    result.session_instrument_proof_ns = out.session_instrument_proof_ns;
    return result;
}
cg::Plaza2PublisherMessageResult CgateSession::post_validated(std::string_view name, std::span<const std::byte> payload,
                                                              std::uint32_t user_id, bool need_reply,
                                                              std::uint64_t admission_started) {
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
    const auto admission_ns = admission_started ? ns(std::chrono::steady_clock::now()) - admission_started : 0;
    out = impl_->publisher.post_by_message_name(
        name, payload, user_id, need_reply, impl_->config.collect_post_metrics || impl_->config.collect_poll_metrics,
        impl_->config.collect_poll_metrics);
    out.session_admission_ns = admission_ns;
    if (out.certainty == cg::Plaza2SubmissionCertainty::DefinitelyNotSent)
        impl_->replies.pending.erase(user_id);
    return out;
}
} // namespace moex::plaza2_trade
