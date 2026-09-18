#include "moex/plaza2/cgate/plaza2_aggr20_authority_probe.hpp"

#include "moex/plaza2/cgate/plaza2_credential_provider.hpp"
#include "moex/plaza2/cgate/plaza2_manual_operator_gate.hpp"
#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace moex::plaza2::cgate {

namespace {

using generated::FieldCode;
using generated::StreamCode;
using generated::TableCode;

constexpr std::string_view kCredentialToken = "${MOEX_PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kLegacyCredentialToken = "${PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kSoftwareKeyToken = "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}";
constexpr std::string_view kRelativeSchemeToken = "|FILE|scheme/forts_scheme.ini|";

Plaza2Error invalid_config(std::string message) {
    return {
        .code = Plaza2ErrorCode::InvalidConfiguration,
        .runtime_code = 0,
        .message = std::move(message),
    };
}

bool settings_need_credentials(std::string_view value) {
    return value.find(kCredentialToken) != std::string_view::npos ||
           value.find(kLegacyCredentialToken) != std::string_view::npos;
}

bool settings_need_software_key(std::string_view value) {
    return value.find(kSoftwareKeyToken) != std::string_view::npos;
}

void replace_all(std::string& rendered, std::string_view token, std::string_view replacement) {
    std::size_t position = 0;
    while ((position = rendered.find(token, position)) != std::string::npos) {
        rendered.replace(position, token.size(), replacement);
        position += replacement.size();
    }
}

std::string resolve_stream_scheme_path(std::string_view value, const std::filesystem::path& scheme_path) {
    std::string rendered(value);
    const auto replacement = std::string("|FILE|") + scheme_path.string() + "|";
    std::size_t position = 0;
    while ((position = rendered.find(kRelativeSchemeToken, position)) != std::string::npos) {
        rendered.replace(position, kRelativeSchemeToken.size(), replacement);
        position += replacement.size();
    }
    return rendered;
}

std::string resolve_env_open_ini_path(std::string_view value, const std::filesystem::path& config_dir) {
    std::string rendered(value);
    constexpr std::string_view kPrefix = "ini=";
    const auto begin = rendered.find(kPrefix);
    if (begin == std::string::npos) {
        return rendered;
    }
    const auto value_begin = begin + kPrefix.size();
    const auto value_end = rendered.find(';', value_begin);
    const auto value_size = (value_end == std::string::npos ? rendered.size() : value_end) - value_begin;
    const auto raw_path = std::filesystem::path(rendered.substr(value_begin, value_size));
    if (raw_path.is_absolute()) {
        return rendered;
    }
    auto resolved = config_dir / raw_path;
    if (!std::filesystem::exists(resolved) && raw_path.has_parent_path() && raw_path.begin() != raw_path.end() &&
        *raw_path.begin() == "config") {
        resolved = config_dir / raw_path.filename();
    }
    rendered.replace(value_begin, value_size, resolved.string());
    return rendered;
}

bool contains_forbidden_surface(std::string_view value) {
    return value.find("FORTS_TRADE_REPL") != std::string_view::npos ||
           value.find("FORTS_USERORDERBOOK_REPL") != std::string_view::npos ||
           value.find("FORTS_POS_REPL") != std::string_view::npos ||
           value.find("FORTS_PART_REPL") != std::string_view::npos || value.find("p2mq://") != std::string_view::npos ||
           value.find("AddOrder") != std::string_view::npos || value.find("DelOrder") != std::string_view::npos;
}

std::optional<std::int64_t> signed_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger) {
            return field.signed_value;
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger &&
            field.unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(field.unsigned_value);
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> unsigned_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger || field.kind == Plaza2DecodedValueKind::Timestamp) {
            return field.unsigned_value;
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger && field.signed_value >= 0) {
            return static_cast<std::uint64_t>(field.signed_value);
        }
    }
    return std::nullopt;
}

std::string text_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::String || field.kind == Plaza2DecodedValueKind::Decimal ||
            field.kind == Plaza2DecodedValueKind::FloatingPoint) {
            return std::string(field.text_value);
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger) {
            return std::to_string(field.signed_value);
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger) {
            return std::to_string(field.unsigned_value);
        }
    }
    return {};
}

bool session_data_ready_message(std::string_view message) noexcept {
    constexpr std::string_view expected = "session_data_ready";
    if (message.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < message.size(); ++index) {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(message[index]))) != expected[index]) {
            return false;
        }
    }
    return true;
}

