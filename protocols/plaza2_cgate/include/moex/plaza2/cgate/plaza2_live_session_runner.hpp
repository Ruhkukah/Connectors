#pragma once

#include "moex/plaza2/cgate/plaza2_credential_provider.hpp"
#include "moex/plaza2/cgate/plaza2_manual_operator_gate.hpp"
#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2::cgate {

struct Plaza2LiveStreamConfig {
    generated::StreamCode stream_code{kNoStreamCode};
    std::string settings;
    std::string open_settings;
};

struct Plaza2LiveSessionConfig {
    std::string profile_id;
    std::string endpoint_host;
    std::uint16_t endpoint_port{0};
    Plaza2Settings runtime{};
    std::string connection_settings;
    std::string connection_open_settings;
    std::vector<Plaza2LiveStreamConfig> streams;
    Plaza2CredentialConfig credentials{};
    Plaza2RuntimeArmState arm_state{};
    std::uint32_t process_timeout_ms{50};
};

enum class Plaza2LiveRunnerState : std::uint8_t {
    Created = 0,
    Validated = 1,
    Started = 2,
    Ready = 3,
    Stopped = 4,
    Failed = 5,
};

struct Plaza2LiveStreamStatus {
    generated::StreamCode stream_code{kNoStreamCode};
    std::string stream_name;
    bool created{false};
    bool opened{false};
    bool online{false};
    bool snapshot_complete{false};
};

struct Plaza2LiveStateCounts {
    std::size_t session_count{0};
    std::size_t instrument_count{0};
    std::size_t matching_map_count{0};
    std::size_t limit_count{0};
    std::size_t position_count{0};
    std::size_t own_order_count{0};
    std::size_t own_trade_count{0};
};

struct Plaza2LiveHealthSnapshot {
    Plaza2LiveRunnerState state{Plaza2LiveRunnerState::Created};
    Plaza2Compatibility compatibility{Plaza2Compatibility::Unknown};
    bool runtime_probe_ok{false};
    bool scheme_drift_ok{false};
    bool ready{false};
    std::uint32_t last_process_runtime_code{0};
    std::string last_error;
    std::string last_resync_reason;
    private_state::ConnectorHealthSnapshot connector_health;
    private_state::ResumeMarkersSnapshot resume_markers;
    std::vector<Plaza2LiveStreamStatus> streams;
    Plaza2LiveStateCounts counts;
};

struct Plaza2LiveRunResult {
    bool ok{false};
    std::string message;
};

class Plaza2LiveSessionRunner {
  public:
    explicit Plaza2LiveSessionRunner(Plaza2LiveSessionConfig config);
    ~Plaza2LiveSessionRunner();

    Plaza2LiveSessionRunner(Plaza2LiveSessionRunner&&) noexcept;
    Plaza2LiveSessionRunner& operator=(Plaza2LiveSessionRunner&&) noexcept;

    Plaza2LiveSessionRunner(const Plaza2LiveSessionRunner&) = delete;
    Plaza2LiveSessionRunner& operator=(const Plaza2LiveSessionRunner&) = delete;

    [[nodiscard]] Plaza2LiveRunResult start();
    [[nodiscard]] Plaza2LiveRunResult poll_once();
    [[nodiscard]] Plaza2LiveRunResult stop();

    [[nodiscard]] const Plaza2LiveHealthSnapshot& health_snapshot() const noexcept;
    [[nodiscard]] const Plaza2RuntimeProbeReport& probe_report() const noexcept;
    [[nodiscard]] const private_state::Plaza2PrivateStateProjector& projector() const noexcept;
    [[nodiscard]] const std::vector<std::string>& operator_log_lines() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::plaza2::cgate
