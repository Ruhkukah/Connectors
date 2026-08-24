#include "moex/plaza2_trade/plaza2_test_trade_transport.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace moex::plaza2_trade {

namespace {

namespace plaza2 = ::moex::plaza2;
namespace cgate = ::moex::plaza2::cgate;
namespace private_state = ::moex::plaza2::private_state;
namespace fake = ::moex::plaza2::fake;
namespace generated = ::moex::plaza2::generated;

using cgate::Plaza2DecodedValueKind;
using cgate::Plaza2Error;
using cgate::Plaza2ErrorCode;
using cgate::Plaza2ListenerEvent;
using cgate::Plaza2ListenerEventHandler;
using cgate::Plaza2ListenerEventKind;
using cgate::Plaza2PublisherMessageResult;
using plaza2::fake::EngineState;
using plaza2::fake::EventKind;
using plaza2::fake::EventSpec;
using plaza2::fake::FieldValueSpec;
using plaza2::fake::RowSpec;
using plaza2::generated::StreamCode;

constexpr std::string_view kCredentialToken = "${MOEX_PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kLegacyCredentialToken = "${PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kSoftwareKeyToken = "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}";
constexpr std::string_view kRelativeSchemeToken = "|FILE|scheme/forts_scheme.ini|";

Plaza2Error invalid(std::string message, Plaza2ErrorCode code = Plaza2ErrorCode::InvalidConfiguration) {
    return {.code = code, .runtime_code = 0, .message = std::move(message)};
}

void replace_all(std::string& value, std::string_view token, std::string_view replacement) {
    std::size_t position = 0;
    while ((position = value.find(token, position)) != std::string::npos) {
        value.replace(position, token.size(), replacement);
        position += replacement.size();
    }
}

std::optional<std::string> load_secret(const cgate::Plaza2CredentialConfig& config) {
    if (config.source == cgate::Plaza2CredentialSource::None) {
        return std::string{};
    }
    if (config.source == cgate::Plaza2CredentialSource::Env) {
        if (config.env_var.empty()) {
            return std::nullopt;
        }
        const auto* value = std::getenv(config.env_var.c_str());
        if (value == nullptr || *value == '\0') {
            return std::nullopt;
        }
        return std::string(value);
    }
    std::ifstream input(config.file_path);
    if (!input) {
        return std::nullopt;
    }
    std::string value;
    std::getline(input, value);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::string resolve_scheme(std::string value, const std::filesystem::path& scheme_path) {
    const auto replacement = std::string("|FILE|") + scheme_path.string() + "|";
    replace_all(value, kRelativeSchemeToken, replacement);
    return value;
}

std::string resolve_ini(std::string value, const std::filesystem::path& config_dir) {
    constexpr std::string_view prefix = "ini=";
    const auto begin = value.find(prefix);
    if (begin == std::string::npos) {
        return value;
    }
    const auto value_begin = begin + prefix.size();
    const auto value_end = value.find(';', value_begin);
    const auto value_size = (value_end == std::string::npos ? value.size() : value_end) - value_begin;
    const auto raw = std::filesystem::path(value.substr(value_begin, value_size));
    if (raw.is_absolute()) {
        return value;
    }
    auto resolved = config_dir / raw;
    if (!std::filesystem::exists(resolved) && raw.has_parent_path() && raw.begin() != raw.end() &&
        *raw.begin() == "config") {
        resolved = config_dir / raw.filename();
    }
    value.replace(value_begin, value_size, resolved.string());
    return value;
}

bool loopback_connection(std::string_view settings) {
    return settings.find("127.0.0.1") != std::string_view::npos ||
           settings.find("localhost") != std::string_view::npos || settings.find("::1") != std::string_view::npos;
}

std::optional<std::int64_t> parse_scaled_decimal(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto dot = text.find('.');
    if (dot != std::string_view::npos && text.find('.', dot + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto whole = dot == std::string_view::npos ? text : text.substr(0, dot);
    const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || fraction.size() > 6 ||
        !std::all_of(whole.begin(), whole.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }) ||
        !std::all_of(fraction.begin(), fraction.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const auto ch : whole) {
        const auto digit = static_cast<std::int64_t>(ch - '0');
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    if (value > std::numeric_limits<std::int64_t>::max() / 1000000) {
        return std::nullopt;
    }
    value *= 1000000;
    std::int64_t fractional_value = 0;
    for (const auto ch : fraction) {
        fractional_value = fractional_value * 10 + static_cast<std::int64_t>(ch - '0');
    }
    for (std::size_t index = fraction.size(); index < 6; ++index) {
        fractional_value *= 10;
    }
    return value + fractional_value;
}

bool atomic_write_file(const std::filesystem::path& path, std::string_view contents, std::string& error) {
    std::error_code filesystem_error;
    if (!std::filesystem::create_directories(path.parent_path(), filesystem_error) && filesystem_error) {
        error = "failed to create execution-safety directory: " + filesystem_error.message();
        return false;
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "failed to open execution-safety temporary file";
            return false;
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            error = "failed to flush execution-safety temporary file";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        error = "failed to publish execution-safety receipt: " + filesystem_error.message();
        return false;
    }
    return true;
}

std::size_t stream_index(const EngineState& state, StreamCode code) {
    for (std::size_t index = 0; index < state.streams.size(); ++index) {
        if (state.streams[index].stream_code == code) {
            return index;
        }
    }
    return state.streams.size();
}

class PrivateProjectorBridge final : public Plaza2ListenerEventHandler {
  public:
    explicit PrivateProjectorBridge(private_state::Plaza2PrivateStateProjector& projector) : projector_(projector) {
        scenario_.scenario_id = "plaza2_test_trade_transport_private_state";
        scenario_.description = "offline fake CGate private-state bridge";
        scenario_.metadata_version = 1;
    }

    void reset(std::span<const Plaza2TestTradeStreamConfig> streams) {
        projector_.reset();
        state_ = {};
        pending_row_deltas_.assign(streams.size(), 0);
        for (const auto& stream : streams) {
            const auto* descriptor = plaza2::generated::FindStreamByCode(stream.stream_code);
            state_.streams.push_back({
                .stream_code = stream.stream_code,
                .stream_name = descriptor == nullptr ? std::string_view{} : descriptor->stream_name,
            });
        }
        callback_error_.clear();
    }

    void begin_run() {
        state_.open = true;
        state_.closed = false;
        projector_.on_event(scenario_, EventSpec{.kind = EventKind::kOpen}, state_);
    }

    void end_run() {
        state_.closed = true;
        state_.online = false;
        state_.transaction_open = false;
        for (auto& stream : state_.streams) {
            stream.online = false;
        }
        projector_.on_event(scenario_, EventSpec{.kind = EventKind::kClose}, state_);
    }

    [[nodiscard]] const std::string& callback_error() const noexcept {
        return callback_error_;
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        try {
            switch (event.kind) {
            case Plaza2ListenerEventKind::Open:
            case Plaza2ListenerEventKind::Timeout:
                return {};
            case Plaza2ListenerEventKind::Close:
                return close_stream(event.stream_code);
            case Plaza2ListenerEventKind::TransactionBegin:
                return begin_transaction(event.stream_code);
            case Plaza2ListenerEventKind::TransactionCommit:
                return commit_transaction(event.stream_code);
            case Plaza2ListenerEventKind::StreamData:
                return stream_data(event);
            case Plaza2ListenerEventKind::Online:
                return online(event.stream_code);
            case Plaza2ListenerEventKind::LifeNum:
                if (state_.has_lifenum && state_.last_lifenum == event.unsigned_value) {
                    return {};
                }
                state_.has_lifenum = true;
                state_.last_lifenum = event.unsigned_value;
                projector_.on_event(
                    scenario_, EventSpec{.kind = EventKind::kLifeNum, .numeric_value = event.unsigned_value}, state_);
                return {};
            case Plaza2ListenerEventKind::ClearDeleted:
                return clear_deleted(event.stream_code);
            case Plaza2ListenerEventKind::ReplState:
                state_.last_replstate.assign(event.text_value);
                projector_.on_event(scenario_, EventSpec{.kind = EventKind::kReplState}, state_);
                return {};
            }
        } catch (const std::exception& error) {
            callback_error_ = error.what();
            return invalid(callback_error_, Plaza2ErrorCode::CallbackFailed);
        } catch (...) {
            callback_error_ = "private-state bridge failed with an unknown exception";
            return invalid(callback_error_, Plaza2ErrorCode::CallbackFailed);
        }
        return {};
    }

  private:
    Plaza2Error ordering(std::string message) {
        callback_error_ = message;
        return invalid(std::move(message), Plaza2ErrorCode::AdapterState);
    }

    Plaza2Error close_stream(StreamCode code) {
        const auto index = stream_index(state_, code);
        if (index == state_.streams.size()) {
            return ordering("private-state close for unknown stream");
        }
        state_.streams[index].online = false;
        state_.online =
            std::any_of(state_.streams.begin(), state_.streams.end(), [](const auto& stream) { return stream.online; });
        return {};
    }

    Plaza2Error begin_transaction(StreamCode code) {
        const auto index = stream_index(state_, code);
        if (index == state_.streams.size()) {
            return ordering("private-state transaction for unknown stream");
        }
        if (state_.transaction_open) {
            return ordering("private-state nested transaction");
        }
        std::fill(pending_row_deltas_.begin(), pending_row_deltas_.end(), 0);
        state_.transaction_open = true;
        projector_.on_event(scenario_, EventSpec{.kind = EventKind::kTransactionBegin, .stream_code = code}, state_);
        return {};
    }

    Plaza2Error commit_transaction(StreamCode code) {
        const auto index = stream_index(state_, code);
        if (index == state_.streams.size() || !state_.transaction_open) {
            return ordering("private-state transaction commit without begin");
        }
        for (std::size_t stream = 0; stream < pending_row_deltas_.size(); ++stream) {
            state_.streams[stream].committed_row_count += pending_row_deltas_[stream];
        }
        std::fill(pending_row_deltas_.begin(), pending_row_deltas_.end(), 0);
        state_.transaction_open = false;
        ++state_.commit_count;
        const EventSpec event{.kind = EventKind::kTransactionCommit, .stream_code = code};
        projector_.on_event(scenario_, event, state_);
        projector_.on_transaction_commit(scenario_, event, state_);
        return {};
    }

    Plaza2Error stream_data(const Plaza2ListenerEvent& event) {
        const auto index = stream_index(state_, event.stream_code);
        if (index == state_.streams.size() || !state_.transaction_open) {
            return ordering("private-state row outside a declared transaction");
        }
        text_storage_.clear();
        fields_.clear();
        text_storage_.reserve(event.fields.size());
        fields_.reserve(event.fields.size());
        for (const auto& field : event.fields) {
            FieldValueSpec value{.field_code = field.field_code};
            switch (field.kind) {
            case Plaza2DecodedValueKind::None:
                continue;
            case Plaza2DecodedValueKind::SignedInteger:
                value.kind = plaza2::fake::ValueKind::kSignedInteger;
                value.signed_value = field.signed_value;
                break;
            case Plaza2DecodedValueKind::UnsignedInteger:
                value.kind = plaza2::fake::ValueKind::kUnsignedInteger;
                value.unsigned_value = field.unsigned_value;
                break;
            case Plaza2DecodedValueKind::Decimal:
                value.kind = plaza2::fake::ValueKind::kDecimal;
                text_storage_.emplace_back(field.text_value);
                value.text_value = text_storage_.back();
                break;
            case Plaza2DecodedValueKind::FloatingPoint:
                value.kind = plaza2::fake::ValueKind::kFloatingPoint;
                text_storage_.emplace_back(field.text_value);
                value.text_value = text_storage_.back();
                break;
            case Plaza2DecodedValueKind::String:
                value.kind = plaza2::fake::ValueKind::kString;
                text_storage_.emplace_back(field.text_value);
                value.text_value = text_storage_.back();
                break;
            case Plaza2DecodedValueKind::Timestamp:
                value.kind = plaza2::fake::ValueKind::kTimestamp;
                value.unsigned_value = field.unsigned_value;
                break;
            }
            fields_.push_back(std::move(value));
        }
        const EventSpec fake_event{
            .kind = EventKind::kStreamData, .stream_code = event.stream_code, .table_code = event.table_code};
        const RowSpec row{.stream_code = event.stream_code,
                          .table_code = event.table_code,
                          .field_count = static_cast<std::uint32_t>(fields_.size())};
        projector_.on_stream_row(scenario_, fake_event, row, fields_, state_);
        ++pending_row_deltas_[index];
        return {};
    }

    Plaza2Error online(StreamCode code) {
        const auto index = stream_index(state_, code);
        if (index == state_.streams.size()) {
            return ordering("private-state ONLINE for unknown stream");
        }
        state_.streams[index].online = true;
        state_.streams[index].snapshot_complete = true;
        state_.online = !state_.streams.empty() && std::all_of(state_.streams.begin(), state_.streams.end(),
                                                               [](const auto& stream) { return stream.online; });
        projector_.on_event(scenario_, EventSpec{.kind = EventKind::kOnline, .stream_code = code}, state_);
        return {};
    }

    Plaza2Error clear_deleted(StreamCode code) {
        const auto index = stream_index(state_, code);
        if (index == state_.streams.size()) {
            return ordering("private-state CLEARDELETED for unknown stream");
        }
        ++state_.streams[index].clear_deleted_count;
        projector_.on_event(scenario_, EventSpec{.kind = EventKind::kClearDeleted, .stream_code = code}, state_);
        return {};
    }

    private_state::Plaza2PrivateStateProjector& projector_;
    plaza2::fake::ScenarioSpec scenario_{};
    EngineState state_{};
    std::vector<std::uint64_t> pending_row_deltas_;
    std::vector<std::string> text_storage_;
    std::vector<FieldValueSpec> fields_;
    std::string callback_error_;
};

class AggrProjectorBridge final : public Plaza2ListenerEventHandler {
  public:
    explicit AggrProjectorBridge(cgate::Plaza2Aggr20BookProjector& projector) : projector_(projector) {}

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        switch (event.kind) {
        case Plaza2ListenerEventKind::TransactionBegin:
            projector_.begin_transaction();
            return {};
        case Plaza2ListenerEventKind::StreamData:
            if (event.table_code == plaza2::generated::TableCode::kFortsAggrReplOrdersAggr) {
                return projector_.on_row(event.fields);
            }
            return {};
        case Plaza2ListenerEventKind::TransactionCommit:
            return projector_.commit();
        case Plaza2ListenerEventKind::ClearDeleted:
            projector_.reset();
            return {};
        default:
            return {};
        }
    }

  private:
    cgate::Plaza2Aggr20BookProjector& projector_;
};

class ReplyBridge final : public Plaza2ListenerEventHandler {
  public:
    using Event = Plaza2TestSessionHost::ReplyEvent;

    void arm(std::uint32_t user_id, Plaza2TradeCommandKind kind) {
        active_[user_id] = kind;
    }

    void clear() {
        events_.clear();
        error_.clear();
        active_.clear();
        signatures_.clear();
    }

    [[nodiscard]] std::vector<Event> take() {
        auto events = std::move(events_);
        events_.clear();
        return events;
    }

    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        if (event.kind != Plaza2ListenerEventKind::StreamData && event.kind != Plaza2ListenerEventKind::Timeout) {
            return {};
        }
        const auto active = active_.find(event.user_id);
        if (active == active_.end()) {
            return fail("reply for an unknown active user_id");
        }
        if (event.kind == Plaza2ListenerEventKind::Timeout) {
            events_.push_back(
                {.user_id = event.user_id, .message_id = 0, .command_kind = active->second, .timed_out = true});
            return {};
        }
        const auto expected_message_id = active->second == Plaza2TradeCommandKind::AddOrder   ? 179
                                         : active->second == Plaza2TradeCommandKind::DelOrder ? 177
                                                                                              : 186;
        if (event.message_id != expected_message_id) {
            return fail("reply message family contradicts the active user_id command");
        }
        const auto signature = std::to_string(event.message_id) + ":" + cgate::plaza2_sha256_hex(event.raw_payload);
        const auto seen = signatures_.find(event.user_id);
        if (seen != signatures_.end() && seen->second != signature) {
            return fail("duplicate contradictory reply for an active user_id");
        }
        signatures_[event.user_id] = signature;
        events_.push_back({.user_id = event.user_id,
                           .message_id = event.message_id,
                           .command_kind = active->second,
                           .timed_out = false,
                           .raw_payload = std::vector<std::byte>(event.raw_payload.begin(), event.raw_payload.end())});
        return {};
    }

  private:
    Plaza2Error fail(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
        }
        return invalid(error_, Plaza2ErrorCode::CallbackFailed);
    }

    std::map<std::uint32_t, Plaza2TradeCommandKind> active_;
    std::map<std::uint32_t, std::string> signatures_;
    std::vector<Event> events_;
    std::string error_;
};

} // namespace

struct Plaza2TestSessionHost::Impl {
    explicit Impl(Plaza2TestSessionHostConfig initial)
        : config(std::move(initial)), private_bridge(private_projector), aggr_bridge(aggr_projector) {}

