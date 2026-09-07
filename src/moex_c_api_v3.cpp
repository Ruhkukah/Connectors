#include "adapters/alorengine_capi/moex_c_api_v3.h"

#include "moex/connector_host/operator_config.hpp"
#include "moex/plaza2_trade/plaza2_order_lifecycle.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct MoexConnectorHostV3Tag {
    moex::connector_host::Plaza2HostConfigInputs inputs;
    std::unique_ptr<moex::connector_host::ConnectorHost> host;
    std::optional<moex::plaza2_trade::PreSendPlan> cached_plan;
    std::optional<moex::connector_host::ConnectorHostOrderRequest> cached_request;
};

namespace {
using moex::connector_host::ConnectorHost;
using moex::connector_host::ConnectorHostOrderRequest;
using moex::connector_host::HostPurpose;
using moex::connector_host::Plaza2HostConfigInputs;
namespace cg = moex::plaza2::cgate;
namespace trade = moex::plaza2_trade;

bool valid_header(std::uint32_t size, std::uint16_t version, std::size_t expected) noexcept {
    return version == MOEX_C_ABI_V3_VERSION && size >= expected;
}

bool required_string(const char* value) noexcept {
    return value != nullptr && *value != '\0';
}

std::string copy_string(const char* value) {
    return value == nullptr ? std::string{} : std::string(value);
}

const char* environment_value(const char* name) {
    if (!required_string(name))
        return nullptr;
    const auto* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

MoexResult map_error(const cg::Plaza2Error& error) noexcept {
    if (!error)
        return MOEX_RESULT_OK;
    switch (error.code) {
    case cg::Plaza2ErrorCode::InvalidConfiguration:
        return MOEX_RESULT_INVALID_ARGUMENT;
    case cg::Plaza2ErrorCode::MissingRuntime:
    case cg::Plaza2ErrorCode::SymbolLoadFailed:
    case cg::Plaza2ErrorCode::ProbeIncompatible:
    case cg::Plaza2ErrorCode::RuntimeCallFailed:
    case cg::Plaza2ErrorCode::DecodeFailed:
    case cg::Plaza2ErrorCode::CallbackFailed:
        return MOEX_RESULT_UNAVAILABLE;
    case cg::Plaza2ErrorCode::AdapterState:
        return MOEX_RESULT_NOT_STARTED;
    case cg::Plaza2ErrorCode::SendDisabledPreSendPhase:
        return MOEX_RESULT_NOT_SUPPORTED;
    default:
        return MOEX_RESULT_INTERNAL_ERROR;
    }
}

template <std::size_t N> bool copy_text(char (&destination)[N], std::string_view value) noexcept {
    const auto count = std::min(value.size(), N == 0 ? 0U : N - 1U);
    if (count != 0)
        std::memcpy(destination, value.data(), count);
    if (N != 0)
        destination[count] = '\0';
    return count == value.size();
}

template <typename T> void initialize_header(T& value) noexcept {
    value.struct_size = static_cast<std::uint32_t>(sizeof(T));
    value.abi_version = static_cast<std::uint16_t>(MOEX_C_ABI_V3_VERSION);
}

MoexConnectorHostV3Tag* checked_handle(MoexConnectorHostHandleV3 handle) noexcept {
    return reinterpret_cast<MoexConnectorHostV3Tag*>(handle);
}

MoexV3TargetProvenance translate_provenance(const moex::plaza2::private_state::SourceRowProvenance& row,
                                            std::int64_t typed_key) noexcept {
    MoexV3TargetProvenance out{};
    out.stream_code = static_cast<std::uint32_t>(row.stream_code);
    out.table_code = static_cast<std::uint32_t>(row.table_code);
    out.repl_rev = row.repl_rev;
    out.lifenum = row.lifenum;
    out.present = row.present ? 1U : 0U;
    out.typed_row_key = typed_key;
    return out;
}

void translate_reply(const std::optional<trade::OrderReplyObservation>& value, MoexV3ReplyInfo& out) noexcept {
    if (!value)
        return;
    out.present = 1U;
    out.accepted = value->accepted ? 1U : 0U;
    out.timed_out = value->timed_out ? 1U : 0U;
    out.order_id_present = value->order_id.has_value() ? 1U : 0U;
    out.code = value->code;
    out.order_id = value->order_id.value_or(0);
}

void translate_submission(const cg::Plaza2PublisherMessageResult& value, MoexV3SubmissionInfo& out) noexcept {
    out.certainty = static_cast<std::uint32_t>(value.certainty);
    out.post_invoked = value.post_invoked ? 1U : 0U;
}

void translate_snapshot(const MoexConnectorHostV3Tag& handle, MoexPersistentSnapshotV3& out) noexcept {
    const auto snapshot = handle.host->snapshot();
    initialize_header(out);
    out.host_state = static_cast<std::uint32_t>(snapshot.state);
    out.purpose = static_cast<std::uint32_t>(handle.inputs.purpose);
    out.environment = static_cast<std::uint32_t>(snapshot.environment);
    out.transport_mode = static_cast<std::uint32_t>(snapshot.mode);
    (void)copy_text(out.runtime_compatibility, snapshot.runtime_compatibility);
    (void)copy_text(out.runtime_scheme_sha256, snapshot.runtime_scheme_sha256);
    out.publisher_ready = snapshot.publisher_ready ? 1U : 0U;
    out.reply_ready = snapshot.reply_ready ? 1U : 0U;
    out.private_streams_ready = snapshot.private_streams_ready ? 1U : 0U;
    out.observation_ready = snapshot.observation_ready ? 1U : 0U;
    out.target_refdata_provenance_ready = snapshot.target_refdata_provenance_ready ? 1U : 0U;
    out.target_aggr20_uncrossed = snapshot.target_aggr20_uncrossed ? 1U : 0U;
    out.uob_periodic_consistent = snapshot.uob_periodic_consistent ? 1U : 0U;
    out.zero_starting_position_proven = snapshot.zero_starting_position_proven ? 1U : 0U;
    out.target_isin_id = snapshot.target_isin_id;
    out.session_id = snapshot.session_id;
    (void)copy_text(out.target, snapshot.target);
    (void)copy_text(out.min_step, snapshot.min_step);
    out.session_status_present = snapshot.session_status.has_value() ? 1U : 0U;
    out.instrument_status_present = snapshot.instrument_status.has_value() ? 1U : 0U;
    out.trade_anchor_present = snapshot.trade_anchor.has_value() ? 1U : 0U;
    out.lifecycle_state_present = snapshot.lifecycle_state.has_value() ? 1U : 0U;
    out.session_status = snapshot.session_status.value_or(0);
    out.instrument_status = snapshot.instrument_status.value_or(0);
    (void)copy_text(out.bid, snapshot.bid);
    (void)copy_text(out.ask, snapshot.ask);
    out.bbo_age_ms = snapshot.bbo_age_ms;
    out.refdata_lifenum = snapshot.refdata_lifenum;
    out.fut_instruments_provenance = translate_provenance(snapshot.fut_instruments_provenance, snapshot.target_isin_id);
    out.fut_sess_contents_provenance =
        translate_provenance(snapshot.fut_sess_contents_provenance, snapshot.target_isin_id);
    out.session_provenance = translate_provenance(snapshot.session_provenance, snapshot.session_id);
    out.position_evidence_class = static_cast<std::uint32_t>(snapshot.position_evidence_class);
    out.pos_trades_rev = snapshot.pos_trades_rev;
    out.pos_trades_lifenum = snapshot.pos_trades_lifenum;
    if (snapshot.trade_anchor) {
        out.trade_anchor_trades_rev = snapshot.trade_anchor->trades_rev;
        out.trade_anchor_trades_lifenum = snapshot.trade_anchor->trades_lifenum;
        out.trade_anchor_server_time = snapshot.trade_anchor->server_time;
    }
    out.trade_replay_complete = snapshot.trade_replay_complete ? 1U : 0U;
    out.limits_set = snapshot.limits_set ? 1U : 0U;
    out.active_own_order_count = snapshot.active_own_order_count;
    out.order_epoch_active = snapshot.order_epoch_active ? 1U : 0U;
    out.order_authorized = snapshot.order_authorized ? 1U : 0U;
    out.order_submission_attempted = snapshot.order_submission_attempted ? 1U : 0U;
    out.new_order_allowed = snapshot.new_order_allowed ? 1U : 0U;
    out.lifecycle_state = snapshot.lifecycle_state ? static_cast<std::uint32_t>(*snapshot.lifecycle_state) : 0U;
    out.order_id = snapshot.order_id;
    out.original_quantity = snapshot.original_quantity;
    out.remaining_quantity = snapshot.remaining_quantity;
    out.executed_quantity = snapshot.executed_quantity;
    out.market_safe = snapshot.market_safe ? 1U : 0U;
    out.evidence_consistent = snapshot.evidence_consistent ? 1U : 0U;
    translate_reply(snapshot.add_reply, out.add_reply);
    translate_reply(snapshot.cancel_reply, out.cancel_reply);
    out.cg_pub_msgnew = snapshot.publisher_calls.msgnew;
    out.cg_pub_post = snapshot.publisher_calls.post;
    out.stream_count = static_cast<std::uint32_t>(std::min(snapshot.streams.size(), std::size_t(MOEX_V3_MAX_STREAMS)));
    for (std::size_t i = 0; i < out.stream_count; ++i) {
        const auto& source = snapshot.streams[i];
        auto& target = out.streams[i];
        target.stream_code = static_cast<std::uint32_t>(source.stream_code);
        target.online = source.online ? 1U : 0U;
        target.snapshot_complete = source.snapshot_complete ? 1U : 0U;
        target.periodic_snapshot_consistent = source.periodic_snapshot_consistent ? 1U : 0U;
        target.has_publication_state = source.has_publication_state ? 1U : 0U;
        target.committed_row_count = source.committed_row_count;
        target.last_commit_sequence = source.last_commit_sequence;
        target.publication_state = source.publication_state;
    }
    (void)copy_text(out.last_error, snapshot.last_error);
}

void translate_plan(const MoexConnectorHostV3Tag& handle, const trade::PreSendPlan& plan,
                    MoexPersistentPlanInfoV3& out) noexcept {
    initialize_header(out);
    const auto& request = *handle.cached_request;
    out.ok = plan.ok ? 1U : 0U;
    out.failure = static_cast<std::uint32_t>(plan.failure);
    out.side = static_cast<std::uint32_t>(request.side);
    out.quantity = request.quantity;
    (void)copy_text(out.price, request.price);
    (void)copy_text(out.plan_sha256, plan.sha256);
    (void)copy_text(out.add_payload_sha256, cg::plaza2_sha256_hex(plan.add_command.payload));
    (void)copy_text(out.recovery_payload_sha256, cg::plaza2_sha256_hex(plan.exact_ext_id_recovery_command.payload));
    out.canonical_size = static_cast<std::uint32_t>(std::min<std::size_t>(plan.canonical_json.size(), UINT32_MAX));
    (void)copy_text(out.message, plan.message);
}

void translate_order_result(const trade::OrderLifecycleResult& value, MoexPersistentOrderResultV3& out) noexcept {
    initialize_header(out);
    out.ok = value.ok ? 1U : 0U;
    out.market_safe_terminal = value.market_safe_terminal ? 1U : 0U;
    out.journal_ok = value.journal_ok ? 1U : 0U;
    out.journal_degraded = value.journal_degraded ? 1U : 0U;
    out.evidence_consistent = value.evidence_consistent ? 1U : 0U;
    out.orphan_incident_written = value.orphan_incident_written ? 1U : 0U;
    out.lifecycle_state = static_cast<std::uint32_t>(value.state);
    if (value.observation) {
        const auto& row = *value.observation;
        out.order_id = row.public_order_id != 0 ? row.public_order_id : row.private_order_id;
        out.original_quantity = row.original_quantity;
        out.remaining_quantity = row.remaining_quantity;
        out.executed_quantity = row.executed_quantity;
    }
    translate_submission(value.add_submission, out.add_submission);
    translate_submission(value.cancel_submission, out.cancel_submission);
    translate_submission(value.recovery_submission, out.recovery_submission);
    translate_reply(value.add_reply, out.add_reply);
    translate_reply(value.cancel_reply, out.cancel_reply);
    translate_reply(value.recovery_reply, out.recovery_reply);
    (void)copy_text(out.journal_path, value.journal_path.string());
    (void)copy_text(out.message, value.message);
}

void translate_reconciliation(const trade::RestartReconciliationResult& value,
                              MoexPersistentReconciliationResultV3& out) noexcept {
    initialize_header(out);
    out.ok = value.ok ? 1U : 0U;
    out.run_found = value.run_found ? 1U : 0U;
    out.resolved = value.resolved ? 1U : 0U;
    out.locks_retained = value.locks_retained ? 1U : 0U;
    out.lifecycle_state = static_cast<std::uint32_t>(value.state);
    (void)copy_text(out.journal_path, value.journal_path.string());
    (void)copy_text(out.message, value.message);
}

bool request_from(const MoexPersistentOrderRequestV3& input, ConnectorHostOrderRequest& output) {
    if ((input.side != 1U && input.side != 2U) || input.quantity != 1 || !required_string(input.price) ||
        !required_string(input.base_contract_code))
        return false;
    output.side = input.side == 1U ? trade::Plaza2TradeSide::Buy : trade::Plaza2TradeSide::Sell;
    output.price = copy_string(input.price);
    output.base_contract_code = copy_string(input.base_contract_code);
    output.comment = copy_string(input.comment);
    output.quantity = input.quantity;
    return true;
}

bool same_request(const ConnectorHostOrderRequest& left, const ConnectorHostOrderRequest& right) noexcept {
    return left.side == right.side && left.price == right.price &&
           left.base_contract_code == right.base_contract_code && left.comment == right.comment &&
           left.quantity == right.quantity;
}

template <typename Function> MoexResult guard(Function&& function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return MOEX_RESULT_INTERNAL_ERROR;
    } catch (...) {
        return MOEX_RESULT_INTERNAL_ERROR;
    }
}
} // namespace

