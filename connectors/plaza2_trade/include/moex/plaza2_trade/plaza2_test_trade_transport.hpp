#pragma once

#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"
#include "moex/plaza2/cgate/plaza2_credential_provider.hpp"
#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include "moex/plaza2_trade/plaza2_order_lifecycle.hpp"

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

namespace moex::plaza2_trade {

struct Plaza2TestTradeStreamConfig {
    plaza2::generated::StreamCode stream_code{plaza2::cgate::kNoStreamCode};
    std::string settings;
    std::string open_settings;
};

// Static intent is the human-authorized object. It contains no live BBO or
// operator-supplied age; those values are captured in an execution receipt.
struct Plaza2AuthorizedOrderIntent {
    std::string sha256;
    std::string add_payload_sha256;
    std::string recovery_payload_sha256;
    std::int64_t isin_id{0};
    std::string base_contract_code;
    Plaza2TradeSide side{Plaza2TradeSide::Buy};
    std::string price;
    std::int64_t quantity{1};
    std::int32_t ext_id{0};
    std::uint32_t add_user_id{0};
    std::uint32_t cancel_user_id{0};
    std::uint32_t recovery_user_id{0};
    std::string policy_version;
    std::string policy_sha256;
    std::uint32_t max_distance_ticks{0};
};

struct Plaza2ExecutionSafetyReceipt {
    std::string authorized_intent_sha256;
    std::int64_t target_isin_id{0};
    std::optional<plaza2::cgate::Plaza2Aggr20InstrumentSnapshot> aggr20;
    std::optional<plaza2::private_state::InstrumentSnapshot> instrument;
    std::optional<plaza2::private_state::TradingSessionSnapshot> session;
    std::optional<plaza2::private_state::LimitSnapshot> limit;
    std::chrono::milliseconds local_age{0};
    bool passive_non_marketable{false};
    bool bbo_distance_allowed{false};
    bool quantity_one{false};
    bool private_streams_ready{false};
    bool p2mqreply_open{false};
    bool publisher_open{false};
    bool trading_capable{false};
    std::string canonical_json;
    std::string sha256;
};

// This host is deliberately TEST-only and owns the one Env/connection used by
// private replication, AGGR20, p2mqreply, and the publisher.
struct Plaza2TestSessionHostConfig {
    plaza2::cgate::Plaza2Settings runtime{};
    std::string connection_settings;
    std::string connection_open_settings;
    std::vector<Plaza2TestTradeStreamConfig> private_streams;
    Plaza2TestTradeStreamConfig aggr20_stream;
    std::string publisher_settings;
    std::string publisher_open_settings;
    plaza2::cgate::Plaza2CredentialConfig credentials{};
    plaza2::cgate::Plaza2CredentialConfig software_key{};
    std::uint32_t process_timeout_ms{50};
};

class Plaza2TestSessionHost final {
  public:
    explicit Plaza2TestSessionHost(Plaza2TestSessionHostConfig config);
    ~Plaza2TestSessionHost();

    Plaza2TestSessionHost(const Plaza2TestSessionHost&) = delete;
    Plaza2TestSessionHost& operator=(const Plaza2TestSessionHost&) = delete;
    Plaza2TestSessionHost(Plaza2TestSessionHost&&) noexcept;
    Plaza2TestSessionHost& operator=(Plaza2TestSessionHost&&) noexcept;

    [[nodiscard]] plaza2::cgate::Plaza2Error start();
    [[nodiscard]] plaza2::cgate::Plaza2Error poll();
    [[nodiscard]] plaza2::cgate::Plaza2Error stop();
    [[nodiscard]] bool started() const noexcept;

    [[nodiscard]] const plaza2::cgate::Plaza2RuntimeProbeReport& probe_report() const noexcept;
    [[nodiscard]] const plaza2::private_state::Plaza2PrivateStateProjector& private_state() const noexcept;
    [[nodiscard]] const plaza2::cgate::Plaza2Aggr20BookProjector& aggr20_projector() const noexcept;
    [[nodiscard]] bool p2mqreply_open() const noexcept;
    [[nodiscard]] bool publisher_open() const noexcept;

    struct ReplyEvent {
        std::uint32_t user_id{0};
        std::int32_t message_id{0};
        Plaza2TradeCommandKind command_kind{Plaza2TradeCommandKind::AddOrder};
        bool timed_out{false};
        std::vector<std::byte> raw_payload;
    };

    [[nodiscard]] std::vector<ReplyEvent> take_reply_events();
    [[nodiscard]] const std::string& last_callback_error() const noexcept;

    [[nodiscard]] plaza2::cgate::Plaza2PublisherMessageResult
    post(std::string_view message_name, std::span<const std::byte> payload, std::uint32_t user_id, bool need_reply);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct Plaza2TestTradeTransportConfig {
    Plaza2TestSessionHostConfig host;
    std::int64_t target_isin_id{0};
    std::int32_t target_session_id{0};
    std::chrono::milliseconds max_aggr20_age{std::chrono::seconds(2)};
    std::optional<Plaza2AuthorizedOrderIntent> authorized_intent;
    std::filesystem::path execution_safety_receipt_path;
    bool require_zero_starting_position{false};
    std::string target_tick_size;
    std::string target_price;
    Plaza2TradeSide target_side{Plaza2TradeSide::Buy};
    std::uint32_t target_max_distance_ticks{0};
    std::int32_t observation_ext_id{0};
    std::string observation_client_code;
    Plaza2TradeSide observation_side{Plaza2TradeSide::Buy};
    std::int64_t observation_quantity{1};
};

class Plaza2TestTradeTransport final : public OrderLifecycleTransport {
  public:
    explicit Plaza2TestTradeTransport(Plaza2TestTradeTransportConfig config);
    ~Plaza2TestTradeTransport() override;

    Plaza2TestTradeTransport(const Plaza2TestTradeTransport&) = delete;
    Plaza2TestTradeTransport& operator=(const Plaza2TestTradeTransport&) = delete;
    Plaza2TestTradeTransport(Plaza2TestTradeTransport&&) noexcept;
    Plaza2TestTradeTransport& operator=(Plaza2TestTradeTransport&&) noexcept;

    [[nodiscard]] plaza2::cgate::Plaza2PublisherMessageResult post(const Plaza2TradeEncodedCommand& command,
                                                                   std::uint32_t user_id) override;
    [[nodiscard]] plaza2::cgate::Plaza2PublisherMessageResult
    post_exact_ext_id_recovery(const Plaza2TradeEncodedCommand& command, std::uint32_t user_id) override;
    [[nodiscard]] OrderLifecyclePollResult poll(std::chrono::steady_clock::time_point deadline) override;
    [[nodiscard]] OrderLifecyclePollResult reconcile() override;

    [[nodiscard]] const Plaza2TestSessionHost& host() const noexcept;
    [[nodiscard]] Plaza2TestSessionHost& host() noexcept;
    [[nodiscard]] const std::optional<Plaza2ExecutionSafetyReceipt>& last_execution_safety_receipt() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::plaza2_trade
