#include "moex/plaza2_twime_reconciler/plaza2_twime_integrated_test_runner.hpp"

#include "moex_c_api_internal.hpp"
#include "moex/twime_sbe/twime_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace moex::plaza2_twime_reconciler {

namespace {

using moex::capi_internal::install_private_state_projector;
using moex::capi_internal::install_reconciler_snapshot;
using moex::plaza2::cgate::Plaza2LiveHealthSnapshot;
using moex::plaza2::cgate::Plaza2LiveRunResult;
using moex::plaza2::cgate::Plaza2LiveSessionRunner;
using moex::plaza2::cgate::Plaza2LiveStreamStatus;
using moex::plaza2::private_state::Plaza2PrivateStateProjector;
using moex::twime_sbe::DecodedTwimeMessage;
using moex::twime_sbe::TwimeCodec;
using moex::twime_sbe::TwimeDecodeError;
using moex::twime_sbe::TwimeEncodeRequest;
using moex::twime_trade::TwimeFakeClock;
using moex::twime_trade::TwimeInMemorySessionPersistenceStore;
using moex::twime_trade::TwimeJournalEntry;
using moex::twime_trade::TwimeLiveSessionRunner;
using moex::twime_trade::TwimeLiveSessionRunResult;
using moex::twime_trade::TwimeSessionState;

constexpr std::string_view kIntegratedConnectorName = "plaza2_twime_integrated_test";

std::string state_name(Plaza2TwimeIntegratedRunnerState state) {
    switch (state) {
    case Plaza2TwimeIntegratedRunnerState::Created:
        return "Created";
    case Plaza2TwimeIntegratedRunnerState::Validated:
        return "Validated";
    case Plaza2TwimeIntegratedRunnerState::Started:
        return "Started";
    case Plaza2TwimeIntegratedRunnerState::Ready:
        return "Ready";
    case Plaza2TwimeIntegratedRunnerState::Stopped:
        return "Stopped";
    case Plaza2TwimeIntegratedRunnerState::Failed:
        return "Failed";
    }
    return "Unknown";
}

bool all_streams_open(std::span<const Plaza2LiveStreamStatus> streams) {
    return !streams.empty() && std::all_of(streams.begin(), streams.end(), [](const Plaza2LiveStreamStatus& stream) {
        return stream.created && stream.opened;
    });
}

bool all_streams_online(std::span<const Plaza2LiveStreamStatus> streams) {
    return !streams.empty() && std::all_of(streams.begin(), streams.end(),
                                           [](const Plaza2LiveStreamStatus& stream) { return stream.online; });
}

bool all_streams_snapshot_complete(std::span<const Plaza2LiveStreamStatus> streams) {
    return !streams.empty() && std::all_of(streams.begin(), streams.end(), [](const Plaza2LiveStreamStatus& stream) {
        return stream.snapshot_complete;
    });
}

std::string source_prefix(std::string_view source) {
    return "[" + std::string(source) + "] ";
}

std::string sanitize_operator_line(std::string_view source, std::string_view line) {
    auto rendered = source_prefix(source) + std::string(line);
    if (const auto endpoint = rendered.find("endpoint="); endpoint != std::string::npos) {
        const auto value_begin = endpoint + std::string_view("endpoint=").size();
        const auto value_end = rendered.find_first_of(" ;", value_begin);
        rendered.replace(value_begin,
                         value_end == std::string::npos ? rendered.size() - value_begin : value_end - value_begin,
                         "[REDACTED_ENDPOINT]");
    }
    if (const auto credentials = rendered.find("credentials="); credentials != std::string::npos) {
        const auto value_begin = credentials + std::string_view("credentials=").size();
        const auto value_end = rendered.find_first_of(" ;", value_begin);
        rendered.replace(value_begin,
                         value_end == std::string::npos ? rendered.size() - value_begin : value_end - value_begin,
                         "[REDACTED]");
    }
    return rendered;
}

std::uint64_t max_twime_logical_sequence(const TwimeNormalizedInputBatch& batch) {
    std::uint64_t out = 0;
    for (const auto& order : batch.order_inputs) {
        out = std::max(out, order.logical_sequence);
    }
    for (const auto& trade : batch.trade_inputs) {
        out = std::max(out, trade.logical_sequence);
    }
    return out;
}

std::vector<TwimeEncodeRequest> decode_outbound_requests(std::span<const TwimeJournalEntry> entries,
                                                         std::string& error) {
    std::vector<TwimeEncodeRequest> requests;
    requests.reserve(entries.size());

    TwimeCodec codec;
    for (const auto& entry : entries) {
        if (entry.bytes.empty()) {
            continue;
        }

        DecodedTwimeMessage decoded;
        if (codec.decode_message(entry.bytes, decoded) != TwimeDecodeError::Ok || decoded.metadata == nullptr) {
            error = "failed to decode outbound TWIME journal entry";
            return {};
        }

        TwimeEncodeRequest request;
        request.message_name = entry.message_name.empty() ? std::string(decoded.metadata->name) : entry.message_name;
        request.template_id = entry.template_id == 0 ? decoded.metadata->template_id : entry.template_id;
        request.fields.reserve(decoded.fields.size());
        for (const auto& field : decoded.fields) {
            if (field.metadata == nullptr) {
                continue;
            }
            request.fields.push_back({
                std::string(field.metadata->name),
                field.value,
            });
        }
        requests.push_back(std::move(request));
    }

    return requests;
}

Plaza2TwimeIntegratedRunResult ok_result(std::string message) {
    return {.ok = true, .message = std::move(message)};
}

} // namespace