extern "C" {

uint32_t moex_v3_abi_version(void) {
    return MOEX_C_ABI_V3_VERSION;
}

MoexResult moex_v3_create_host(const MoexConnectorHostCreateParamsV3* params, MoexConnectorHostHandleV3* out_handle) {
    return guard([&]() -> MoexResult {
        if (params == nullptr || out_handle == nullptr)
            return MOEX_RESULT_INVALID_ARGUMENT;
        *out_handle = nullptr;
        if (!valid_header(params->struct_size, params->abi_version, sizeof(*params)))
            return MOEX_RESULT_INVALID_ARGUMENT;
        const char* const required[] = {
            params->runtime_root,         params->scheme_dir,          params->config_dir,
            params->env_settings_env_var, params->broker_code_env_var, params->client_code_env_var,
            params->base_run_id,          params->journal_root,        params->receipt_path,
            params->profile_id,           params->profile_fingerprint};
        for (const auto* value : required)
            if (!required_string(value))
                return MOEX_RESULT_INVALID_ARGUMENT;
        if (params->isin_id <= 0 || params->session_id <= 0 || params->base_ext_id <= 0 ||
            params->base_add_user_id == 0 || params->base_cancel_user_id == 0 || params->base_recovery_user_id == 0)
            return MOEX_RESULT_INVALID_ARGUMENT;
        const auto* env_settings = environment_value(params->env_settings_env_var);
        const auto* broker_code = environment_value(params->broker_code_env_var);
        const auto* client_code = environment_value(params->client_code_env_var);
        if (env_settings == nullptr || broker_code == nullptr || client_code == nullptr)
            return MOEX_RESULT_INVALID_ARGUMENT;

        auto value = std::make_unique<MoexConnectorHostV3Tag>();
        auto& inputs = value->inputs;
        inputs.purpose = HostPurpose::OrderTest;
        inputs.runtime_root = copy_string(params->runtime_root);
        inputs.library_path = copy_string(params->library_path);
        inputs.scheme_dir = copy_string(params->scheme_dir);
        inputs.config_dir = copy_string(params->config_dir);
        inputs.env_open_settings = env_settings;
        inputs.credentials_env_var = required_string(params->credentials_env_var)
                                         ? copy_string(params->credentials_env_var)
                                         : "MOEX_PLAZA2_TEST_CREDENTIALS";
        inputs.software_key_env_var = required_string(params->software_key_env_var)
                                          ? copy_string(params->software_key_env_var)
                                          : "MOEX_PLAZA2_CGATE_SOFTWARE_KEY";
        inputs.broker_code = broker_code;
        inputs.client_code = client_code;
        inputs.expected_spectra_release =
            required_string(params->expected_release) ? copy_string(params->expected_release) : "SPECTRA9.9.0";
        inputs.expected_scheme_sha256 = copy_string(params->expected_scheme_sha256);
        inputs.isin_id = params->isin_id;
        inputs.session_id = params->session_id;
        inputs.arm_state = {.test_network_armed = params->armed_test_network != 0,
                            .test_session_armed = params->armed_test_session != 0,
                            .test_plaza2_armed = params->armed_test_plaza2 != 0,
                            .test_order_send_armed = params->armed_test_order_send != 0};
        // The config builder currently needs an OrderLifecycleConfig. These
        // values are inert adapter-owned construction values; every executable
        // epoch supplies its own request through plan/begin_order.
        inputs.side = trade::Plaza2TradeSide::Sell;
        inputs.price = "103000";
        inputs.base_contract_code = "RTS";
        inputs.comment = "connector-host-cabi-v3-inert";
        inputs.ext_id = params->base_ext_id;
        inputs.add_user_id = params->base_add_user_id;
        inputs.cancel_user_id = params->base_cancel_user_id;
        inputs.recovery_user_id = params->base_recovery_user_id;
        inputs.run_id = copy_string(params->base_run_id);
        inputs.journal_root = copy_string(params->journal_root);
        inputs.receipt_path = copy_string(params->receipt_path);
        inputs.profile_id = copy_string(params->profile_id);
        inputs.profile_fingerprint = copy_string(params->profile_fingerprint);
        inputs.policy_version = required_string(params->policy_version)
                                    ? copy_string(params->policy_version)
                                    : "connector-host-cabi-v3:qty1:distance4:age5000:zero";
        inputs.policy_sha256 = copy_string(params->policy_sha256);
        value->host = std::make_unique<ConnectorHost>(moex::connector_host::build_plaza2_host_config(inputs));
        *out_handle = reinterpret_cast<MoexConnectorHostHandleV3>(value.release());
        return MOEX_RESULT_OK;
    });
}

MoexResult moex_v3_destroy_host(MoexConnectorHostHandleV3 handle) {
    return guard([&]() -> MoexResult {
        if (handle == nullptr)
            return MOEX_RESULT_INVALID_ARGUMENT;
        delete checked_handle(handle);
        return MOEX_RESULT_OK;
    });
}

MoexResult moex_v3_start(MoexConnectorHostHandleV3 handle) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr)
            return MOEX_RESULT_INVALID_ARGUMENT;
        return map_error(value->host->start());
    });
}