    Plaza2Error start() {
        if (started) {
            return invalid("TEST session host is already started", Plaza2ErrorCode::AdapterState);
        }
        if (config.runtime.environment != cgate::Plaza2Environment::Test) {
            return invalid("TEST session host refuses non-TEST environment");
        }
        if (!loopback_connection(config.connection_settings)) {
            return invalid("offline TEST session host requires a loopback connection setting");
        }
        if (config.runtime.runtime_root.empty() || config.connection_settings.empty() ||
            config.publisher_settings.empty() || config.private_streams.empty() ||
            config.aggr20_stream.settings.empty()) {
            return invalid("TEST session host requires runtime, connection, publisher, private, and AGGR20 settings");
        }
        if (config.runtime.env_open_settings.find(kCredentialToken) != std::string::npos ||
            config.runtime.env_open_settings.find(kLegacyCredentialToken) != std::string::npos) {
            return invalid("TEST session host env_open_settings must use the CGate software-key token");
        }
        probe = cgate::Plaza2RuntimeProbe::probe(config.runtime);
        probe_report = probe;
        if (probe.compatibility == cgate::Plaza2Compatibility::Incompatible ||
            probe.compatibility == cgate::Plaza2Compatibility::Unknown) {
            return invalid("TEST runtime probe is incompatible", Plaza2ErrorCode::ProbeIncompatible);
        }
        const auto credentials = load_secret(config.credentials);
        const auto software_key = load_secret(config.software_key);
        if (!credentials.has_value() || !software_key.has_value()) {
            return invalid("TEST session host secret source is missing or empty");
        }
        const auto contains_token = [](std::string_view value, std::string_view token) {
            return value.find(token) != std::string_view::npos;
        };
        const auto requires_credentials = contains_token(config.runtime.env_open_settings, kCredentialToken) ||
                                          contains_token(config.runtime.env_open_settings, kLegacyCredentialToken) ||
                                          contains_token(config.connection_settings, kCredentialToken) ||
                                          contains_token(config.publisher_settings, kCredentialToken);
        const auto requires_software_key = contains_token(config.runtime.env_open_settings, kSoftwareKeyToken) ||
                                           contains_token(config.connection_settings, kSoftwareKeyToken) ||
                                           contains_token(config.publisher_settings, kSoftwareKeyToken);
        if ((requires_credentials && credentials->empty()) || (requires_software_key && software_key->empty())) {
            return invalid("TEST session host settings require a configured secret source");
        }
        effective_runtime = config.runtime;
        render(effective_runtime.env_open_settings, credentials.value(), software_key.value());
        effective_runtime.env_open_settings = resolve_ini(effective_runtime.env_open_settings, probe.layout.config_dir);
        if (const auto error = env.open(effective_runtime); error) {
            return error;
        }
        if (const auto error = connection.create(
                env, render_copy(config.connection_settings, credentials.value(), software_key.value()));
            error) {
            return error;
        }
        if (const auto error = connection.open(config.connection_open_settings); error) {
            return error;
        }

        private_bridge.reset(config.private_streams);
        private_bridge.begin_run();
        bridge_started = true;
        reply_bridge.clear();
        aggr_projector.reset();

        private_listeners.reserve(config.private_streams.size());
        for (const auto& stream : config.private_streams) {
            private_listeners.emplace_back();
            auto& listener = private_listeners.back();
            const auto settings = resolve_scheme(
                render_copy(stream.settings, credentials.value(), software_key.value()), probe.layout.scheme_path);
            if (const auto error = listener.create(connection, stream.stream_code, settings, &private_bridge); error) {
                return error;
            }
            if (const auto error = listener.open(stream.open_settings); error) {
                return error;
            }
        }

        const auto aggr_settings =
            resolve_scheme(render_copy(config.aggr20_stream.settings, credentials.value(), software_key.value()),
                           probe.layout.scheme_path);
        if (const auto error =
                aggr_listener.create(connection, config.aggr20_stream.stream_code, aggr_settings, &aggr_bridge);
            error) {
            return error;
        }
        if (const auto error = aggr_listener.open(config.aggr20_stream.open_settings); error) {
            return error;
        }
        if (const auto error =
                reply_listener.create(connection, cgate::kNoStreamCode, "p2mqreply://;ref=PUB", &reply_bridge);
            error) {
            return error;
        }
        if (const auto error = reply_listener.open({}); error) {
            return error;
        }
        reply_listener_is_open = true;
        if (const auto error = publisher.create(
                connection, render_copy(config.publisher_settings, credentials.value(), software_key.value()));
            error) {
            return error;
        }
        if (const auto error = publisher.open(config.publisher_open_settings); error) {
            return error;
        }
        publisher_is_open = true;
        started = true;
        return {};
    }

