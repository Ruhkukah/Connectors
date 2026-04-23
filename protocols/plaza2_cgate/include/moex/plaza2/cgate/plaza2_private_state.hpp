#pragma once

#include "moex/plaza2/cgate/plaza2_fake_engine.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2::private_state {

enum class InstrumentKind : std::uint8_t {
    kUnknown = 0,
    kFuture = 1,
    kOption = 2,
    kMultileg = 3,
};

enum class PositionScope : std::uint8_t {
    kClient = 0,
    kSettlementAccount = 1,
};

struct ConnectorHealthSnapshot {
    bool open{false};
    bool closed{false};
    bool snapshot_active{false};
    bool online{false};
    bool transaction_open{false};
    std::uint64_t commit_count{0};
    std::uint64_t callback_error_count{0};
};

struct ResumeMarkersSnapshot {
    bool has_lifenum{false};
    std::uint64_t last_lifenum{0};
    std::string last_replstate;
};

struct StreamHealthSnapshot {
    generated::StreamCode stream_code{fake::kNoStreamCode};
    std::string stream_name;
    bool online{false};
    bool snapshot_complete{false};
    std::uint64_t clear_deleted_count{0};
    std::uint64_t committed_row_count{0};
    std::uint64_t last_commit_sequence{0};
    bool has_publication_state{false};
    std::int32_t publication_state{0};
    std::int64_t last_trades_rev{0};
    std::int64_t last_trades_lifenum{0};
    std::int64_t last_server_time{0};
    std::int64_t last_info_moment{0};
    std::int64_t last_event_id{0};
    std::int32_t last_event_type{0};
    std::string last_message;
};

struct TradingSessionSnapshot {
    std::int32_t sess_id{0};
    std::int64_t begin{0};
    std::int64_t end{0};
    std::int32_t state{0};
    std::int64_t inter_cl_begin{0};
    std::int64_t inter_cl_end{0};
    std::int32_t inter_cl_state{0};
    bool eve_on{false};
    std::int64_t eve_begin{0};
    std::int64_t eve_end{0};
    bool mon_on{false};
    std::int64_t mon_begin{0};
    std::int64_t mon_end{0};
    std::int64_t settl_sess_begin{0};
    std::int64_t clr_sess_begin{0};
    std::int64_t settl_price_calc_time{0};
    std::int64_t settl_sess_t1_begin{0};
    std::int64_t margin_call_fix_schedule{0};
};

struct InstrumentLegSnapshot {
    std::int32_t leg_isin_id{0};
    std::int32_t qty_ratio{0};
    std::int8_t leg_order_no{0};
};

struct InstrumentSnapshot {
    std::int32_t isin_id{0};
    std::int32_t sess_id{0};
    InstrumentKind kind{InstrumentKind::kUnknown};
    std::string isin;
    std::string short_isin;
    std::string name;
    std::string base_contract_code;
    std::int32_t fut_isin_id{0};
    std::int32_t option_series_id{0};
    std::int32_t inst_term{0};
    std::int32_t roundto{0};
    std::int32_t lot_volume{0};
    std::int32_t trade_mode_id{0};
    std::int32_t state{0};
    std::int32_t signs{0};
    bool put{false};
    bool is_spread{false};
    std::string min_step;
    std::string step_price;
    std::string settlement_price;
    std::string strike;
    std::int64_t last_trade_date{0};
    std::int64_t group_mask{0};
    std::int64_t trade_period_access{0};
    std::vector<InstrumentLegSnapshot> legs;
};

struct MatchingMapSnapshot {
    std::int32_t base_contract_id{0};
    std::int8_t matching_id{0};
};

struct LimitSnapshot {
    PositionScope scope{PositionScope::kClient};
    std::string account_code;
    bool limits_set{false};
    bool is_auto_update_limit{false};
    std::string money_free;
    std::string money_blocked;
    std::string vm_reserve;
    std::string fee;
    std::string money_old;
    std::string money_amount;
    std::string money_pledge_amount;
    std::string actual_amount_of_base_currency;
    std::string vm_intercl;
    std::string broker_fee;
    std::string penalty;
    std::string premium_intercl;
    std::string net_option_value;
};

