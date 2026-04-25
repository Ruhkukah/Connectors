#pragma once

#include "moex/plaza2/cgate/plaza2_credential_provider.hpp"
#include "moex/plaza2/cgate/plaza2_manual_operator_gate.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2::cgate {

struct Plaza2Aggr20Level {
    std::int64_t isin_id{0};
    std::int64_t price_scaled{0};
    std::int64_t volume{0};
    std::int32_t dir{0};
    std::uint64_t repl_id{0};
    std::int64_t repl_rev{0};
    std::uint64_t moment{0};
    std::uint64_t moment_ns{0};
    std::string price;
    std::string synth_volume;
};

struct Plaza2Aggr20Snapshot {
    std::size_t row_count{0};
    std::size_t instrument_count{0};
    std::size_t bid_depth_levels{0};
    std::size_t ask_depth_levels{0};
    std::uint64_t last_repl_id{0};
    std::int64_t last_repl_rev{0};
    std::optional<Plaza2Aggr20Level> top_bid;
    std::optional<Plaza2Aggr20Level> top_ask;
    std::vector<Plaza2Aggr20Level> levels;
};

class Plaza2Aggr20BookProjector {
  public:
    void reset();
    void begin_transaction();
    [[nodiscard]] Plaza2Error on_row(std::span<const Plaza2DecodedFieldValue> fields);
    [[nodiscard]] Plaza2Error commit();
    void rollback();

    [[nodiscard]] const Plaza2Aggr20Snapshot& snapshot() const noexcept;
    [[nodiscard]] bool transaction_open() const noexcept;

  private:
    std::vector<Plaza2Aggr20Level> staged_rows_;
    Plaza2Aggr20Snapshot committed_;
    bool transaction_open_{false};
};

struct Plaza2Aggr20MdStreamConfig {
    generated::StreamCode stream_code{generated::StreamCode::kFortsAggrRepl};
    std::string settings;
    std::string open_settings;
};

struct Plaza2Aggr20MdConfig {
    std::string profile_id;
    std::string endpoint_host;
    std::uint16_t endpoint_port{0};
    Plaza2Settings runtime{};
    std::string connection_settings;
    std::string connection_open_settings;
    Plaza2Aggr20MdStreamConfig stream;
    Plaza2CredentialConfig credentials{};
    Plaza2RuntimeArmState arm_state{};
    bool test_market_data_armed{false};
    std::uint32_t process_timeout_ms{50};
};

enum class Plaza2Aggr20MdRunnerState : std::uint8_t {
    Created = 0,
    Validated = 1,
    Started = 2,
    Ready = 3,
    Stopped = 4,
    Failed = 5,
};

struct Plaza2Aggr20MdHealthSnapshot {
    Plaza2Aggr20MdRunnerState state{Plaza2Aggr20MdRunnerState::Created};
    Plaza2Compatibility compatibility{Plaza2Compatibility::Unknown};
    bool runtime_probe_ok{false};
    bool scheme_drift_ok{false};
    Plaza2Compatibility scheme_drift_status{Plaza2Compatibility::Unknown};
    std::size_t scheme_drift_warning_count{0};
    std::size_t scheme_drift_fatal_count{0};
    bool stream_created{false};
    bool stream_opened{false};
    bool stream_online{false};
    bool stream_snapshot_complete{false};
    bool ready{false};
    std::uint32_t last_process_runtime_code{0};
    std::string last_error;
    std::string failure_classification;
    Plaza2Aggr20Snapshot snapshot;
};

struct Plaza2Aggr20MdRunResult {
    bool ok{false};
    std::string message;
};

[[nodiscard]] std::string_view plaza2_aggr20_md_runner_state_name(Plaza2Aggr20MdRunnerState state) noexcept;
[[nodiscard]] std::string classify_plaza2_aggr20_failure(const Plaza2Aggr20MdHealthSnapshot& health);
[[nodiscard]] Plaza2Error validate_plaza2_aggr20_md_config(const Plaza2Aggr20MdConfig& config);

class Plaza2Aggr20MdRunner {
  public:
    explicit Plaza2Aggr20MdRunner(Plaza2Aggr20MdConfig config);
    ~Plaza2Aggr20MdRunner();

    Plaza2Aggr20MdRunner(Plaza2Aggr20MdRunner&&) noexcept;
    Plaza2Aggr20MdRunner& operator=(Plaza2Aggr20MdRunner&&) noexcept;

    Plaza2Aggr20MdRunner(const Plaza2Aggr20MdRunner&) = delete;
    Plaza2Aggr20MdRunner& operator=(const Plaza2Aggr20MdRunner&) = delete;

    [[nodiscard]] Plaza2Aggr20MdRunResult start();
    [[nodiscard]] Plaza2Aggr20MdRunResult poll_once();
    [[nodiscard]] Plaza2Aggr20MdRunResult stop();

    [[nodiscard]] const Plaza2Aggr20MdHealthSnapshot& health_snapshot() const noexcept;
    [[nodiscard]] const Plaza2RuntimeProbeReport& probe_report() const noexcept;
    [[nodiscard]] const Plaza2Aggr20BookProjector& projector() const noexcept;
    [[nodiscard]] const std::vector<std::string>& operator_log_lines() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::plaza2::cgate