std::string upper_copy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return out;
}

bool is_preferred_alrs(const private_state::InstrumentSnapshot& instrument) {
    const auto symbol = upper_copy(instrument.isin);
    const auto short_symbol = upper_copy(instrument.short_isin);
    const auto base_contract = upper_copy(instrument.base_contract_code);
    return symbol.starts_with("ALRS") || short_symbol.starts_with("ALRS") || base_contract == "ALRS";
}

struct LoadedSecrets {
    std::optional<std::string> credentials;
    std::optional<std::string> software_key;
};

std::optional<LoadedSecrets> load_secrets(const Plaza2Aggr20AuthorityProbeConfig& config, std::string& error) {
    const auto needs_credentials = settings_need_credentials(config.runtime.env_open_settings) ||
                                   settings_need_credentials(config.connection_settings) ||
                                   settings_need_credentials(config.connection_open_settings) ||
                                   settings_need_credentials(config.refdata_stream.settings) ||
                                   settings_need_credentials(config.refdata_stream.open_settings) ||
                                   settings_need_credentials(config.aggr_stream.settings) ||
                                   settings_need_credentials(config.aggr_stream.open_settings);
    const auto needs_software_key = settings_need_software_key(config.runtime.env_open_settings) ||
                                    settings_need_software_key(config.connection_settings) ||
                                    settings_need_software_key(config.connection_open_settings) ||
                                    settings_need_software_key(config.refdata_stream.settings) ||
                                    settings_need_software_key(config.refdata_stream.open_settings) ||
                                    settings_need_software_key(config.aggr_stream.settings) ||
                                    settings_need_software_key(config.aggr_stream.open_settings);

    LoadedSecrets secrets;
    if (config.credentials.source != Plaza2CredentialSource::None) {
        const auto loaded = load_plaza2_credentials(config.credentials);
        if (!loaded.has_value()) {
            error = "read-only AGGR authority probe credentials are missing or empty";
            return std::nullopt;
        }
        secrets.credentials = loaded->value;
    } else if (needs_credentials) {
        error = "probe settings require PLAZA II credentials, but no credential source was configured";
        return std::nullopt;
    }

    if (config.software_key.source != Plaza2CredentialSource::None) {
        const auto loaded = load_plaza2_credentials(config.software_key);
        if (!loaded.has_value()) {
            error = "read-only AGGR authority probe software key is missing or empty";
            return std::nullopt;
        }
        secrets.software_key = loaded->value;
    } else if (needs_software_key) {
        error = "probe settings require the PLAZA II software key, but no software-key source was configured";
        return std::nullopt;
    }
    return secrets;
}

std::string render_setting(std::string_view value, const LoadedSecrets& secrets) {
    std::string rendered(value);
    if (secrets.credentials.has_value()) {
        replace_all(rendered, kCredentialToken, *secrets.credentials);
        replace_all(rendered, kLegacyCredentialToken, *secrets.credentials);
    }
    if (secrets.software_key.has_value()) {
        replace_all(rendered, kSoftwareKeyToken, *secrets.software_key);
    }
    return rendered;
}