    Plaza2Error poll() {
        if (!started) {
            return invalid("TEST session host is not started", Plaza2ErrorCode::AdapterState);
        }
        std::uint32_t runtime_code = 0;
        const auto error = connection.process(config.process_timeout_ms, &runtime_code);
        if (error) {
            if (!reply_bridge.error().empty()) {
                return invalid(reply_bridge.error(), Plaza2ErrorCode::CallbackFailed);
            }
            if (!private_bridge.callback_error().empty()) {
                return invalid(private_bridge.callback_error(), Plaza2ErrorCode::CallbackFailed);
            }
            return error;
        }
        if (!reply_bridge.error().empty()) {
            return invalid(reply_bridge.error(), Plaza2ErrorCode::CallbackFailed);
        }
        if (!private_bridge.callback_error().empty()) {
            return invalid(private_bridge.callback_error(), Plaza2ErrorCode::CallbackFailed);
        }
        return {};
    }

    Plaza2Error stop() {
        const auto has_resources = started || env.is_open() || connection.is_created() || publisher.is_created() ||
                                   reply_listener.is_created() || aggr_listener.is_created() ||
                                   !private_listeners.empty();
        if (!has_resources) {
            return {};
        }
        // Reverse dependency order: publisher/listeners, connection, then Env.
        static_cast<void>(publisher.close());
        static_cast<void>(publisher.destroy());
        publisher_is_open = false;
        static_cast<void>(reply_listener.close());
        static_cast<void>(reply_listener.destroy());
        reply_listener_is_open = false;
        static_cast<void>(aggr_listener.close());
        static_cast<void>(aggr_listener.destroy());
        for (auto it = private_listeners.rbegin(); it != private_listeners.rend(); ++it) {
            static_cast<void>(it->close());
            static_cast<void>(it->destroy());
        }
        private_listeners.clear();
        static_cast<void>(connection.close());
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        if (bridge_started) {
            private_bridge.end_run();
            bridge_started = false;
        }
        started = false;
        return {};
    }

