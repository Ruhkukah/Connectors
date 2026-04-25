#pragma once

#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"
#include "moex/plaza2/cgate/plaza2_live_session_runner.hpp"
#include "moex/plaza2_trade/plaza2_trade_codec.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2::cgate {}

namespace moex::plaza2_trade {

namespace cgate = moex::plaza2::cgate;

enum class Plaza2TradeOrderEntryFailure {
    None,
    MissingArmFlag,
    ProductionProfileRejected,
    InvalidOrderProfile,
    RuntimeProbeFailed,
    SchemeDriftIncompatible,
    PrivateStateNotReady,
    Aggr20NotReady,
    CommandValidationFailed,
    PublisherOpenFailed,
    CommandSendFailed,
    ReplyRejected,
    ReplyTimeout,
    ReplicationConfirmationTimeout,
    CancelSendFailed,
    CancelReplyRejected,
    CancelConfirmationTimeout,
    Unknown,
};

enum class Plaza2TradeOrderEntryMode {
    DryRun,
    SendTestOrder,
};

struct Plaza2TradeTinyOrderConfig {
    std::int32_t isin_id{0};
    std::string broker_code;
    std::string client_code;
    Plaza2TradeSide side{Plaza2TradeSide::Buy};
    Plaza2TradeOrderType order_type{Plaza2TradeOrderType::Limit};
    std::string price;
    std::int32_t quantity{0};
    std::int32_t max_quantity{1};
    std::int32_t ext_id{0};
    std::string client_transaction_id_prefix;
    std::string comment;
};

struct Plaza2TradeOrderEntryConfig {
    std::string profile_id;
    Plaza2TradeOrderEntryMode mode{Plaza2TradeOrderEntryMode::DryRun};
    cgate::Plaza2RuntimeArmState arm_state{};
    bool order_entry_armed{false};
    bool tiny_order_armed{false};
    bool send_test_order{false};
    bool production_profile{false};
    bool enable_aggr20_sanity{false};
    std::uint32_t max_polls{512};
    cgate::Plaza2LiveSessionConfig private_session{};
    cgate::Plaza2Aggr20MdConfig aggr20_sanity{};
    cgate::Plaza2LivePublisherConfig publisher{};
    Plaza2TradeTinyOrderConfig tiny_order{};
};

struct Plaza2TradeOrderEntryEvidence {
    bool command_encoded{false};
    bool command_submitted{false};
    bool reply_received{false};
    bool reply_accepted{false};
    bool private_order_seen{false};
    bool user_orderbook_seen{false};
    bool cancel_submitted{false};
    bool cancel_confirmed{false};
    bool dry_run{true};
    std::int32_t add_order_msgid{0};
    std::int32_t del_order_msgid{0};
    std::uint64_t add_user_id{0};
    std::uint64_t cancel_user_id{0};
    std::int64_t confirmed_order_id{0};
    std::string failure_classification;
    std::string diagnostic;
};

struct Plaza2TradeOrderEntryResult {
    bool ok{false};
    Plaza2TradeOrderEntryFailure failure{Plaza2TradeOrderEntryFailure::Unknown};
    std::string message;
    Plaza2TradeOrderEntryEvidence evidence;
};

[[nodiscard]] std::string_view plaza2_trade_order_entry_failure_name(Plaza2TradeOrderEntryFailure failure) noexcept;
[[nodiscard]] Plaza2TradeOrderEntryResult
validate_plaza2_trade_order_entry_config(const Plaza2TradeOrderEntryConfig& config);
[[nodiscard]] AddOrderRequest make_plaza2_trade_add_order_request(const Plaza2TradeTinyOrderConfig& order);
[[nodiscard]] DelOrderRequest make_plaza2_trade_del_order_request(const Plaza2TradeTinyOrderConfig& order,
                                                                  std::int64_t order_id);

class Plaza2TradeOrderEntryGateway {
  public:
    virtual ~Plaza2TradeOrderEntryGateway() = default;
    [[nodiscard]] virtual Plaza2TradeOrderEntryResult start_private_state() = 0;
    [[nodiscard]] virtual Plaza2TradeOrderEntryResult poll_private_state() = 0;
    [[nodiscard]] virtual bool private_ready() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::int64_t> find_order_id(std::int32_t ext_id,
                                                                    std::string_view client_code) const = 0;
    [[nodiscard]] virtual bool order_cancelled(std::int64_t order_id) const = 0;
    [[nodiscard]] virtual Plaza2TradeOrderEntryResult open_publisher() = 0;
    [[nodiscard]] virtual Plaza2TradeOrderEntryResult post_command(const Plaza2TradeEncodedCommand& command,
                                                                   std::uint64_t user_id) = 0;
    virtual void stop() = 0;
};

class Plaza2TradeLiveOrderEntryGateway final : public Plaza2TradeOrderEntryGateway {
  public:
    explicit Plaza2TradeLiveOrderEntryGateway(Plaza2TradeOrderEntryConfig config);
    ~Plaza2TradeLiveOrderEntryGateway() override;

    [[nodiscard]] Plaza2TradeOrderEntryResult start_private_state() override;
    [[nodiscard]] Plaza2TradeOrderEntryResult poll_private_state() override;
    [[nodiscard]] bool private_ready() const noexcept override;
    [[nodiscard]] std::optional<std::int64_t> find_order_id(std::int32_t ext_id,
                                                            std::string_view client_code) const override;
    [[nodiscard]] bool order_cancelled(std::int64_t order_id) const override;
    [[nodiscard]] Plaza2TradeOrderEntryResult open_publisher() override;
    [[nodiscard]] Plaza2TradeOrderEntryResult post_command(const Plaza2TradeEncodedCommand& command,
                                                           std::uint64_t user_id) override;
    void stop() override;

  private:
    Plaza2TradeOrderEntryConfig config_;
    cgate::Plaza2LiveSessionRunner private_runner_;
};

class Plaza2TradeTestOrderRunner {
  public:
    Plaza2TradeTestOrderRunner(Plaza2TradeOrderEntryConfig config,
                               std::unique_ptr<Plaza2TradeOrderEntryGateway> gateway);
    ~Plaza2TradeTestOrderRunner();

    [[nodiscard]] Plaza2TradeOrderEntryResult run();

  private:
    Plaza2TradeOrderEntryConfig config_;
    std::unique_ptr<Plaza2TradeOrderEntryGateway> gateway_;
    Plaza2TradeCodec codec_;
};

} // namespace moex::plaza2_trade