struct ProbeAggrHandler final : Plaza2ListenerEventHandler {
    explicit ProbeAggrHandler(Plaza2Aggr20ListenerBridge& bridge) : bridge_(bridge) {}

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        if (event.kind == Plaza2ListenerEventKind::Open) {
            online_seen_ = false;
        }
        if (event.kind == Plaza2ListenerEventKind::TransactionBegin) {
            ++transaction_sequence_;
            current_transaction_id_ = transaction_sequence_;
            current_transaction_row_index_ = 0;
            current_transaction_aborted_ = false;
            current_record_indices_.clear();
        }
        if (event.kind == Plaza2ListenerEventKind::StreamData &&
            event.table_code == TableCode::kFortsAggrReplSysEvents) {
            Plaza2Aggr20AuthorityProbeSysEvent record;
            record.source_repl_id = unsigned_field(event.fields, FieldCode::kFortsAggrReplSysEventsReplId).value_or(0);
            record.source_repl_rev = signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsReplRev).value_or(0);
            record.source_repl_act = signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsReplAct).value_or(0);
            record.event_type = static_cast<std::int32_t>(
                signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsEventType).value_or(0));
            record.event_id = signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsEventId).value_or(0);
            record.sess_id = static_cast<std::int32_t>(
                signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsSessId).value_or(0));
            record.message = text_field(event.fields, FieldCode::kFortsAggrReplSysEventsMessage);
            record.server_time = unsigned_field(event.fields, FieldCode::kFortsAggrReplSysEventsServerTime).value_or(0);
            record.transaction_id = current_transaction_id_;
            record.transaction_row_index = ++current_transaction_row_index_;
            record.observed_before_online = !online_seen_;
            current_record_indices_.push_back(records_.size());
            records_.push_back(std::move(record));
        }

        if (event.kind == Plaza2ListenerEventKind::Close || event.kind == Plaza2ListenerEventKind::LifeNum ||
            event.kind == Plaza2ListenerEventKind::ClearDeleted) {
            current_transaction_aborted_ = true;
        }

        const auto error = bridge_.on_plaza2_listener_event(event);
        if (event.kind == Plaza2ListenerEventKind::TransactionCommit) {
            if (!error && !current_transaction_aborted_) {
                for (const auto index : current_record_indices_) {
                    records_[index].transaction_committed = true;
                }
            }
            current_record_indices_.clear();
            current_transaction_id_ = 0;
            current_transaction_row_index_ = 0;
            current_transaction_aborted_ = false;
        }
        if (event.kind == Plaza2ListenerEventKind::Online && !error) {
            online_seen_ = true;
        }
        return error;
    }

    [[nodiscard]] const std::vector<Plaza2Aggr20AuthorityProbeSysEvent>& records() const noexcept {
        return records_;
    }

  private:
    Plaza2Aggr20ListenerBridge& bridge_;
    std::vector<Plaza2Aggr20AuthorityProbeSysEvent> records_;
    std::vector<std::size_t> current_record_indices_;
    std::uint64_t transaction_sequence_{0};
    std::uint64_t current_transaction_id_{0};
    std::uint64_t current_transaction_row_index_{0};
    bool current_transaction_aborted_{false};
    bool online_seen_{false};
};

bool has_ready_after_online(const std::vector<Plaza2Aggr20AuthorityProbeSysEvent>& records,
                            std::int32_t expected_session_id) {
    return std::any_of(records.begin(), records.end(), [&](const auto& record) {
        return record.transaction_committed && !record.observed_before_online && record.event_type == 1 &&
               record.sess_id == expected_session_id && session_data_ready_message(record.message);
    });
}

bool attempt_completed_cleanly(const Plaza2Aggr20AuthorityProbeAttempt& attempt) noexcept {
    return attempt.listener_created && attempt.listener_opened && attempt.online && attempt.snapshot_complete &&
           !attempt.listener_last_callback_error.has_value() && attempt.error.empty();
}

std::string attempt_name(std::uint32_t ordinal) {
    return ordinal == 1 ? "INITIAL_OPEN" : "LISTENER_REOPEN";
}

void capture_listener_callback_error(Plaza2Listener& listener, Plaza2Aggr20AuthorityProbeAttempt& attempt) {
    const auto& callback_error = listener.last_callback_error();
    if (!callback_error) {
        return;
    }

    const bool changed = !attempt.listener_last_callback_error.has_value() ||
                         attempt.listener_last_callback_error->code != callback_error.code ||
                         attempt.listener_last_callback_error->runtime_code != callback_error.runtime_code ||
                         attempt.listener_last_callback_error->message != callback_error.message;
    attempt.listener_last_callback_error = callback_error;
    if (changed) {
        attempt.event_trace.push_back(
            "event=LISTENER_CALLBACK_ERROR code=" + std::to_string(static_cast<std::uint32_t>(callback_error.code)) +
            " runtime_code=" + std::to_string(callback_error.runtime_code) + " message=" + callback_error.message);
    }
}