MoexResult moex_v3_poll(MoexConnectorHostHandleV3 handle) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr)
            return MOEX_RESULT_INVALID_ARGUMENT;
        return map_error(value->host->poll());
    });
}

MoexResult moex_v3_stop(MoexConnectorHostHandleV3 handle) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr)
            return MOEX_RESULT_INVALID_ARGUMENT;
        const auto result = value->host->stop();
        if (!result)
            value->cached_plan.reset(), value->cached_request.reset();
        return map_error(result);
    });
}

MoexResult moex_v3_get_snapshot(MoexConnectorHostHandleV3 handle, MoexPersistentSnapshotV3* out_snapshot) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr || out_snapshot == nullptr ||
            !valid_header(out_snapshot->struct_size, out_snapshot->abi_version, sizeof(*out_snapshot)))
            return MOEX_RESULT_INVALID_ARGUMENT;
        std::memset(out_snapshot, 0, sizeof(*out_snapshot));
        translate_snapshot(*value, *out_snapshot);
        return MOEX_RESULT_OK;
    });
}

MoexResult moex_v3_plan_order(MoexConnectorHostHandleV3 handle, const MoexPersistentOrderRequestV3* request,
                              MoexPersistentPlanInfoV3* out_plan) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr || request == nullptr || out_plan == nullptr ||
            !valid_header(request->struct_size, request->abi_version, sizeof(*request)) ||
            !valid_header(out_plan->struct_size, out_plan->abi_version, sizeof(*out_plan)))
            return MOEX_RESULT_INVALID_ARGUMENT;
        ConnectorHostOrderRequest native_request;
        if (!request_from(*request, native_request))
            return MOEX_RESULT_INVALID_ARGUMENT;
        value->cached_request = native_request;
        value->cached_plan = value->host->plan_order(native_request);
        std::memset(out_plan, 0, sizeof(*out_plan));
        translate_plan(*value, *value->cached_plan, *out_plan);
        return value->cached_plan->ok ? MOEX_RESULT_OK : MOEX_RESULT_UNAVAILABLE;
    });
}

