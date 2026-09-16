#pragma once

#include "moex/plaza2/cgate/plaza2_credential_provider.hpp"
#include "moex/plaza2/cgate/plaza2_certification_evidence.hpp"
#include "moex/plaza2/cgate/plaza2_manual_operator_gate.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include "moex/plaza2/cgate/plaza2_fixed_point.hpp"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace moex::plaza2::cgate {

struct Plaza2Aggr20Level {
    std::int64_t isin_id{0};
    // AGGR20 keeps six decimal places internally. Session/order values use
    // scale 1e5 and must not be compared by mixing the two unit systems.
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
    // Cross-instrument values retained for diagnostics only. Trading must use
    // snapshot_for_isin(), never these global best prices.
    std::optional<Plaza2Aggr20Level> top_bid;
    std::optional<Plaza2Aggr20Level> top_ask;
    std::vector<Plaza2Aggr20Level> levels;
    std::chrono::steady_clock::time_point committed_at{};
    std::uint64_t exchange_moment{0};
    std::uint64_t exchange_moment_ns{0};
};

struct Plaza2Aggr20InstrumentSnapshot {
    std::int64_t isin_id{0};
    std::size_t row_count{0};
    std::size_t bid_depth_levels{0};
    std::size_t ask_depth_levels{0};
    std::uint64_t last_repl_id{0};
    std::int64_t last_repl_rev{0};
    std::optional<Plaza2Aggr20Level> top_bid;
    std::optional<Plaza2Aggr20Level> top_ask;
    // Positional depth in deterministic price order. Bids are descending and
    // asks are ascending; the vector is target-scoped and never represents a
    // replication-vector or repl_id order.
    std::vector<Plaza2Aggr20Level> levels;
    // Monotonic target-source version. It advances only when this target's
    // normalized state or source identity changes; unrelated ISIN commits do
    // not advance it.
    std::uint64_t source_snapshot_version{0};
    // FNV-1a over the documented fixed-width little-endian canonical form of
    // the sorted target levels and their source identity.
    std::uint64_t source_snapshot_hash{0};
    // This is captured from the local monotonic clock at commit time.
    std::chrono::steady_clock::time_point committed_at{};
    std::uint64_t exchange_moment{0};
    std::uint64_t exchange_moment_ns{0};
};

class Plaza2Aggr20QualificationObserver {
  public:
    virtual ~Plaza2Aggr20QualificationObserver() = default;
    virtual void committed(const Plaza2Aggr20Snapshot&) noexcept = 0;
};

class Plaza2Aggr20BookProjector {
  public:
    using Clock = std::chrono::steady_clock;
    using NowFn = std::function<Clock::time_point()>;

    explicit Plaza2Aggr20BookProjector(NowFn now = {});

    void set_qualification_observer(Plaza2Aggr20QualificationObserver* observer) noexcept {
        qualification_observer_ = observer;
    }
    void reset();
    void begin_transaction();
    [[nodiscard]] Plaza2Error on_row(std::span<const Plaza2DecodedFieldValue> fields);
    [[nodiscard]] Plaza2Error commit();
    void rollback();

    [[nodiscard]] const Plaza2Aggr20Snapshot& snapshot() const noexcept;
    [[nodiscard]] std::optional<Plaza2Aggr20InstrumentSnapshot> snapshot_for_isin(std::int64_t isin_id) const;
    [[nodiscard]] bool transaction_open() const noexcept;

  private:
    Plaza2Aggr20QualificationObserver* qualification_observer_{nullptr};
    std::vector<Plaza2Aggr20Level> staged_rows_;
    std::unordered_set<std::int64_t> affected_isin_ids_;
    Plaza2Aggr20Snapshot committed_;
    std::unordered_map<std::int64_t, Plaza2Aggr20InstrumentSnapshot> instrument_snapshots_;
    std::uint64_t snapshot_version_counter_{0};
    NowFn now_;
    bool transaction_open_{false};
};

struct Plaza2Aggr20SysEventSnapshot {
    std::uint64_t source_repl_id{0};
    std::int64_t source_repl_rev{0};
    std::int64_t source_repl_act{0};
    std::int32_t event_type{0};
    std::int64_t event_id{0};
    std::int32_t sess_id{0};
    std::string message;
    std::uint64_t server_time{0};
    bool seen_during_snapshot{false};
};

enum class Plaza2Aggr20AuthorityState : std::uint8_t {
    WaitingForTransport = 0,
    WaitingForSnapshot = 1,
    WaitingForSessionDataReady = 2,
    Authoritative = 3,
    Recovering = 4,
};