Plaza2Aggr20AuthorityProbeAttempt
run_aggr_attempt(Plaza2Connection& connection, const Plaza2Aggr20AuthorityProbeConfig& config,
                 const std::string& aggr_settings, const std::string& aggr_open_settings,
                 const Plaza2Aggr20AuthorityProbeSelection& selection, std::uint32_t ordinal) {
    Plaza2Aggr20AuthorityProbeAttempt attempt;
    attempt.ordinal = ordinal;
    attempt.name = attempt_name(ordinal);
    attempt.event_trace.push_back("probe=listener_open attempt=" + std::to_string(ordinal) + " name=" + attempt.name);

    Plaza2Aggr20BookProjector projector;
    Plaza2Aggr20ListenerBridge bridge(projector, selection.sess_id);
    ProbeAggrHandler handler(bridge);
    bridge.set_recovery_service("FORTS_AGGR20_REPL");
    bridge.set_event_trace([&attempt](std::string line) { attempt.event_trace.push_back(std::move(line)); });

    Plaza2Listener listener;
    const auto capture_decimal_rejection = [&]() {
        if (!attempt.first_decimal_rejection.has_value() && bridge.first_decimal_rejection().has_value())
            attempt.first_decimal_rejection = bridge.first_decimal_rejection();
    };
    if (const auto error = listener.create(connection, StreamCode::kFortsAggrRepl, aggr_settings, &handler); error) {
        attempt.error = error.message;
        return attempt;
    }
    attempt.listener_created = true;
    if (const auto error = listener.open(aggr_open_settings); error) {
        attempt.error = error.message;
    } else {
        attempt.listener_opened = true;
    }
    capture_listener_callback_error(listener, attempt);
    capture_decimal_rejection();

    if (attempt.error.empty()) {
        const auto deadline = std::chrono::steady_clock::now() + config.observation_window;
        while (std::chrono::steady_clock::now() < deadline) {
            std::uint32_t runtime_code = 0;
            if (const auto error = connection.process(config.process_timeout_ms, &runtime_code); error) {
                capture_listener_callback_error(listener, attempt);
                capture_decimal_rejection();
                attempt.error = error.message + "; runtime_code=" + std::to_string(runtime_code);
                break;
            }
            capture_listener_callback_error(listener, attempt);
            capture_decimal_rejection();
            if (bridge.online() && bridge.snapshot_complete() &&
                has_ready_after_online(handler.records(), selection.sess_id)) {
                break;
            }
            if (config.process_timeout_ms == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    capture_listener_callback_error(listener, attempt);
    capture_decimal_rejection();

    attempt.sys_events = handler.records();
    attempt.online = bridge.online();
    attempt.snapshot_complete = bridge.snapshot_complete();
    attempt.session_data_ready_after_online = has_ready_after_online(attempt.sys_events, selection.sess_id);
    const auto target_snapshot = projector.snapshot_for_isin(selection.isin_id);
    attempt.target_authoritative = bridge.authoritative() && target_snapshot.has_value();

    attempt.event_trace.push_back("probe=listener_close attempt=" + std::to_string(ordinal) + " name=" + attempt.name);
    if (const auto error = listener.close(); error && attempt.error.empty()) {
        attempt.error = error.message;
    }
    capture_listener_callback_error(listener, attempt);
    capture_decimal_rejection();
    attempt.event_trace.push_back("probe=listener_close_complete attempt=" + std::to_string(ordinal) +
                                  " name=" + attempt.name);
    attempt.event_trace.push_back("probe=listener_destroy attempt=" + std::to_string(ordinal) +
                                  " name=" + attempt.name);
    const auto destroy_error = listener.destroy();
    if (destroy_error && attempt.error.empty()) {
        capture_listener_callback_error(listener, attempt);
        capture_decimal_rejection();
        attempt.error = destroy_error.message;
    }
    attempt.event_trace.push_back("probe=listener_destroy_complete attempt=" + std::to_string(ordinal) +
                                  " name=" + attempt.name);
    attempt.experiment_complete = attempt_completed_cleanly(attempt);
    return attempt;
}

Plaza2Error validate_probe_config(const Plaza2Aggr20AuthorityProbeConfig& config) {
    if (config.endpoint_host.empty() || config.endpoint_port == 0) {
        return invalid_config("endpoint_host and endpoint_port must be set for the read-only AGGR authority probe");
    }
    if (config.runtime.environment != Plaza2Environment::Test) {
        return invalid_config("the read-only AGGR authority probe is TEST-only");
    }
    if (const auto error = validate_plaza2_settings(config.runtime); error) {
        return error;
    }
    if (config.runtime.env_open_settings.empty()) {
        return invalid_config("runtime.env_open_settings must be provided explicitly");
    }
    if (config.connection_settings.empty()) {
        return invalid_config("connection_settings must be provided explicitly");
    }
    if (config.refdata_stream.stream_code != StreamCode::kFortsRefdataRepl ||
        config.aggr_stream.stream_code != StreamCode::kFortsAggrRepl) {
        return invalid_config("the probe accepts only FORTS_REFDATA_REPL and FORTS_AGGR20_REPL listeners");
    }
    if (config.refdata_stream.settings.find("FORTS_REFDATA_REPL") == std::string::npos) {
        return invalid_config("refdata_stream.settings must explicitly use FORTS_REFDATA_REPL");
    }
    if (config.aggr_stream.settings.find("FORTS_AGGR20_REPL") == std::string::npos) {
        return invalid_config("aggr_stream.settings must explicitly use FORTS_AGGR20_REPL");
    }
    if (config.refdata_stream.open_settings.find("snapshot+online") == std::string::npos ||
        config.aggr_stream.open_settings.find("snapshot+online") == std::string::npos) {
        return invalid_config("both probe listeners must be opened with mode=snapshot+online");
    }
    if (config.observation_window.count() <= 0) {
        return invalid_config("observation_window must be positive");
    }
    for (const auto value :
         {std::string_view(config.runtime.env_open_settings), std::string_view(config.connection_settings),
          std::string_view(config.connection_open_settings), std::string_view(config.refdata_stream.settings),
          std::string_view(config.refdata_stream.open_settings), std::string_view(config.aggr_stream.settings),
          std::string_view(config.aggr_stream.open_settings)}) {
        if (contains_forbidden_surface(value)) {
            return invalid_config(
                "read-only AGGR authority probe settings contain a private command or publisher surface");
        }
    }
    if (const auto transport_gate =
            Plaza2ManualOperatorGate::validate_transport_connect(config.endpoint_host, config.arm_state);
        !transport_gate.allowed) {
        return invalid_config(transport_gate.reason);
    }
    if (const auto session_gate =
            Plaza2ManualOperatorGate::validate_session_start(config.endpoint_host, config.arm_state);
        !session_gate.allowed) {
        return invalid_config(session_gate.reason);
    }
    return {};
}

std::optional<private_state::TradingSessionSnapshot>
select_active_session(const private_state::Plaza2PrivateStateProjector& projector) {
    std::vector<const private_state::TradingSessionSnapshot*> active;
    for (const auto& session : projector.sessions()) {
        if (session.state == 1) {
            active.push_back(&session);
        }
    }
    if (active.empty()) {
        return std::nullopt;
    }
    if (active.size() == 1) {
        return *active.front();
    }

    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<const private_state::TradingSessionSnapshot*> in_window;
    for (const auto* session : active) {
        if (session->begin > 0 && session->end > session->begin && session->begin <= now && now <= session->end) {
            in_window.push_back(session);
        }
    }
    if (in_window.size() != 1) {
        return std::nullopt;
    }
    return *in_window.front();
}

std::optional<Plaza2Aggr20AuthorityProbeSelection>
select_instrument(const private_state::Plaza2PrivateStateProjector& projector,
                  const private_state::TradingSessionSnapshot& session) {
    std::vector<const private_state::InstrumentSnapshot*> candidates;
    for (const auto& instrument : projector.instruments()) {
        if (instrument.kind != private_state::InstrumentKind::kFuture || instrument.isin_id <= 0 ||
            instrument.sess_id != session.sess_id || !instrument.current_session_member || instrument.state != 1 ||
            instrument.trade_mode_id == 0 || instrument.min_step.empty() || instrument.isin.empty() ||
            !instrument.future_session_terms.has_value()) {
            continue;
        }
        candidates.push_back(&instrument);
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto* lhs, const auto* rhs) {
        const auto lhs_preferred = is_preferred_alrs(*lhs);
        const auto rhs_preferred = is_preferred_alrs(*rhs);
        if (lhs_preferred != rhs_preferred) {
            return lhs_preferred > rhs_preferred;
        }
        if (lhs->isin != rhs->isin) {
            return lhs->isin < rhs->isin;
        }
        return lhs->isin_id < rhs->isin_id;
    });

    const auto& instrument = *candidates.front();
    Plaza2Aggr20AuthorityProbeSelection selection;
    selection.sess_id = session.sess_id;
    selection.isin_id = instrument.isin_id;
    selection.symbol = instrument.isin;
    selection.short_symbol = instrument.short_isin;
    selection.name = instrument.name;
    selection.base_contract_code = instrument.base_contract_code;
    selection.refdata_lifenum = projector.refdata_lifenum().value_or(0);
    selection.fut_instruments_source =
        projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, instrument.isin_id);
    selection.fut_sess_contents_source =
        projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutSessContents, instrument.isin_id);
    selection.session_source =
        projector.session_source_provenance(TableCode::kFortsRefdataReplSession, session.sess_id);
    if (selection.refdata_lifenum == 0 || !selection.fut_instruments_source.has_value() ||
        !selection.fut_sess_contents_source.has_value() || !selection.session_source.has_value() ||
        !selection.fut_instruments_source->present || !selection.fut_sess_contents_source->present ||
        !selection.session_source->present) {
        return std::nullopt;
    }
    return selection;
}

} // namespace

