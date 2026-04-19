#pragma once

#include "moex/twime_trade/transport/twime_credential_provider.hpp"
#include "moex/twime_trade/transport/twime_tcp_transport.hpp"
#include "moex/twime_trade/twime_manual_operator_gate.hpp"
#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/twime_session_health.hpp"
#include "moex/twime_trade/twime_session_metrics.hpp"
#include "moex/twime_trade/twime_session_persistence.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace moex::twime_trade {

struct TwimeLiveSessionPolicy {
    bool reconnect_enabled{false};
    std::uint32_t max_reconnect_attempts{3};
    std::uint32_t establish_deadline_ms{10000};
    std::uint32_t graceful_terminate_timeout_ms{3000};
};

struct TwimeLiveSessionConfig {
    TwimeSessionConfig session{};
    transport::TwimeTcpConfig tcp{};
    transport::TwimeCredentialConfig credentials{};
    TwimeLiveSessionPolicy policy{};
};

struct TwimeLiveSessionRunResult {
    bool ok{false};
    std::string message;
};

class TwimeLiveSessionRunner {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using TimeSource = std::function<TimePoint()>;

    TwimeLiveSessionRunner(TwimeLiveSessionConfig config, TwimeSessionPersistenceStore& persistence_store,
                           TwimeFakeClock& session_clock);

    void set_time_source(TimeSource time_source);

    [[nodiscard]] TwimeLiveSessionRunResult start();
    [[nodiscard]] TwimeLiveSessionRunResult poll_once();
    [[nodiscard]] TwimeLiveSessionRunResult request_stop();
    [[nodiscard]] TwimeLiveSessionRunResult stop_if_needed();

    [[nodiscard]] bool reconnect_due() const noexcept;
    [[nodiscard]] const TwimeSessionHealthSnapshot& health_snapshot() const noexcept;
    [[nodiscard]] const TwimeSessionMetrics& session_metrics() const noexcept;
    [[nodiscard]] const transport::TwimeTcpTransport& transport() const noexcept;
    [[nodiscard]] const std::vector<std::string>& operator_log_lines() const noexcept;
    [[nodiscard]] const std::vector<std::string>& cert_log_lines() const noexcept;
    [[nodiscard]] const TwimeSession& session() const noexcept;
    [[nodiscard]] bool ready_for_application_order_flow() const noexcept;
    [[nodiscard]] bool establish_deadline_expired() const noexcept;

  private:
    [[nodiscard]] TimePoint now() const;
    void sync_fake_clock();
    void refresh_health();
    void append_operator_log(std::string line);
    void persist_snapshot();
    [[nodiscard]] TwimeLiveSessionRunResult fail(std::string message);
    void handle_session_events(std::span<const TwimeSessionEvent> events);
    [[nodiscard]] bool should_schedule_reconnect() const noexcept;
    void maybe_schedule_reconnect();
    void maybe_attempt_reconnect();
    [[nodiscard]] TwimeLiveSessionRunResult validate_operator_arms() const;
    [[nodiscard]] TwimeLiveSessionRunResult load_credentials();

    TwimeLiveSessionConfig config_;
    TwimeSessionPersistenceStore& persistence_store_;
    TwimePersistentRecoveryStateStore recovery_store_;
    TwimeFakeClock& session_clock_;
    transport::TwimeTcpTransport transport_;
    TwimeSession session_;
    TimeSource time_source_{};
    TimePoint last_time_point_{};
    bool has_last_time_point_{false};
    TwimeSessionHealthSnapshot health_{};
    TwimeSessionMetrics metrics_{};
    std::vector<std::string> operator_log_lines_;
    bool started_{false};
    bool stop_requested_{false};
    bool external_session_path_{false};
    bool transport_open_seen_{false};
    bool establish_sent_seen_{false};
    std::uint64_t establish_started_ms_{0};
    std::uint64_t terminate_started_ms_{0};
    bool reconnect_scheduled_{false};
    std::uint64_t reconnect_due_ms_{0};
};

} // namespace moex::twime_trade