MoexResult moex_v3_copy_plan_canonical(MoexConnectorHostHandleV3 handle, void* buffer, uint32_t capacity,
                                       uint32_t* written) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr || written == nullptr || (capacity != 0U && buffer == nullptr))
            return MOEX_RESULT_INVALID_ARGUMENT;
        if (!value->cached_plan || !value->cached_request)
            return MOEX_RESULT_NOT_INITIALIZED;
        if (!value->cached_plan->ok)
            return MOEX_RESULT_UNAVAILABLE;
        const auto size = value->cached_plan->canonical_json.size();
        if (size > UINT32_MAX)
            return MOEX_RESULT_OVERFLOW;
        *written = static_cast<uint32_t>(size);
        if (capacity < *written)
            return MOEX_RESULT_BUFFER_TOO_SMALL;
        if (size != 0)
            std::memcpy(buffer, value->cached_plan->canonical_json.data(), size);
        return MOEX_RESULT_OK;
    });
}

MoexResult moex_v3_begin_order(MoexConnectorHostHandleV3 handle, const MoexPersistentOrderRequestV3* request,
                               const void* canonical_bytes, uint32_t canonical_size, const char* full_sha256) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr || request == nullptr || full_sha256 == nullptr ||
            !valid_header(request->struct_size, request->abi_version, sizeof(*request)) || canonical_size == 0U ||
            canonical_bytes == nullptr || std::strlen(full_sha256) != 64U)
            return MOEX_RESULT_INVALID_ARGUMENT;
        ConnectorHostOrderRequest native_request;
        if (!request_from(*request, native_request) || !value->cached_request || !value->cached_plan ||
            !value->cached_plan->ok || !same_request(native_request, *value->cached_request))
            return MOEX_RESULT_INVALID_ARGUMENT;
        const auto result = value->host->begin_order(
            native_request, std::string_view(static_cast<const char*>(canonical_bytes), canonical_size),
            std::string_view(full_sha256, 64));
        if (!result)
            value->cached_plan.reset(), value->cached_request.reset();
        return map_error(result);
    });
}