std::string_view plaza2_aggr20_authority_probe_result_name(Plaza2Aggr20AuthorityProbeResult result) noexcept {
    switch (result) {
    case Plaza2Aggr20AuthorityProbeResult::Yes:
        return "YES";
    case Plaza2Aggr20AuthorityProbeResult::No:
        return "NO";
    case Plaza2Aggr20AuthorityProbeResult::Inconclusive:
        return "INCONCLUSIVE";
    }
    return "INCONCLUSIVE";
}

Plaza2Aggr20AuthorityProbeResult plaza2_aggr20_authority_probe_classify_attempts(
    const std::vector<Plaza2Aggr20AuthorityProbeAttempt>& attempts) noexcept {
    if (attempts.size() != 2) {
        return Plaza2Aggr20AuthorityProbeResult::Inconclusive;
    }

    const auto& initial_open = attempts[0];
    const auto& listener_reopen = attempts[1];
    const bool initial_open_complete = attempt_completed_cleanly(initial_open);
    const bool listener_reopen_complete = attempt_completed_cleanly(listener_reopen);
    if (!initial_open_complete || !listener_reopen_complete) {
        return Plaza2Aggr20AuthorityProbeResult::Inconclusive;
    }
    if (initial_open.session_data_ready_after_online && listener_reopen.session_data_ready_after_online) {
        return Plaza2Aggr20AuthorityProbeResult::Yes;
    }
    return Plaza2Aggr20AuthorityProbeResult::No;
}