struct Plaza2TwimeIntegratedTestRunner::Impl {
    explicit Impl(Plaza2TwimeIntegratedTestConfig initial_config)
        : config(std::move(initial_config)), twime_runner(config.twime, twime_persistence, twime_clock),
          plaza_runner(config.plaza), reconciler(config.reconciler_stale_after_steps) {
        apply_integrated_arms();
        refresh_health();
    }

    void apply_integrated_arms() {
        config.twime.tcp.runtime_arm_state.test_network_armed = config.arm_state.test_network_armed;
        config.twime.tcp.runtime_arm_state.test_session_armed = config.arm_state.test_session_armed;
        config.plaza.arm_state.test_network_armed = config.arm_state.test_network_armed;
        config.plaza.arm_state.test_session_armed = config.arm_state.test_session_armed;
        config.plaza.arm_state.test_plaza2_armed = config.arm_state.test_plaza2_armed;
    }

    void set_time_source(TimeSource source) {
        time_source = std::move(source);
        twime_runner.set_time_source(time_source);
    }

    Plaza2TwimeIntegratedRunResult start() {
        if (started) {
            return fail("integrated TEST runner already started");
        }
        if (const auto validation = validate_config(); !validation.ok) {
            return validation;
        }
        health.state = Plaza2TwimeIntegratedRunnerState::Validated;
        health.evidence.startup_report_ready = true;
        append_operator_log("state=Validated");

        if (const auto handle_result = create_abi_handle(); !handle_result.ok) {
            return handle_result;
        }

        const auto twime_start = twime_runner.start();
        capture_child_logs();
        if (!twime_start.ok) {
            cleanup_runtime();
            return fail("TWIME TEST runner start failed: " + twime_start.message);
        }
        health.twime_validation_ok = true;

        const auto plaza_start = plaza_runner.start();
        capture_child_logs();
        if (!plaza_start.ok) {
            cleanup_runtime();
            return fail("PLAZA II TEST runner start failed: " + plaza_start.message);
        }
        health.plaza_validation_ok = true;
        started = true;
        refresh_health();
        health.state = Plaza2TwimeIntegratedRunnerState::Started;
        append_operator_log("state=Started");
        return ok_result("integrated TEST runner started");
    }