MoexResult moex_v3_submit_order(MoexConnectorHostHandleV3 handle, MoexPersistentOrderResultV3* out_result) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr || out_result == nullptr ||
            !valid_header(out_result->struct_size, out_result->abi_version, sizeof(*out_result)))
            return MOEX_RESULT_INVALID_ARGUMENT;
        std::memset(out_result, 0, sizeof(*out_result));
        translate_order_result(value->host->submit_order(), *out_result);
        return MOEX_RESULT_OK;
    });
}

MoexResult moex_v3_poll_order(MoexConnectorHostHandleV3 handle, MoexPersistentOrderResultV3* out_result) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr || out_result == nullptr ||
            !valid_header(out_result->struct_size, out_result->abi_version, sizeof(*out_result)))
            return MOEX_RESULT_INVALID_ARGUMENT;
        std::memset(out_result, 0, sizeof(*out_result));
        translate_order_result(value->host->poll_order(), *out_result);
        return MOEX_RESULT_OK;
    });
}

MoexResult moex_v3_cancel_current_order(MoexConnectorHostHandleV3 handle, MoexPersistentOrderResultV3* out_result) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr || out_result == nullptr ||
            !valid_header(out_result->struct_size, out_result->abi_version, sizeof(*out_result)))
            return MOEX_RESULT_INVALID_ARGUMENT;
        std::memset(out_result, 0, sizeof(*out_result));
        translate_order_result(value->host->cancel_current_order(), *out_result);
        return MOEX_RESULT_OK;
    });
}