struct Plaza2Aggr20AuthoritySnapshot {
    Plaza2Aggr20AuthorityState state{Plaza2Aggr20AuthorityState::WaitingForTransport};
    bool transport_active{false};
    bool snapshot_complete{false};
    bool session_data_ready{false};
    // Source-level authority. ConnectorHost additionally requires a
    // target-scoped snapshot_for_isin() record before exposing a target book.
    bool target_authoritative{false};
    std::uint64_t stream_epoch{0};
    std::uint64_t market_data_authority_epoch{0};
    std::optional<Plaza2Aggr20SysEventSnapshot> last_sys_event;
};

// Shared by the standalone market-data runner and the trading host.
class Plaza2Aggr20ListenerBridge final : public Plaza2ListenerEventHandler {
  public:
    explicit Plaza2Aggr20ListenerBridge(Plaza2Aggr20BookProjector& projector, std::int32_t expected_session_id = 0)
        : projector_(projector), expected_session_id_(expected_session_id) {}
    void reset() noexcept;
    [[nodiscard]] bool online() const noexcept {
        return online_;
    }
    [[nodiscard]] bool snapshot_complete() const noexcept {
        return snapshot_complete_;
    }
    [[nodiscard]] bool transport_active() const noexcept {
        return transport_active_;
    }
    [[nodiscard]] bool session_data_ready() const noexcept {
        return session_data_ready_;
    }
    [[nodiscard]] bool authoritative() const noexcept {
        return target_authoritative_;
    }
    [[nodiscard]] const Plaza2Aggr20AuthoritySnapshot authority_snapshot() const;
    [[nodiscard]] bool has_lifenum() const noexcept {
        return has_lifenum_;
    }
    [[nodiscard]] std::uint64_t last_lifenum() const noexcept {
        return last_lifenum_;
    }
    [[nodiscard]] bool recovering() const noexcept {
        return reopen_required_ || retry_at_.has_value();
    }
    [[nodiscard]] const Plaza2Error& last_recovery_error() const noexcept {
        return last_recovery_error_;
    }
    [[nodiscard]] std::string_view recovery_failure_classification() const noexcept {
        return recovery_failure_classification_;
    }
    void set_bootstrap_watchdog(std::chrono::milliseconds watchdog) noexcept {
        bootstrap_watchdog_ = watchdog;
    }
    [[nodiscard]] Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent&) override;
    void on_plaza2_listener_error(const Plaza2Error& error) noexcept override {
        reset();
        last_recovery_error_ = error;
        recovery_failure_classification_ = "fatal_callback";
    }
    [[nodiscard]] Plaza2Error supervise(Plaza2Listener&, std::chrono::steady_clock::time_point now);

  private:
    void invalidate(bool request_reopen, bool transport_active) noexcept;
    [[nodiscard]] bool accepts_session(std::int32_t sess_id) const noexcept;
    Plaza2Aggr20BookProjector& projector_;
    std::int32_t expected_session_id_{0};
    bool transport_active_{false};
    bool online_{false};
    bool snapshot_complete_{false};
    bool session_data_ready_{false};
    bool target_authoritative_{false};
    bool reopen_required_{false};
    bool has_lifenum_{false};
    std::uint64_t last_lifenum_{0};
    std::uint64_t stream_epoch_{0};
    std::uint64_t market_data_authority_epoch_{0};
    std::optional<Plaza2Aggr20SysEventSnapshot> last_sys_event_;
    std::optional<std::chrono::steady_clock::time_point> retry_at_;
    std::optional<std::chrono::steady_clock::time_point> bootstrap_started_at_;
    std::chrono::milliseconds bootstrap_watchdog_{30000};
    Plaza2Error last_recovery_error_;
    std::string recovery_failure_classification_;
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
    Plaza2CredentialConfig software_key{};
    Plaza2RuntimeArmState arm_state{};
    bool test_market_data_armed{false};
    std::uint32_t process_timeout_ms{50};
    Plaza2Aggr20BookProjector::NowFn now;
    std::chrono::milliseconds listener_bootstrap_watchdog{30000};
    // Supplied by the launch/evidence path; absence keeps the runner from
    // reporting certification-ready even when the stream itself is healthy.
    std::optional<Plaza2ClockEvidence> clock_evidence;
};

enum class Plaza2Aggr20MdRunnerState : std::uint8_t {
    Created = 0,
    Validated = 1,
    Started = 2,
    Ready = 3,
    Stopped = 4,
    Failed = 5,
    Recovering = 6,
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
    bool transport_active{false};
    bool stream_online{false};
    bool stream_snapshot_complete{false};
    bool clock_evidence_present{false};
    bool clock_evidence_ok{false};
    bool session_data_ready{false};
    bool target_authoritative{false};
    Plaza2Aggr20AuthoritySnapshot authority;
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