Plaza2Aggr20AuthorityProbe::Plaza2Aggr20AuthorityProbe(Plaza2Aggr20AuthorityProbeConfig config)
    : config_(std::move(config)) {}

Plaza2Aggr20AuthorityProbeReport Plaza2Aggr20AuthorityProbe::run() {
    Plaza2Aggr20AuthorityProbeReport report;
    if (const auto validation = validate_probe_config(config_); validation) {
        report.error = validation.message;
        return report;
    }

    std::string secret_error;
    const auto secrets = load_secrets(config_, secret_error);
    if (!secrets.has_value()) {
        report.error = secret_error;
        return report;
    }

    auto effective_runtime = config_.runtime;
    effective_runtime.env_open_settings = render_setting(config_.runtime.env_open_settings, *secrets);
    const auto runtime_probe = Plaza2RuntimeProbe::probe(effective_runtime);
    if (runtime_probe.compatibility == Plaza2Compatibility::Incompatible ||
        runtime_probe.compatibility == Plaza2Compatibility::Unknown) {
        report.error = "PLAZA II runtime probe is incompatible or unknown";
        return report;
    }
    if (runtime_probe.layout.scheme_path.empty()) {
        report.error = "PLAZA II runtime probe did not resolve a scheme path";
        return report;
    }

    const auto effective_connection_settings = render_setting(config_.connection_settings, *secrets);
    const auto effective_connection_open_settings = render_setting(config_.connection_open_settings, *secrets);
    const auto effective_refdata_settings = resolve_stream_scheme_path(
        render_setting(config_.refdata_stream.settings, *secrets), runtime_probe.layout.scheme_path);
    const auto effective_refdata_open_settings = render_setting(config_.refdata_stream.open_settings, *secrets);
    const auto effective_aggr_settings = resolve_stream_scheme_path(
        render_setting(config_.aggr_stream.settings, *secrets), runtime_probe.layout.scheme_path);
    const auto effective_aggr_open_settings = render_setting(config_.aggr_stream.open_settings, *secrets);
    effective_runtime.env_open_settings =
        resolve_env_open_ini_path(effective_runtime.env_open_settings, runtime_probe.layout.config_dir);

    private_state::Plaza2PrivateStateProjector refdata_projector;
    Plaza2PrivateStateBridge refdata_bridge(refdata_projector);
    const std::array refdata_streams = {StreamCode::kFortsRefdataRepl};
    if (const auto error = refdata_bridge.reset(refdata_streams); error) {
        report.error = error.message;
        return report;
    }
    if (const auto error = refdata_bridge.begin_run(); error) {
        report.error = error.message;
        return report;
    }

    Plaza2Env env;
    Plaza2Connection connection;
    Plaza2Listener refdata_listener;
    if (const auto error = env.open(effective_runtime); error) {
        report.error = error.message;
        static_cast<void>(refdata_bridge.end_run());
        return report;
    }
    if (const auto error = connection.create(env, effective_connection_settings); error) {
        report.error = error.message;
        static_cast<void>(env.close());
        static_cast<void>(refdata_bridge.end_run());
        return report;
    }
    if (const auto error = connection.open(effective_connection_open_settings); error) {
        report.error = error.message;
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        static_cast<void>(refdata_bridge.end_run());
        return report;
    }
    if (const auto error = refdata_listener.create(connection, StreamCode::kFortsRefdataRepl,
                                                   effective_refdata_settings, &refdata_bridge);
        error) {
        report.error = error.message;
        static_cast<void>(connection.close());
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        static_cast<void>(refdata_bridge.end_run());
        return report;
    }
    report.refdata_listener_created = true;
    if (const auto error = refdata_listener.open(effective_refdata_open_settings); error) {
        report.error = error.message;
        static_cast<void>(refdata_listener.destroy());
        static_cast<void>(connection.close());
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        static_cast<void>(refdata_bridge.end_run());
        return report;
    }
    report.refdata_listener_opened = true;

    bool refdata_ready = false;
    const auto refdata_deadline = std::chrono::steady_clock::now() + config_.observation_window;
    while (std::chrono::steady_clock::now() < refdata_deadline) {
        std::uint32_t runtime_code = 0;
        if (const auto error = connection.process(config_.process_timeout_ms, &runtime_code); error) {
            report.error = error.message + "; runtime_code=" + std::to_string(runtime_code);
            break;
        }
        refdata_ready = refdata_bridge.state().online && refdata_bridge.state().streams.size() == 1 &&
                        refdata_bridge.state().streams.front().snapshot_complete;
        if (refdata_ready) {
            break;
        }
        if (config_.process_timeout_ms == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    report.refdata_online = refdata_ready && refdata_bridge.state().online;
    report.refdata_snapshot_complete = refdata_ready;
    if (report.error.empty() && !refdata_ready) {
        report.error = "fresh REFDATA did not reach ONLINE and snapshot-complete within the observation window";
    }

    if (report.error.empty()) {
        const auto session = select_active_session(refdata_projector);
        if (!session.has_value()) {
            report.error = "REFDATA did not identify exactly one active session from session.state=1";
        } else {
            report.selection = select_instrument(refdata_projector, *session);
            if (!report.selection.has_value()) {
                report.error = "REFDATA did not identify a valid current futures instrument with complete provenance";
            }
        }
    }

    if (report.error.empty()) {
        for (std::uint32_t ordinal = 1; ordinal <= 2; ++ordinal) {
            auto attempt = run_aggr_attempt(connection, config_, effective_aggr_settings, effective_aggr_open_settings,
                                            *report.selection, ordinal);
            report.attempts.push_back(std::move(attempt));
        }
    }

    static_cast<void>(refdata_listener.close());
    static_cast<void>(refdata_listener.destroy());
    static_cast<void>(connection.close());
    static_cast<void>(connection.destroy());
    static_cast<void>(env.close());
    static_cast<void>(refdata_bridge.end_run());

    if (!report.error.empty()) {
        report.result = Plaza2Aggr20AuthorityProbeResult::Inconclusive;
        return report;
    }

    if (report.attempts.size() >= 1) {
        report.initial_open_session_data_ready_after_online = report.attempts[0].session_data_ready_after_online;
    }
    if (report.attempts.size() >= 2) {
        report.listener_reopen_session_data_ready_after_online = report.attempts[1].session_data_ready_after_online;
    }
    report.result = plaza2_aggr20_authority_probe_classify_attempts(report.attempts);
    report.read_only_contract_ok = true;
    return report;
}

} // namespace moex::plaza2::cgate