struct PositionSnapshot {
    PositionScope scope{PositionScope::kClient};
    std::string account_code;
    std::int32_t isin_id{0};
    std::int8_t account_type{0};
    std::int64_t xpos{0};
    std::int64_t xbuys_qty{0};
    std::int64_t xsells_qty{0};
    std::int64_t xday_open_qty{0};
    std::int64_t xday_open_buys_qty{0};
    std::int64_t xday_open_sells_qty{0};
    std::int64_t xopen_qty{0};
    std::string waprice;
    std::string net_volume_rur;
    std::int64_t last_deal_id{0};
    std::int64_t last_quantity{0};
};

struct OwnOrderSnapshot {
    bool multileg{false};
    std::int64_t public_order_id{0};
    std::int64_t private_order_id{0};
    std::int32_t sess_id{0};
    std::int32_t isin_id{0};
    std::string client_code;
    std::string login_from;
    std::string comment;
    std::string price;
    std::int64_t public_amount{0};
    std::int64_t public_amount_rest{0};
    std::int64_t private_amount{0};
    std::int64_t private_amount_rest{0};
    std::int64_t id_deal{0};
    std::int64_t xstatus{0};
    std::int64_t xstatus2{0};
    std::int8_t dir{0};
    std::int8_t public_action{0};
    std::int8_t private_action{0};
    std::int64_t moment{0};
    std::uint64_t moment_ns{0};
    std::int32_t ext_id{0};
    bool from_trade_repl{false};
    bool from_user_book{false};
    bool from_current_day{false};
};

struct OwnTradeSnapshot {
    bool multileg{false};
    std::int64_t id_deal{0};
    std::int32_t sess_id{0};
    std::int32_t isin_id{0};
    std::string price;
    std::string rate_price;
    std::string swap_price;
    std::int64_t amount{0};
    std::int64_t public_order_id_buy{0};
    std::int64_t public_order_id_sell{0};
    std::int64_t private_order_id_buy{0};
    std::int64_t private_order_id_sell{0};
    std::string code_buy;
    std::string code_sell;
    std::string comment_buy;
    std::string comment_sell;
    std::string login_buy;
    std::string login_sell;
    std::int64_t moment{0};
    std::uint64_t moment_ns{0};
};

class Plaza2PrivateStateProjector final : public fake::CommitListener {
  public:
    Plaza2PrivateStateProjector();
    ~Plaza2PrivateStateProjector() override;

    Plaza2PrivateStateProjector(const Plaza2PrivateStateProjector&) = delete;
    Plaza2PrivateStateProjector& operator=(const Plaza2PrivateStateProjector&) = delete;
    Plaza2PrivateStateProjector(Plaza2PrivateStateProjector&&) noexcept;
    Plaza2PrivateStateProjector& operator=(Plaza2PrivateStateProjector&&) noexcept;

    void reset();

    [[nodiscard]] const ConnectorHealthSnapshot& connector_health() const;
    [[nodiscard]] const ResumeMarkersSnapshot& resume_markers() const;
    [[nodiscard]] std::span<const StreamHealthSnapshot> stream_health() const;
    [[nodiscard]] std::span<const TradingSessionSnapshot> sessions() const;
    [[nodiscard]] std::span<const InstrumentSnapshot> instruments() const;
    [[nodiscard]] std::span<const MatchingMapSnapshot> matching_map() const;
    [[nodiscard]] std::span<const LimitSnapshot> limits() const;
    [[nodiscard]] std::span<const PositionSnapshot> positions() const;
    [[nodiscard]] std::span<const OwnOrderSnapshot> own_orders() const;
    [[nodiscard]] std::span<const OwnTradeSnapshot> own_trades() const;

    void on_event(const fake::ScenarioSpec& scenario, const fake::EventSpec& event,
                  const fake::EngineState& state) override;
    void on_stream_row(const fake::ScenarioSpec& scenario, const fake::EventSpec& event, const fake::RowSpec& row,
                       std::span<const fake::FieldValueSpec> fields, const fake::EngineState& state) override;
    void on_transaction_commit(const fake::ScenarioSpec& scenario, const fake::EventSpec& commit_event,
                               const fake::EngineState& state) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::plaza2::private_state
