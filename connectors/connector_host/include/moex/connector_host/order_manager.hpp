#pragma once

#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2/cgate/plaza2_publisher_rate.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include "moex/plaza2_trade/plaza2_trade_codec.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <tuple>

namespace moex::connector_host {

enum class OrderState {
    PendingNew,
    Working,
    PartFilled,
    PendingCancel,
    PendingReplace,
    Filled,
    Cancelled,
    Rejected,
    Unknown
};
[[nodiscard]] std::string_view order_state_name(OrderState state) noexcept;
[[nodiscard]] bool terminal(OrderState state) noexcept;

struct OrderRequest {
    std::string client_order_id;
    std::int32_t isin_id{};
    plaza2_trade::Plaza2TradeSide side{plaza2_trade::Plaza2TradeSide::Buy};
    plaza2_trade::Plaza2TradeOrderType type{plaza2_trade::Plaza2TradeOrderType::Limit};
    std::string price, comment;
    std::int32_t quantity{};
};

struct ManagedOrder {
    OrderRequest request;
    OrderState state{OrderState::PendingNew};
    std::int32_t ext_id{}, sess_id{};
    std::int64_t order_id{}, remaining{}, executed{};
    bool cancel_requested{}, absence_reply{};
    bool operator_action_required{}, confirmed_by_replication{};
    bool add_unconfirmed{};
    bool execution_baseline_known{true};
    std::int64_t sent_utc_seconds{};
    std::set<std::int64_t> order_ids;
    std::string last_error;
};

struct RiskLimits {
    std::int32_t max_quantity{100};
    // Price * quantity in quote units; this is a configured exposure cap, not a margin estimate.
    std::int64_t max_notional_scaled{1000000000000LL};
    std::size_t max_open_orders{100};
    bool kill_switch{};
};

struct OrderManagerConfig {
    std::string broker_code, client_code;
    std::uint32_t max_commands_per_second{30};
    std::chrono::milliseconds reply_timeout{60000};
    std::chrono::seconds absence_margin{60};
    // Only definitive business rejections spend this budget.
    std::uint32_t max_cancel_attempts{3};
    std::chrono::milliseconds cancel_retry_base{1000}, cancel_retry_max{30000};
    RiskLimits risk;
    std::int32_t next_ext_id{1};
    std::uint32_t next_user_id{1};
};

// The owner loop calls every method on one thread. Commands remain queued across
// outages; Add is submitted at most once after an uncertain submission.
class OrderManager {
  public:
    using Clock = std::chrono::steady_clock;
    using Send = std::function<plaza2::cgate::Plaza2PublisherMessageResult(
        const plaza2_trade::Plaza2TradeEncodedCommand&, std::uint32_t)>;
    using Ready = std::function<bool(std::int32_t)>;
    using Terms = std::function<std::optional<plaza2::private_state::FutureSessionTerms>(std::int32_t)>;
    using Log = std::function<void(std::string_view, std::string_view)>;

    OrderManager(OrderManagerConfig config, Send send, Ready ready, Terms terms, Log log = {});
    [[nodiscard]] std::string place(OrderRequest request);
    [[nodiscard]] std::string cancel(std::string_view client_order_id);
    [[nodiscard]] std::string move(std::string_view client_order_id, std::string price, std::int32_t quantity);
    [[nodiscard]] std::string cancel_all(std::int32_t isin_id);
    void set_kill_switch(bool enabled);
    void poll(Clock::time_point now, std::int64_t utc_seconds);
    void on_reply(std::uint32_t user_id, const plaza2_trade::Plaza2TradeDecodedReply& reply, Clock::time_point now,
                  std::uint64_t trade_commit_sequence = 0);
    void on_timeout(std::uint32_t user_id, Clock::time_point now);
    // USERORDERBOOK is used only while rebuilding, never as a live Add gate.
    void observe_orders(std::span<const plaza2::private_state::OwnOrderSnapshot> rows, bool rebuilding = false);
    void invalidate_execution_baselines();
    void observe_trades(std::span<const plaza2::private_state::OwnTradeSnapshot> rows);
    // Call after applying TRADE deltas, including commits with no changed orders.
    void observe_trade_commit(std::uint64_t sequence);
    void prove_absence(std::int64_t trade_server_time, bool trade_online);
    [[nodiscard]] const std::map<std::string, ManagedOrder>& orders() const noexcept {
        return orders_;
    }
    [[nodiscard]] std::size_t queued() const noexcept {
        return cancels_.size() + adds_.size();
    }
    [[nodiscard]] bool operator_action_required() const noexcept;

