#pragma once

#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"
#include "moex/plaza2/cgate/plaza2_public_deals.hpp"
#include "moex/plaza2/cgate/plaza2_credential_provider.hpp"
#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include "moex/plaza2_trade/plaza2_trade_codec.hpp"

#include "moex/plaza2/cgate/plaza2_publisher_rate.hpp"
#include <functional>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moex::connector_host {
class ConnectorHost;
}

namespace moex::plaza2_trade {

struct CgateStreamConfig {
    plaza2::generated::StreamCode stream_code{plaza2::cgate::kNoStreamCode};
    std::string settings;
    std::string open_settings;
};

// POS.info selects the replay boundary before opening TRADE.
struct Plaza2TradeReplayAnchor {
    std::int64_t trades_rev{0};
    std::int64_t trades_lifenum{0};
    std::int64_t server_time{0};
};

enum class CgateSessionMode : std::uint8_t {
    OfflineFake = 0,
    Live = 1,
};

enum class PublisherRateOwner : std::uint8_t { Session, External };

// One owner keeps the CGate environment and connection alive while each
// replication listener and publisher recovers independently.
struct CgateSessionConfig {
    CgateSessionMode mode{CgateSessionMode::OfflineFake};
    plaza2::cgate::Plaza2Settings runtime{};
    std::string connection_settings;
    std::string connection_open_settings;
    std::vector<CgateStreamConfig> private_streams;
    // The two current-day/session status streams are required alongside the
    // REFDATA-only read-side profile, and supplementary to the five trading
    // private replication streams above.
    std::vector<CgateStreamConfig> status_streams;
    CgateStreamConfig aggr20_stream;
    // Optional public anonymous trades, independent of private TRADE replay.
    // An empty URL preserves the existing four-listener read-only profile.
    CgateStreamConfig public_deals_stream;
    std::int64_t public_deals_target_isin_id{0};
    // Optional exact AGGR20 sys_events session identity. Zero accepts a
    // non-zero current session id and is suitable only before refdata
    // negotiation has supplied the target session.
    std::int32_t aggr20_target_session_id{0};
    std::string publisher_settings;
    std::string publisher_open_settings;
    std::string publisher_name{"PUB"};
    std::string p2mqreply_settings;
    std::string p2mqreply_open_settings;
    // ConnectorHost's live DTC runner uses the same CGate owner and
    // read-side replication, but deliberately does not create a publisher or
    // reply listener. This is a transport capability boundary, not a send
    // mode or an execution authorization.
    bool read_only_market_data{false};
    bool allow_orders{false};
    std::uint32_t reply_timeout_ms{60000};
    // When enabled, FORTS_TRADE_REPL is opened after the negotiated POS.info
    // anchor is available, using that exact trades_rev/lifenum anchor.
    bool trade_replay_from_pos_anchor{false};
    plaza2::cgate::Plaza2CredentialConfig credentials{};
    plaza2::cgate::Plaza2CredentialConfig software_key{};
    std::uint32_t process_timeout_ms{50};
    std::chrono::milliseconds recovery_retry_interval{1000};
    // Recoverable external outages are operator-cancellable and do not have
    // an automatic terminal deadline. This threshold only raises the
    // operator-visible waiting diagnostic.
    std::chrono::milliseconds recovery_alert_after{60000};
    std::function<std::chrono::steady_clock::time_point()> recovery_now;

    // Local conservative cap; configure from the provisioned login limit, not a claimed exchange default.
    std::uint32_t publisher_messages_per_second{30};
    PublisherRateOwner publisher_rate_owner{PublisherRateOwner::Session};
    std::function<void(std::string_view, std::string_view)> event_log;
    std::function<void(const plaza2::cgate::Plaza2ListenerEvent&)> listener_event_log;
    std::function<std::uint64_t()> publisher_now_ms; // Empty uses steady_clock; injectable for offline boundary tests.
};

struct Plaza2TransportHealth {
    std::uint32_t connection{0}, publisher{0}, reply{0}, aggr{0}, public_deals{0};
    std::array<std::uint32_t, 8> private_states{};
    std::array<plaza2::generated::StreamCode, 8> private_streams{};
    std::size_t private_count{0};
    bool valid{false};
    bool private_active{false};
    [[nodiscard]] bool all_active() const noexcept {
        return valid && connection == 3 && publisher == 3 && reply == 3 && aggr == 3 && private_active;
    }
};

enum class Plaza2SessionOperation { Stopped, Starting, Running, Recovering, Failed };

enum class Plaza2RecoveryWaitState : std::uint8_t {
    None,
    WaitingForRouter,
    WaitingForPlaza,
    WaitingForService,
    RecoveringBootstrap,
};

[[nodiscard]] std::string_view plaza2_recovery_wait_state_name(Plaza2RecoveryWaitState state) noexcept;

struct Plaza2RecoveryStatus {
    Plaza2SessionOperation operation{Plaza2SessionOperation::Stopped};
    std::uint64_t generation{0}, attempts{0}, transitions{0};
    std::uint64_t error_time_ns{0};
    std::uint64_t wait_start_time_ns{0};
    std::uint64_t wait_duration_ms{0};
    bool alert_active{false};
    Plaza2RecoveryWaitState wait_state{Plaza2RecoveryWaitState::None};
    std::string involved_service;
    plaza2::cgate::Plaza2Error cause;
    Plaza2TransportHealth health;
};

class CgateSession final {
  public:
    explicit CgateSession(CgateSessionConfig config);
    ~CgateSession();

