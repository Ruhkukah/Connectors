#pragma once

#include "moex/plaza2/cgate/plaza2_fake_engine.hpp"
#include "moex/plaza2_trade/plaza2_trade_codec.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moex::plaza2_trade {

enum class Plaza2TradeFakeSessionState {
    Disconnected,
    Established,
    Recovering,
    Terminated,
};

enum class Plaza2TradeFakeOutcomeStatus {
    Accepted,
    Rejected,
    DuplicateClientTransactionId,
    UnknownOrder,
    InvalidState,
    UnsupportedCommand,
    ValidationFailed,
};

enum class Plaza2TradeFakeOrderStatus {
    Active,
    Canceled,
    Moved,
    Filled,
    Rejected,
};

struct Plaza2TradeFakeOrder {
    std::int64_t synthetic_order_id{0};
    std::int64_t client_transaction_id{0};
    std::int32_t instrument_id{0};
    Plaza2TradeSide side{Plaza2TradeSide::Buy};
    std::string price;
    std::int64_t quantity{0};
    std::int64_t remaining_quantity{0};
    std::string client_code;
    Plaza2TradeFakeOrderStatus status{Plaza2TradeFakeOrderStatus::Active};
    Plaza2TradeCommandKind original_command_family{Plaza2TradeCommandKind::AddOrder};
    Plaza2TradeCommandKind last_command_family{Plaza2TradeCommandKind::AddOrder};
};

struct Plaza2TradeFakeReplicationBatch {
    std::shared_ptr<std::deque<std::string>> text_storage{std::make_shared<std::deque<std::string>>()};
    std::vector<moex::plaza2::generated::StreamCode> streams;
    std::vector<moex::plaza2::fake::EventSpec> events;
    std::vector<moex::plaza2::fake::RowSpec> rows;
    std::vector<moex::plaza2::fake::FieldValueSpec> fields;
    std::vector<moex::plaza2::fake::InvariantSpec> invariants;
    moex::plaza2::fake::ScenarioSpec scenario;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] moex::plaza2::fake::ScenarioDataView view() const;
};

struct Plaza2TradeFakeSubmitResult {
    Plaza2TradeCommandKind command_family{Plaza2TradeCommandKind::AddOrder};
    std::int64_t client_transaction_id{0};
    std::int32_t reply_msgid{0};
    Plaza2TradeFakeOutcomeStatus status{Plaza2TradeFakeOutcomeStatus::Rejected};
    std::string diagnostic;
    std::optional<std::int64_t> generated_order_id;
    std::optional<std::int64_t> generated_deal_id;
    Plaza2TradeDecodedReply decoded_reply;
    Plaza2TradeEncodedCommand encoded_command;
    Plaza2TradeFakeReplicationBatch replication;
};

class Plaza2TradeFakeSession {
  public:
    Plaza2TradeFakeSession();

    void establish();
    void disconnect();
    void recover();
    void terminate();

    [[nodiscard]] Plaza2TradeFakeSessionState state() const noexcept;
    [[nodiscard]] std::span<const Plaza2TradeFakeOrder> orders() const noexcept;

    [[nodiscard]] Plaza2TradeFakeSubmitResult submit(const Plaza2TradeCommandRequest& request);
    [[nodiscard]] Plaza2TradeFakeSubmitResult simulate_fill(std::int64_t order_id, std::int64_t deal_id,
                                                            std::int64_t fill_quantity);

  private:
    Plaza2TradeFakeSessionState state_{Plaza2TradeFakeSessionState::Disconnected};
    Plaza2TradeCodec codec_;
    std::vector<Plaza2TradeFakeOrder> orders_;
    std::vector<std::int64_t> seen_client_transaction_ids_;
    std::int64_t next_order_id_{2000001};
    std::uint64_t next_repl_id_{1};
    std::uint64_t next_moment_{1700000000};
};

[[nodiscard]] const char* fake_outcome_name(Plaza2TradeFakeOutcomeStatus status) noexcept;

} // namespace moex::plaza2_trade