    static void render(std::string& value, const std::string& credentials, const std::string& software_key) {
        replace_all(value, kCredentialToken, credentials);
        replace_all(value, kLegacyCredentialToken, credentials);
        replace_all(value, kSoftwareKeyToken, software_key);
    }

    static std::string render_copy(std::string value, const std::string& credentials, const std::string& software_key) {
        render(value, credentials, software_key);
        return value;
    }

    Plaza2TestSessionHostConfig config;
    cgate::Plaza2RuntimeProbeReport probe_report;
    cgate::Plaza2RuntimeProbeReport probe;
    cgate::Plaza2Settings effective_runtime;
    cgate::Plaza2Env env;
    cgate::Plaza2Connection connection;
    cgate::Plaza2Publisher publisher;
    cgate::Plaza2Listener reply_listener;
    cgate::Plaza2Listener aggr_listener;
    std::vector<cgate::Plaza2Listener> private_listeners;
    private_state::Plaza2PrivateStateProjector private_projector;
    cgate::Plaza2Aggr20BookProjector aggr_projector;
    PrivateProjectorBridge private_bridge;
    AggrProjectorBridge aggr_bridge;
    ReplyBridge reply_bridge;
    bool reply_listener_is_open{false};
    bool publisher_is_open{false};
    bool bridge_started{false};
    bool started{false};
};

Plaza2TestSessionHost::Plaza2TestSessionHost(Plaza2TestSessionHostConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Plaza2TestSessionHost::~Plaza2TestSessionHost() {
    if (impl_) {
        static_cast<void>(impl_->stop());
    }
}

Plaza2TestSessionHost::Plaza2TestSessionHost(Plaza2TestSessionHost&&) noexcept = default;
Plaza2TestSessionHost& Plaza2TestSessionHost::operator=(Plaza2TestSessionHost&&) noexcept = default;

Plaza2Error Plaza2TestSessionHost::start() {
    return impl_->start();
}
Plaza2Error Plaza2TestSessionHost::poll() {
    return impl_->poll();
}
Plaza2Error Plaza2TestSessionHost::stop() {
    return impl_->stop();
}
bool Plaza2TestSessionHost::started() const noexcept {
    return impl_->started;
}
const cgate::Plaza2RuntimeProbeReport& Plaza2TestSessionHost::probe_report() const noexcept {
    return impl_->probe_report;
}
const private_state::Plaza2PrivateStateProjector& Plaza2TestSessionHost::private_state() const noexcept {
    return impl_->private_projector;
}
const cgate::Plaza2Aggr20BookProjector& Plaza2TestSessionHost::aggr20_projector() const noexcept {
    return impl_->aggr_projector;
}
bool Plaza2TestSessionHost::p2mqreply_open() const noexcept {
    return impl_->reply_listener_is_open;
}
bool Plaza2TestSessionHost::publisher_open() const noexcept {
    return impl_->publisher_is_open;
}
std::vector<Plaza2TestSessionHost::ReplyEvent> Plaza2TestSessionHost::take_reply_events() {
    return impl_->reply_bridge.take();
}
const std::string& Plaza2TestSessionHost::last_callback_error() const noexcept {
    if (!impl_->reply_bridge.error().empty()) {
        return impl_->reply_bridge.error();
    }
    return impl_->private_bridge.callback_error();
}
Plaza2PublisherMessageResult Plaza2TestSessionHost::post(std::string_view message_name,
                                                         std::span<const std::byte> payload, std::uint32_t user_id,
                                                         bool need_reply) {
    Plaza2TradeCommandKind kind = Plaza2TradeCommandKind::AddOrder;
    if (message_name == "DelOrder") {
        kind = Plaza2TradeCommandKind::DelOrder;
    } else if (message_name == "DelUserOrders") {
        kind = Plaza2TradeCommandKind::DelUserOrders;
    }
    impl_->reply_bridge.arm(user_id, kind);
    return impl_->publisher.post_by_message_name(message_name, payload, user_id, need_reply);
}

struct Plaza2TestTradeTransport::Impl {
    explicit Impl(Plaza2TestTradeTransportConfig initial) : config(std::move(initial)), host(config.host) {}

    Plaza2Error preflight_target() {
        if (config.target_isin_id == 0) {
            return invalid("AddOrder requires an exact target isin_id");
        }
        if (config.target_session_id == 0) {
            return invalid("AddOrder requires an exact target session id");
        }
        if (const auto error = host.poll(); error) {
            return error;
        }
        const auto scoped = host.aggr20_projector().snapshot_for_isin(config.target_isin_id);
        if (!scoped.has_value() || !scoped->top_bid.has_value() || !scoped->top_ask.has_value()) {
            return invalid("target AGGR20 snapshot is missing or not two-sided");
        }
        if (std::chrono::steady_clock::now() - scoped->committed_at > config.max_aggr20_age) {
            return invalid("target AGGR20 snapshot is stale");
        }
        const auto& private_state = host.private_state();
        const auto instrument =
            std::find_if(private_state.instruments().begin(), private_state.instruments().end(),
                         [&](const auto& candidate) { return candidate.isin_id == config.target_isin_id; });
        if (instrument == private_state.instruments().end()) {
            return invalid("target instrument is absent from committed refdata");
        }
        if (instrument->kind != plaza2::private_state::InstrumentKind::kFuture || instrument->min_step.empty() ||
            instrument->trade_mode_id == 0) {
            return invalid("target instrument is not a complete futures instrument");
        }
        if (config.target_session_id != 0) {
            const auto session =
                std::find_if(private_state.sessions().begin(), private_state.sessions().end(),
                             [&](const auto& candidate) { return candidate.sess_id == config.target_session_id; });
            if (session == private_state.sessions().end() || session->state != 2) {
                return invalid("target trading session is missing or not tradable");
            }
        }
        const auto matching_limit_count =
            std::count_if(private_state.limits().begin(), private_state.limits().end(), [&](const auto& candidate) {
                return candidate.scope == plaza2::private_state::PositionScope::kClient &&
                       candidate.account_code == config.observation_client_code && candidate.limits_set;
            });
        const auto limit =
            std::find_if(private_state.limits().begin(), private_state.limits().end(), [&](const auto& candidate) {
                return candidate.scope == plaza2::private_state::PositionScope::kClient &&
                       candidate.account_code == config.observation_client_code && candidate.limits_set;
            });
        if (limit == private_state.limits().end() || matching_limit_count != 1) {
            return invalid(matching_limit_count == 0 ? "applicable committed client limit row is missing or unset"
                                                     : "multiple applicable committed client limit rows are ambiguous");
        }
        if (config.require_zero_starting_position) {
            const auto position = std::find_if(private_state.positions().begin(), private_state.positions().end(),
                                               [&](const auto& candidate) {
                                                   return candidate.isin_id == config.target_isin_id &&
                                                          candidate.account_code == config.observation_client_code;
                                               });
            if (position != private_state.positions().end() && position->xpos != 0) {
                return invalid("target starting position is not zero");
            }
        }
        const auto private_ready =
            std::all_of(private_state.stream_health().begin(), private_state.stream_health().end(),
                        [](const auto& stream) { return stream.online && stream.snapshot_complete; });
        if (!private_ready || private_state.stream_health().size() < config.host.private_streams.size()) {
            return invalid("private replication is not online and snapshot-complete for every stream");
        }
        if (!host.probe_report().trading_capable) {
            return invalid("TEST runtime is not trading-capable");
        }
        if (!host.p2mqreply_open()) {
            return invalid("p2mqreply listener is not open");
        }
        if (!host.publisher_open()) {
            return invalid("publisher is not open");
        }
        return {};
    }

    Plaza2Error persist_execution_safety_receipt() {
        if (!config.authorized_intent.has_value() || config.authorized_intent->sha256.size() != 64 ||
            !std::all_of(config.authorized_intent->sha256.begin(), config.authorized_intent->sha256.end(),
                         [](unsigned char ch) { return std::isxdigit(ch) != 0; })) {
            return invalid("AddOrder requires a static authorized intent hash");
        }
        if (config.execution_safety_receipt_path.empty()) {
            return invalid("AddOrder requires an execution-safety receipt path");
        }
        const auto scoped = host.aggr20_projector().snapshot_for_isin(config.target_isin_id);
        if (!scoped.has_value() || !scoped->top_bid.has_value() || !scoped->top_ask.has_value()) {
            return invalid("cannot persist execution-safety receipt without a two-sided target BBO");
        }
        const auto& state = host.private_state();
        const auto instrument =
            std::find_if(state.instruments().begin(), state.instruments().end(),
                         [&](const auto& candidate) { return candidate.isin_id == config.target_isin_id; });
        const auto session = std::find_if(state.sessions().begin(), state.sessions().end(), [&](const auto& candidate) {
            return candidate.sess_id == config.target_session_id;
        });
        const auto limit = std::find_if(state.limits().begin(), state.limits().end(), [&](const auto& candidate) {
            return candidate.scope == plaza2::private_state::PositionScope::kClient &&
                   candidate.account_code == config.observation_client_code && candidate.limits_set;
        });
        if (instrument == state.instruments().end() || session == state.sessions().end() ||
            limit == state.limits().end()) {
            return invalid("execution-safety receipt lacks target refdata/session/limit evidence");
        }

        Plaza2ExecutionSafetyReceipt receipt;
        receipt.authorized_intent_sha256 = config.authorized_intent->sha256;
        receipt.target_isin_id = config.target_isin_id;
        receipt.aggr20 = scoped;
        receipt.instrument = *instrument;
        receipt.session = *session;
        receipt.limit = *limit;
        receipt.local_age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                  scoped->committed_at);
        if (receipt.local_age > config.max_aggr20_age) {
            return invalid("execution-safety receipt observed a stale target AGGR20 snapshot");
        }
        receipt.quantity_one = config.authorized_intent->quantity == 1;
        receipt.private_streams_ready =
            std::all_of(state.stream_health().begin(), state.stream_health().end(),
                        [](const auto& stream) { return stream.online && stream.snapshot_complete; });
        receipt.p2mqreply_open = host.p2mqreply_open();
        receipt.publisher_open = host.publisher_open();
        receipt.trading_capable = host.probe_report().trading_capable;

        const auto price = parse_scaled_decimal(config.target_price);
        const auto tick = parse_scaled_decimal(config.target_tick_size);
        if (price.has_value() && tick.has_value() && *tick > 0) {
            receipt.passive_non_marketable = config.target_side == Plaza2TradeSide::Buy
                                                 ? *price < scoped->top_ask->price_scaled
                                                 : *price > scoped->top_bid->price_scaled;
            const auto distance = config.target_side == Plaza2TradeSide::Buy
                                      ? std::max<std::int64_t>(0, scoped->top_bid->price_scaled - *price)
                                      : std::max<std::int64_t>(0, *price - scoped->top_ask->price_scaled);
            receipt.bbo_distance_allowed = distance % *tick == 0 && static_cast<std::uint64_t>(distance / *tick) <=
                                                                        config.target_max_distance_ticks;
        }

        std::ostringstream json;
        json << "{\n"
             << "  \"schema\": \"moex.plaza2.execution_safety.v1\",\n"
             << "  \"authorized_intent_sha256\": \"" << receipt.authorized_intent_sha256 << "\",\n"
             << "  \"target_isin_id\": " << receipt.target_isin_id << ",\n"
             << "  \"top_bid\": " << scoped->top_bid->price_scaled << ",\n"
             << "  \"top_ask\": " << scoped->top_ask->price_scaled << ",\n"
             << "  \"aggr20_repl_id\": " << scoped->last_repl_id << ",\n"
             << "  \"aggr20_repl_rev\": " << scoped->last_repl_rev << ",\n"
             << "  \"local_age_ms\": " << receipt.local_age.count() << ",\n"
             << "  \"exchange_moment\": " << scoped->exchange_moment << ",\n"
             << "  \"exchange_moment_ns\": " << scoped->exchange_moment_ns << ",\n"
             << "  \"session_id\": " << session->sess_id << ",\n"
             << "  \"session_state\": " << session->state << ",\n"
             << "  \"instrument_kind\": " << static_cast<int>(instrument->kind) << ",\n"
             << "  \"instrument_min_step\": \"" << instrument->min_step << "\",\n"
             << "  \"limits_account_sha256\": \"" << cgate::plaza2_sha256_hex(limit->account_code) << "\",\n"
             << "  \"passive_non_marketable\": " << (receipt.passive_non_marketable ? "true" : "false") << ",\n"
             << "  \"bbo_distance_allowed\": " << (receipt.bbo_distance_allowed ? "true" : "false") << ",\n"
             << "  \"quantity_one\": " << (receipt.quantity_one ? "true" : "false") << ",\n"
             << "  \"private_streams_ready\": " << (receipt.private_streams_ready ? "true" : "false") << ",\n"
             << "  \"p2mqreply_open\": " << (receipt.p2mqreply_open ? "true" : "false") << ",\n"
             << "  \"publisher_open\": " << (receipt.publisher_open ? "true" : "false") << ",\n"
             << "  \"trading_capable\": " << (receipt.trading_capable ? "true" : "false") << "\n"
             << "}\n";
        receipt.canonical_json = json.str();
        receipt.sha256 = cgate::plaza2_sha256_hex(receipt.canonical_json);
        if (!receipt.passive_non_marketable || !receipt.bbo_distance_allowed || !receipt.quantity_one ||
            !receipt.private_streams_ready || !receipt.p2mqreply_open || !receipt.publisher_open ||
            !receipt.trading_capable) {
            return invalid("execution-safety receipt rejected the current target conditions");
        }
        std::string error;
        if (!atomic_write_file(config.execution_safety_receipt_path, receipt.canonical_json, error)) {
            return invalid(error, cgate::Plaza2ErrorCode::RuntimeCallFailed);
        }
        last_receipt = std::move(receipt);
        return {};
    }

    Plaza2PublisherMessageResult submit(const Plaza2TradeEncodedCommand& command, std::uint32_t user_id) {
        if (!command.validation.ok() || command.command_name.empty()) {
            Plaza2PublisherMessageResult result;
            result.validation_error = {.code = cgate::Plaza2ErrorCode::InvalidConfiguration,
                                       .message = "invalid trade command cannot be posted"};
            return result;
        }
        if (config.authorized_intent.has_value()) {
            const auto& intent = *config.authorized_intent;
            const auto expected_hash =
                command.command_kind == Plaza2TradeCommandKind::AddOrder        ? intent.add_payload_sha256
                : command.command_kind == Plaza2TradeCommandKind::DelUserOrders ? intent.recovery_payload_sha256
                                                                                : std::string{};
            if (!expected_hash.empty() && cgate::plaza2_sha256_hex(command.payload) != expected_hash) {
                Plaza2PublisherMessageResult result;
                result.validation_error = invalid("encoded command does not match the authorized payload hash");
                return result;
            }
        }
        if (!host.started()) {
            const auto start_error = host.start();
            if (start_error) {
                Plaza2PublisherMessageResult result;
                result.validation_error = start_error;
                return result;
            }
        }
        if (command.command_kind == Plaza2TradeCommandKind::AddOrder) {
            if (const auto preflight = preflight_target(); preflight) {
                Plaza2PublisherMessageResult result;
                result.validation_error = preflight;
                return result;
            }
            if (const auto receipt = persist_execution_safety_receipt(); receipt) {
                Plaza2PublisherMessageResult result;
                result.validation_error = receipt;
                return result;
            }
        }
        return host.post(command.command_name, command.payload, user_id, true);
    }

    OrderLifecyclePollResult collect(bool process) {
        OrderLifecyclePollResult result;
        if (!host.started()) {
            const auto start_error = host.start();
            if (start_error) {
                result.ok = false;
                result.error = start_error.message;
                return result;
            }
        }
        if (process) {
            const auto error = host.poll();
            if (error) {
                result.ok = false;
                result.error = error.message;
            }
        }
        const Plaza2TradeCodec codec;
        for (const auto& event : host.take_reply_events()) {
            OrderReplyObservation reply;
            reply.user_id = event.user_id;
            reply.timed_out = event.timed_out;
            if (event.message_id != 179 && event.message_id != 177 && event.message_id != 186 &&
                event.message_id != 0) {
                result.ok = false;
                result.error = "reply message family is not allowed for the active user_id";
                continue;
            }
            reply.command_kind = event.command_kind;
            if (!event.timed_out) {
                Plaza2TradeValidationResult validation;
                const auto decoded = codec.decode_reply(event.message_id, event.raw_payload, validation);
                if (!validation.ok()) {
                    result.ok = false;
                    result.error = "malformed PLAZA trade reply: " + validation.message;
                    continue;
                }
                reply.code = decoded.code;
                reply.accepted = decoded.status == Plaza2TradeReplyStatusCategory::Accepted;
                reply.order_id = decoded.order_id;
            }
            result.replies.push_back(reply);
        }
        if (config.observation_ext_id != 0) {
            const auto observation = observe_order(
                config.observation_ext_id, config.observation_client_code, config.observation_side,
                config.observation_quantity, host.private_state().own_orders(), host.private_state().own_trades());
            if (observation.has_value()) {
                result.observations.push_back(*observation);
            }
        }
        return result;
    }

    Plaza2TestTradeTransportConfig config;
    Plaza2TestSessionHost host;
    std::optional<Plaza2ExecutionSafetyReceipt> last_receipt;
};

Plaza2TestTradeTransport::Plaza2TestTradeTransport(Plaza2TestTradeTransportConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
Plaza2TestTradeTransport::~Plaza2TestTradeTransport() = default;
Plaza2TestTradeTransport::Plaza2TestTradeTransport(Plaza2TestTradeTransport&&) noexcept = default;
Plaza2TestTradeTransport& Plaza2TestTradeTransport::operator=(Plaza2TestTradeTransport&&) noexcept = default;

Plaza2PublisherMessageResult Plaza2TestTradeTransport::post(const Plaza2TradeEncodedCommand& command,
                                                            std::uint32_t user_id) {
    if (command.command_kind == Plaza2TradeCommandKind::DelUserOrders) {
        Plaza2PublisherMessageResult result;
        result.validation_error = invalid("exact-ext recovery must use post_exact_ext_id_recovery");
        return result;
    }
    return impl_->submit(command, user_id);
}
Plaza2PublisherMessageResult
Plaza2TestTradeTransport::post_exact_ext_id_recovery(const Plaza2TradeEncodedCommand& command, std::uint32_t user_id) {
    if (command.command_kind != Plaza2TradeCommandKind::DelUserOrders) {
        Plaza2PublisherMessageResult result;
        result.validation_error = invalid("post_exact_ext_id_recovery requires a DelUserOrders command");
        return result;
    }
    return impl_->submit(command, user_id);
}
OrderLifecyclePollResult Plaza2TestTradeTransport::poll(std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline) {
        return {.ok = true, .deadline_reached = true};
    }
    return impl_->collect(true);
}
OrderLifecyclePollResult Plaza2TestTradeTransport::reconcile() {
    return impl_->collect(true);
}
const Plaza2TestSessionHost& Plaza2TestTradeTransport::host() const noexcept {
    return impl_->host;
}
Plaza2TestSessionHost& Plaza2TestTradeTransport::host() noexcept {
    return impl_->host;
}
const std::optional<Plaza2ExecutionSafetyReceipt>&
Plaza2TestTradeTransport::last_execution_safety_receipt() const noexcept {
    return impl_->last_receipt;
}

} // namespace moex::plaza2_trade