    Plaza2TwimeIntegratedRunResult poll_once() {
        if (!started) {
            return fail("integrated TEST runner is not started");
        }

        ++poll_count;

        const auto twime_poll = twime_runner.poll_once();
        capture_child_logs();
        if (!twime_poll.ok) {
            cleanup_runtime();
            return fail("TWIME TEST runner poll failed: " + twime_poll.message);
        }

        const auto plaza_poll = plaza_runner.poll_once();
        capture_child_logs();
        if (!plaza_poll.ok) {
            cleanup_runtime();
            return fail("PLAZA II TEST runner poll failed: " + plaza_poll.message);
        }

        const auto reconciler_result = refresh_reconciler();
        if (!reconciler_result.ok) {
            cleanup_runtime();
            return fail(reconciler_result.message);
        }

        const auto attach_result = attach_abi_snapshots();
        if (!attach_result.ok) {
            cleanup_runtime();
            return fail(attach_result.message);
        }

        refresh_health();
        if (health.readiness.ready) {
            health.state = Plaza2TwimeIntegratedRunnerState::Ready;
            if (!readiness_logged) {
                readiness_logged = true;
                health.evidence.readiness_summary_ready = true;
                append_operator_log("state=Ready");
            }
        }
        return ok_result("integrated TEST poll completed");
    }

    Plaza2TwimeIntegratedRunResult stop() {
        cleanup_runtime();
        health.state = Plaza2TwimeIntegratedRunnerState::Stopped;
        health.evidence.final_summary_ready = true;
        append_operator_log("state=Stopped");
        return ok_result("integrated TEST runner stopped");
    }

    Plaza2TwimeIntegratedRunResult validate_config() {
        if (config.profile_id.empty()) {
            return fail("profile_id must be set for integrated TEST bring-up");
        }
        if (config.twime.tcp.environment != twime_trade::transport::TwimeTcpEnvironment::Test) {
            return fail("integrated TEST bring-up requires TWIME TEST configuration");
        }
        if (config.plaza.runtime.environment != plaza2::cgate::Plaza2Environment::Test) {
            return fail("integrated TEST bring-up requires PLAZA II TEST configuration");
        }
        if (!config.arm_state.test_network_armed || !config.arm_state.test_session_armed ||
            !config.arm_state.test_plaza2_armed || !config.arm_state.test_reconcile_armed) {
            return fail("integrated TEST bring-up requires --armed-test-network, --armed-test-session, "
                        "--armed-test-plaza2, and --armed-test-reconcile");
        }
        return ok_result("integrated TEST config validated");
    }

    Plaza2TwimeIntegratedRunResult create_abi_handle() {
        if (abi_handle != nullptr) {
            return ok_result("C ABI handle already created");
        }
        MoexConnectorCreateParams params{};
        params.struct_size = sizeof(params);
        params.abi_version = MOEX_C_ABI_VERSION;
        params.connector_name = kIntegratedConnectorName.data();
        params.instance_id = config.profile_id.c_str();
        const auto result = moex_create_connector(&params, &abi_handle);
        if (result != MOEX_RESULT_OK || abi_handle == nullptr) {
            abi_handle = nullptr;
            return fail("failed to create integrated Phase 4B ABI handle");
        }
        append_operator_log("abi_handle=created");
        return ok_result("C ABI handle created");
    }

    void destroy_abi_handle() {
        if (abi_handle != nullptr) {
            static_cast<void>(moex_destroy_connector(abi_handle));
            abi_handle = nullptr;
        }
    }

    Plaza2TwimeIntegratedRunResult refresh_reconciler() {
        std::string decode_error;
        const auto outbound_requests =
            decode_outbound_requests(twime_runner.session().outbound_journal().entries(), decode_error);
        if (!decode_error.empty()) {
            return fail("failed to decode TWIME outbound journal for reconciler: " + decode_error);
        }
        const auto batch =
            collect_twime_reconciliation_inputs(outbound_requests, twime_runner.session().inbound_journal().entries(),
                                                &twime_runner.health_snapshot(), &twime_runner.session_metrics(), 1);
        if (!batch.ok) {
            return fail("failed to normalize TWIME inputs for reconciler: " + batch.error);
        }

        reconciler.reset();
        reconciler.set_stale_after_steps(config.reconciler_stale_after_steps);
        reconciler.apply_twime_inputs(batch);
        if (poll_count > 1) {
            reconciler.advance_steps(poll_count - 1);
        }

        const auto plaza_sequence = max_twime_logical_sequence(batch) + 1;
        reconciler.apply_plaza_snapshot(make_plaza_committed_snapshot(plaza_runner.projector(), plaza_sequence));
        health.reconciler_updating = true;
        return ok_result("reconciler refreshed");
    }