MoexResult moex_v3_finish_order_epoch(MoexConnectorHostHandleV3 handle) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr)
            return MOEX_RESULT_INVALID_ARGUMENT;
        const auto result = value->host->finish_order_epoch();
        if (!result)
            value->cached_plan.reset(), value->cached_request.reset();
        return map_error(result);
    });
}

MoexResult moex_v3_reconcile(MoexConnectorHostHandleV3 handle, MoexPersistentReconciliationResultV3* out_result) {
    return guard([&]() -> MoexResult {
        auto* value = checked_handle(handle);
        if (value == nullptr || out_result == nullptr ||
            !valid_header(out_result->struct_size, out_result->abi_version, sizeof(*out_result)))
            return MOEX_RESULT_INVALID_ARGUMENT;
        std::memset(out_result, 0, sizeof(*out_result));
        const auto result = value->host->reconcile();
        translate_reconciliation(result, *out_result);
        if (result.resolved)
            value->cached_plan.reset(), value->cached_request.reset();
        return MOEX_RESULT_OK;
    });
}

#define MOEX_V3_LAYOUT_DEFINITION(name)                                                                                \
    uint32_t moex_v3_sizeof_##name(void) {                                                                             \
        return static_cast<uint32_t>(sizeof(name));                                                                    \
    }                                                                                                                  \
    uint32_t moex_v3_alignof_##name(void) {                                                                            \
        return static_cast<uint32_t>(alignof(name));                                                                   \
    }

MOEX_V3_LAYOUT_DEFINITION(MoexConnectorHostCreateParamsV3)
MOEX_V3_LAYOUT_DEFINITION(MoexPersistentOrderRequestV3)
MOEX_V3_LAYOUT_DEFINITION(MoexV3StreamHealth)
MOEX_V3_LAYOUT_DEFINITION(MoexV3TargetProvenance)
MOEX_V3_LAYOUT_DEFINITION(MoexV3ReplyInfo)
MOEX_V3_LAYOUT_DEFINITION(MoexV3SubmissionInfo)
MOEX_V3_LAYOUT_DEFINITION(MoexPersistentSnapshotV3)
MOEX_V3_LAYOUT_DEFINITION(MoexPersistentPlanInfoV3)
MOEX_V3_LAYOUT_DEFINITION(MoexPersistentOrderResultV3)
MOEX_V3_LAYOUT_DEFINITION(MoexPersistentReconciliationResultV3)

#undef MOEX_V3_LAYOUT_DEFINITION

} // extern "C"
