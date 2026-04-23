#pragma once

#include "adapters/alorengine_capi/moex_c_api.h"
#include "moex/plaza2/cgate/plaza2_live_session_runner.hpp"
#include "moex/plaza2_twime_reconciler/plaza2_twime_reconciler.hpp"
#include "moex/twime_trade/twime_live_session_runner.hpp"
#include "moex/twime_trade/twime_session_metrics.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace moex::plaza2_twime_reconciler {

struct Plaza2TwimeIntegratedArmState {
    bool test_network_armed{false};
    bool test_session_armed{false};
    bool test_plaza2_armed{false};
    bool test_reconcile_armed{false};
};

struct Plaza2TwimeIntegratedTestConfig {
    std::string profile_id;
    twime_trade::TwimeLiveSessionConfig twime{};
    plaza2::cgate::Plaza2LiveSessionConfig plaza{};
    Plaza2TwimeIntegratedArmState arm_state{};
    std::uint64_t reconciler_stale_after_steps{4};
};

enum class Plaza2TwimeIntegratedRunnerState : std::uint8_t {
    Created = 0,
    Validated = 1,
    Started = 2,
    Ready = 3,
    Stopped = 4,
    Failed = 5,
};

struct Plaza2TwimeIntegratedReadinessSnapshot {
    bool twime_session_established{false};
    bool plaza_runtime_probe_ok{false};
    bool plaza_scheme_drift_ok{false};
    bool plaza_streams_open{false};
    bool plaza_streams_online{false};
    bool plaza_streams_snapshot_complete{false};
    bool reconciler_attached{false};
    bool abi_snapshot_attached{false};
    bool ready{false};
    std::string blocker;
};

struct Plaza2TwimeIntegratedEvidenceSnapshot {
    bool startup_report_ready{false};
    bool readiness_summary_ready{false};
    bool final_summary_ready{false};
    std::size_t operator_log_line_count{0};
};

struct Plaza2TwimeIntegratedHealthSnapshot {
    Plaza2TwimeIntegratedRunnerState state{Plaza2TwimeIntegratedRunnerState::Created};
    bool twime_validation_ok{false};
    bool plaza_validation_ok{false};
    bool plaza_runtime_probe_ok{false};
    bool plaza_scheme_drift_ok{false};
    bool reconciler_updating{false};
    bool abi_handle_valid{false};
    bool abi_snapshot_attached{false};
    std::string last_error;
    std::string last_resync_reason;
    twime_trade::TwimeSessionHealthSnapshot twime_session{};
    twime_trade::TwimeSessionMetrics twime_metrics{};
    plaza2::cgate::Plaza2LiveHealthSnapshot plaza{};
    Plaza2TwimeReconcilerHealthSnapshot reconciler{};
    Plaza2TwimeIntegratedReadinessSnapshot readiness{};
    Plaza2TwimeIntegratedEvidenceSnapshot evidence{};
    std::size_t reconciled_order_count{0};
    std::size_t reconciled_trade_count{0};
};

struct Plaza2TwimeIntegratedRunResult {
    bool ok{false};
    std::string message;
};

class Plaza2TwimeIntegratedTestRunner {
  public:
    using TimePoint = twime_trade::TwimeLiveSessionRunner::TimePoint;
    using TimeSource = twime_trade::TwimeLiveSessionRunner::TimeSource;

    explicit Plaza2TwimeIntegratedTestRunner(Plaza2TwimeIntegratedTestConfig config);
    ~Plaza2TwimeIntegratedTestRunner();

    Plaza2TwimeIntegratedTestRunner(Plaza2TwimeIntegratedTestRunner&&) noexcept;
    Plaza2TwimeIntegratedTestRunner& operator=(Plaza2TwimeIntegratedTestRunner&&) noexcept;

    Plaza2TwimeIntegratedTestRunner(const Plaza2TwimeIntegratedTestRunner&) = delete;
    Plaza2TwimeIntegratedTestRunner& operator=(const Plaza2TwimeIntegratedTestRunner&) = delete;

    void set_time_source(TimeSource time_source);

    [[nodiscard]] Plaza2TwimeIntegratedRunResult start();
    [[nodiscard]] Plaza2TwimeIntegratedRunResult poll_once();
    [[nodiscard]] Plaza2TwimeIntegratedRunResult stop();

    [[nodiscard]] const Plaza2TwimeIntegratedHealthSnapshot& health_snapshot() const noexcept;
    [[nodiscard]] const std::vector<std::string>& operator_log_lines() const noexcept;
    [[nodiscard]] const Plaza2TwimeReconciler& reconciler() const noexcept;
    [[nodiscard]] MoexConnectorHandle abi_handle() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::plaza2_twime_reconciler