    CgateSession(const CgateSession&) = delete;
    CgateSession& operator=(const CgateSession&) = delete;
    CgateSession(CgateSession&&) noexcept;
    CgateSession& operator=(CgateSession&&) noexcept;

    [[nodiscard]] plaza2::cgate::Plaza2Error start();
    [[nodiscard]] plaza2::cgate::Plaza2Error poll();
    [[nodiscard]] plaza2::cgate::Plaza2Error stop();
    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool recovering() const noexcept;
    [[nodiscard]] const Plaza2RecoveryStatus& recovery_status() const noexcept;
    [[nodiscard]] Plaza2TransportHealth runtime_health() const;

    [[nodiscard]] const plaza2::cgate::Plaza2RuntimeProbeReport& probe_report() const noexcept;
    [[nodiscard]] const plaza2::private_state::Plaza2PrivateStateProjector& private_state() const noexcept;
    [[nodiscard]] const plaza2::cgate::Plaza2Aggr20BookProjector& aggr20_projector() const noexcept;
    [[nodiscard]] plaza2::cgate::Plaza2PublicDealsSnapshot
    public_deals_snapshot(std::uint64_t after_sequence = 0) const;
    [[nodiscard]] bool aggr_online() const noexcept;
    [[nodiscard]] bool aggr_snapshot_complete() const noexcept;
    [[nodiscard]] bool aggr_session_data_ready() const noexcept;
    [[nodiscard]] bool aggr_valid() const noexcept;
    [[nodiscard]] plaza2::cgate::Plaza2Aggr20Status aggr_status() const;
    [[nodiscard]] bool p2mqreply_open() const noexcept;
    [[nodiscard]] bool publisher_open() const noexcept;
    // Sanitized connection instance identity used for operator/certification
    // evidence.  This is the CGate app_name value, never a credential.
    [[nodiscard]] const std::string& connection_app_name() const noexcept;
    [[nodiscard]] plaza2::cgate::Plaza2PublisherCallCounts publisher_call_counts() const noexcept;
    [[nodiscard]] plaza2::cgate::Plaza2PublisherRateMetrics publisher_rate_metrics() const noexcept;
    [[nodiscard]] CgateSessionMode mode() const noexcept;
    [[nodiscard]] bool trade_replay_anchor_ready() const noexcept;
    [[nodiscard]] std::optional<Plaza2TradeReplayAnchor> trade_replay_anchor_used() const noexcept;

    struct ReplyEvent {
        std::uint32_t user_id{0};
        std::int32_t message_id{0};
        Plaza2TradeCommandKind command_kind{Plaza2TradeCommandKind::AddOrder};
        bool timed_out{false};
        std::vector<std::byte> raw_payload;
    };

    [[nodiscard]] std::vector<ReplyEvent> take_reply_events();
    [[nodiscard]] plaza2::private_state::PrivateRowChanges take_private_row_changes();
    [[nodiscard]] plaza2::cgate::Plaza2PublisherMessageResult post_command(const Plaza2TradeEncodedCommand& command,
                                                                           std::uint32_t user_id);
    [[nodiscard]] const std::string& last_callback_error() const noexcept;

  private:
    [[nodiscard]] plaza2::cgate::Plaza2PublisherMessageResult post_validated(std::string_view message_name,
                                                                             std::span<const std::byte> payload,
                                                                             std::uint32_t user_id, bool need_reply);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::plaza2_trade