  private:
    struct Command {
        plaza2_trade::Plaza2TradeEncodedCommand encoded;
        std::string key;
        std::string replacement_price;
        std::int32_t replacement_quantity{};
        Clock::time_point deadline{}, not_before{};
        std::uint32_t user_id{};
        std::int64_t target_order_id{};
        bool acknowledged{};
        std::uint32_t business_failures{};
        std::int32_t submitted_session{};
    };
    struct BulkCancellation {
        std::uint64_t after_commit_sequence{};
        bool awaiting_reply{true};
    };
    [[nodiscard]] static bool is_bulk_cancel(const Command& command) {
        return command.key.empty() &&
               command.encoded.command_kind == plaza2_trade::Plaza2TradeCommandKind::DelUserOrders &&
               command.encoded.isin_id.has_value();
    }
    struct Exposure {
        std::uint64_t notional{};
        bool active{}, invalid_price{}, unknown{}, operator_action{};
        std::int32_t terminal_session{};
    };
    struct MoveReservation {
        std::uint64_t price_units{};
        std::int32_t quantity{};
    };
    // Retain terminal identities for day relists and outstanding Add replies.
    struct TerminalLink {
        std::string key, price;
        std::int32_t isin_id{}, sess_id{}, ext_id{}, quantity{};
        plaza2_trade::Plaza2TradeSide side{};
        std::int64_t order_id{}, remaining{}, executed{}, sent_utc_seconds{};
        OrderState state{};
        std::vector<std::int64_t> order_ids;
        bool cancel_requested{}, execution_baseline_known{};
    };
    [[nodiscard]] std::string check_risk(const OrderRequest& request, std::size_t extra_orders,
                                         std::string_view exclude_key = {}) const;
    [[nodiscard]] Command encode(plaza2_trade::Plaza2TradeCommandRequest request, std::string key);
    [[nodiscard]] std::uint32_t reserve_user_id();
    [[nodiscard]] bool has_outstanding_command(std::string_view key, plaza2_trade::Plaza2TradeCommandKind kind) const;
    void recovery_cancel(ManagedOrder& order, bool explicit_retry = false);
    void enqueue_cancel(ManagedOrder& order, bool explicit_retry = false);
    void complete_timeout(Command command, Clock::time_point now);
    void retry_cancel(Command command, Clock::time_point now, bool business_rejection = false);
    void replay_deferred_trades();
    void emit(std::string_view kind, std::string_view fields) noexcept;
    void changed(const std::string& key);
    void refresh_exposure(const std::string& key);
    void erase_exposure(const std::string& key);
    [[nodiscard]] Exposure exposure(const std::string& key, const ManagedOrder& order) const;
    void advance_session(std::int32_t session);
    void prune_terminal();
    void add_charge(const Exposure& charge);
    void subtract_charge(const Exposure& charge);
    enum class RecoveredAdoption { NotFound, Adopted, Conflict };
    [[nodiscard]] RecoveredAdoption adopt_recovered_order(const std::string& key, std::int64_t official_id,
                                                          std::int32_t submitted_session);
    [[nodiscard]] bool has_uncertain_submission(std::int32_t session, std::int32_t isin,
                                                plaza2_trade::Plaza2TradeSide side) const;
    OrderManagerConfig config_;
    Send send_;
    Ready ready_;
    Terms terms_;
    Log log_;
    plaza2::cgate::Plaza2PublisherRateGate rate_;
    std::map<std::string, ManagedOrder> orders_;
    std::unordered_map<std::string, Exposure> exposures_;
    std::unordered_map<std::string, MoveReservation> move_reservations_;
    std::set<std::string> unknown_orders_, operator_orders_, used_client_ids_;
    std::map<std::int32_t, std::set<std::string>> terminal_orders_;
    std::unordered_map<std::int64_t, TerminalLink> terminal_links_;
    std::size_t active_orders_{}, invalid_prices_{};
    // Each order contributes at most cap+1; a two-word sum supports subtraction
    // even when reconstructed exposure is well above the configured cap.
    std::uint64_t notional_low_{}, notional_high_{};
    std::int32_t current_session_{}, previous_session_{};
    std::unordered_map<std::int64_t, std::string> order_index_;
    std::unordered_map<std::int32_t, std::string> ext_index_;
    std::set<std::tuple<std::int32_t, std::int64_t, bool>> deals_;
    std::unordered_map<std::int64_t, std::int64_t> filled_by_id_;
    std::map<std::pair<std::int32_t, std::int64_t>, plaza2::private_state::OwnOrderSnapshot> deferred_orders_;
    std::map<std::pair<std::int32_t, std::int64_t>, plaza2::private_state::OwnTradeSnapshot> deferred_trades_;
    std::map<std::int32_t, BulkCancellation> bulk_cancellations_;
    std::uint64_t trade_commit_sequence_{};
    std::deque<Command> cancels_, adds_;
    std::unordered_map<std::uint32_t, Command> pending_;
    Clock::time_point now_{};
    bool throttled_{}, logging_failed_{};
};

} // namespace moex::connector_host