    Plaza2TwimeIntegratedRunResult attach_abi_snapshots() {
        if (abi_handle == nullptr) {
            return fail("integrated ABI handle is not available");
        }

        const auto private_result = install_private_state_projector(abi_handle, plaza_runner.projector().clone());
        if (private_result != MOEX_RESULT_OK) {
            return fail("failed to attach committed PLAZA private-state snapshot to ABI handle");
        }
        const auto reconciler_result = install_reconciler_snapshot(abi_handle, reconciler.clone());
        if (reconciler_result != MOEX_RESULT_OK) {
            return fail("failed to attach reconciler snapshot to ABI handle");
        }
        health.abi_snapshot_attached = true;
        return ok_result("ABI snapshots attached");
    }

    void refresh_health() {
        health.twime_session = twime_runner.health_snapshot();
        health.twime_metrics = twime_runner.session_metrics();
        health.plaza = plaza_runner.health_snapshot();
        health.reconciler = reconciler.health();
        health.plaza_runtime_probe_ok = health.plaza.runtime_probe_ok;
        health.plaza_scheme_drift_ok = health.plaza.scheme_drift_ok;
        health.last_resync_reason = health.plaza.last_resync_reason;
        health.abi_handle_valid = abi_handle != nullptr;
        health.abi_snapshot_attached = health.abi_handle_valid && health.abi_snapshot_attached;
        health.reconciled_order_count = reconciler.orders().size();
        health.reconciled_trade_count = reconciler.trades().size();
        health.evidence.operator_log_line_count = operator_log_lines.size();

        health.readiness.twime_session_established = health.twime_session.transport_open &&
                                                     health.twime_session.session_active &&
                                                     health.twime_session.state == TwimeSessionState::Active;
        health.readiness.plaza_runtime_probe_ok = health.plaza.runtime_probe_ok;
        health.readiness.plaza_scheme_drift_ok = health.plaza.scheme_drift_ok;
        health.readiness.plaza_streams_open = all_streams_open(health.plaza.streams);
        health.readiness.plaza_streams_online = all_streams_online(health.plaza.streams);
        health.readiness.plaza_streams_snapshot_complete = all_streams_snapshot_complete(health.plaza.streams);
        health.readiness.reconciler_attached = health.reconciler.twime.present && health.reconciler.plaza.present;
        health.readiness.abi_snapshot_attached = health.abi_snapshot_attached;
        health.readiness.ready = health.readiness.twime_session_established &&
                                 health.readiness.plaza_runtime_probe_ok && health.readiness.plaza_scheme_drift_ok &&
                                 health.readiness.plaza_streams_open && health.readiness.plaza_streams_online &&
                                 health.readiness.plaza_streams_snapshot_complete &&
                                 health.readiness.reconciler_attached && health.readiness.abi_snapshot_attached;
        health.readiness.blocker = readiness_blocker();
    }

    std::string readiness_blocker() const {
        if (!health.readiness.twime_session_established) {
            return "twime_session_not_established";
        }
        if (!health.readiness.plaza_runtime_probe_ok) {
            return "plaza_runtime_probe_failed";
        }
        if (!health.readiness.plaza_scheme_drift_ok) {
            return "plaza_scheme_drift_failed";
        }
        if (!health.readiness.plaza_streams_open) {
            return "plaza_streams_not_open";
        }
        if (!health.readiness.plaza_streams_online) {
            return "plaza_streams_not_online";
        }
        if (!health.readiness.plaza_streams_snapshot_complete) {
            return "plaza_snapshot_incomplete";
        }
        if (!health.readiness.reconciler_attached) {
            return "reconciler_not_attached";
        }
        if (!health.readiness.abi_snapshot_attached) {
            return "abi_snapshot_not_attached";
        }
        return {};
    }

    void capture_child_logs() {
        const auto& twime_logs = twime_runner.operator_log_lines();
        while (twime_log_index < twime_logs.size()) {
            append_operator_log(sanitize_operator_line("TWIME", twime_logs[twime_log_index]));
            ++twime_log_index;
        }

        const auto& plaza_logs = plaza_runner.operator_log_lines();
        while (plaza_log_index < plaza_logs.size()) {
            append_operator_log(sanitize_operator_line("PLAZA", plaza_logs[plaza_log_index]));
            ++plaza_log_index;
        }
    }

    void append_operator_log(std::string line) {
        if (operator_log_lines.empty() || operator_log_lines.back() != line) {
            operator_log_lines.push_back(std::move(line));
        }
    }

    Plaza2TwimeIntegratedRunResult fail(std::string message) {
        health.last_error = message;
        health.state = Plaza2TwimeIntegratedRunnerState::Failed;
        append_operator_log("error=" + message);
        return {.ok = false, .message = std::move(message)};
    }

    void cleanup_runtime() {
        capture_child_logs();

        if (started) {
            auto twime_stop = twime_runner.request_stop();
            if (!twime_stop.ok) {
                append_operator_log("twime_stop_error=" + twime_stop.message);
            }
            for (int attempt = 0; attempt < 16; ++attempt) {
                const auto twime_poll = twime_runner.poll_once();
                capture_child_logs();
                const auto twime_finish = twime_runner.stop_if_needed();
                if (!twime_poll.ok) {
                    append_operator_log("twime_poll_error=" + twime_poll.message);
                    break;
                }
                if (twime_finish.message == "runner stopped" || twime_finish.message == "terminate timeout reached") {
                    break;
                }
            }
            static_cast<void>(plaza_runner.stop());
            capture_child_logs();
        }

        destroy_abi_handle();
        started = false;
    }

    Plaza2TwimeIntegratedTestConfig config;
    Plaza2TwimeIntegratedHealthSnapshot health;
    TwimeInMemorySessionPersistenceStore twime_persistence;
    TwimeFakeClock twime_clock{0};
    TwimeLiveSessionRunner twime_runner;
    Plaza2LiveSessionRunner plaza_runner;
    Plaza2TwimeReconciler reconciler;
    TimeSource time_source{};
    std::vector<std::string> operator_log_lines;
    MoexConnectorHandle abi_handle{nullptr};
    std::size_t twime_log_index{0};
    std::size_t plaza_log_index{0};
    std::uint64_t poll_count{0};
    bool started{false};
    bool readiness_logged{false};
};

Plaza2TwimeIntegratedTestRunner::Plaza2TwimeIntegratedTestRunner(Plaza2TwimeIntegratedTestConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Plaza2TwimeIntegratedTestRunner::~Plaza2TwimeIntegratedTestRunner() {
    if (impl_ != nullptr) {
        static_cast<void>(impl_->stop());
    }
}

Plaza2TwimeIntegratedTestRunner::Plaza2TwimeIntegratedTestRunner(Plaza2TwimeIntegratedTestRunner&&) noexcept = default;

Plaza2TwimeIntegratedTestRunner&
Plaza2TwimeIntegratedTestRunner::operator=(Plaza2TwimeIntegratedTestRunner&&) noexcept = default;

void Plaza2TwimeIntegratedTestRunner::set_time_source(TimeSource time_source) {
    impl_->set_time_source(std::move(time_source));
}

Plaza2TwimeIntegratedRunResult Plaza2TwimeIntegratedTestRunner::start() {
    return impl_->start();
}

Plaza2TwimeIntegratedRunResult Plaza2TwimeIntegratedTestRunner::poll_once() {
    return impl_->poll_once();
}

Plaza2TwimeIntegratedRunResult Plaza2TwimeIntegratedTestRunner::stop() {
    return impl_->stop();
}

const Plaza2TwimeIntegratedHealthSnapshot& Plaza2TwimeIntegratedTestRunner::health_snapshot() const noexcept {
    return impl_->health;
}

const std::vector<std::string>& Plaza2TwimeIntegratedTestRunner::operator_log_lines() const noexcept {
    return impl_->operator_log_lines;
}

const Plaza2TwimeReconciler& Plaza2TwimeIntegratedTestRunner::reconciler() const noexcept {
    return impl_->reconciler;
}

MoexConnectorHandle Plaza2TwimeIntegratedTestRunner::abi_handle() const noexcept {
    return impl_->abi_handle;
}

} // namespace moex::plaza2_twime_reconciler
