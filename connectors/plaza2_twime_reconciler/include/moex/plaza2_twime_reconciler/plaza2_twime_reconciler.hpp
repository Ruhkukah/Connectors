#pragma once

#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/twime_trade/twime_recovery_state.hpp"
#include "moex/twime_trade/twime_session_health.hpp"
#include "moex/twime_trade/twime_session_metrics.hpp"
#include "moex/twime_sbe/twime_types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moex::plaza2_twime_reconciler {

enum class ReconciliationSource : std::uint8_t {
    Unknown = 0,
    Twime = 1,
    Plaza = 2,
};

enum class MatchMode : std::uint8_t {
    None = 0,
    DirectIdentifier = 1,
    ExactFallbackTuple = 2,
    AmbiguousCandidates = 3,
};

enum class Side : std::uint8_t {
    Unknown = 0,
    Buy = 1,
    Sell = 2,
};

enum class OrderStatus : std::uint8_t {
    ProvisionalTwime = 0,
    Confirmed = 1,
    Rejected = 2,
    Canceled = 3,
    Filled = 4,
    Ambiguous = 5,
    Diverged = 6,
    Stale = 7,
    Unknown = 8,
};

enum class TradeStatus : std::uint8_t {
    TwimeOnly = 0,
    PlazaOnly = 1,
    Matched = 2,
    Diverged = 3,
    Ambiguous = 4,
};

enum class TwimeOrderInputKind : std::uint8_t {
    NewIntent = 0,
    NewAccepted = 1,
    ReplaceIntent = 2,
    ReplaceAccepted = 3,
    CancelIntent = 4,
    CancelAccepted = 5,
    Rejected = 6,
};

enum class TwimeTradeInputKind : std::uint8_t {
    Execution = 0,
};

struct TwimeSourceHealthInput {
    twime_trade::TwimeSessionHealthSnapshot session_health{};
    twime_trade::TwimeSessionMetrics session_metrics{};
};

struct TwimeOrderInput {
    TwimeOrderInputKind kind{TwimeOrderInputKind::NewIntent};
    std::uint64_t logical_sequence{0};
    std::uint64_t cl_ord_id{0};
    std::int64_t order_id{0};
    std::int64_t prev_order_id{0};
    std::int32_t trading_session_id{0};
    std::int32_t security_id{0};
    std::int32_t cl_ord_link_id{0};
    std::string account;
    std::string compliance_id;
    Side side{Side::Unknown};
    bool multileg{false};
    bool has_price{false};
    std::int64_t price_mantissa{0};
    bool has_order_qty{false};
    std::uint32_t order_qty{0};
    std::int64_t reject_code{0};
};

struct TwimeTradeInput {
    TwimeTradeInputKind kind{TwimeTradeInputKind::Execution};
    std::uint64_t logical_sequence{0};
    std::uint64_t cl_ord_id{0};
    std::int64_t order_id{0};
    std::int64_t trade_id{0};
    std::int32_t trading_session_id{0};
    std::int32_t security_id{0};
    Side side{Side::Unknown};
    bool multileg{false};
    bool has_price{false};
    std::int64_t price_mantissa{0};
    bool has_last_qty{false};
    std::uint32_t last_qty{0};
    bool has_order_qty{false};
    std::uint32_t order_qty{0};
};

struct TwimeNormalizeResult {
    bool ok{false};
    std::string error;
    std::optional<TwimeOrderInput> order_input;
    std::optional<TwimeTradeInput> trade_input;
};

struct TwimeNormalizedInputBatch {
    bool ok{true};
    std::string error;
    std::optional<TwimeSourceHealthInput> source_health;
    std::vector<TwimeOrderInput> order_inputs;
    std::vector<TwimeTradeInput> trade_inputs;
};

struct PlazaSourceHealthInput {
    plaza2::private_state::ConnectorHealthSnapshot connector_health{};
    plaza2::private_state::ResumeMarkersSnapshot resume_markers{};
    std::vector<plaza2::private_state::StreamHealthSnapshot> stream_health;
    bool required_private_streams_ready{false};
    bool invalidated{false};
    std::string invalidation_reason;
};

struct PlazaOrderInput {
    bool multileg{false};
    std::int64_t public_order_id{0};
    std::int64_t private_order_id{0};
    std::int32_t sess_id{0};
    std::int32_t isin_id{0};
    std::string client_code;
    std::string login_from;
    std::string comment;
    std::string price_text;
    bool has_price{false};
    std::int64_t price_mantissa{0};
    std::int64_t public_amount{0};
    std::int64_t public_amount_rest{0};
    std::int64_t private_amount{0};
    std::int64_t private_amount_rest{0};
    std::int64_t id_deal{0};
    std::int64_t xstatus{0};
    std::int64_t xstatus2{0};
    Side side{Side::Unknown};
    std::int8_t public_action{0};
    std::int8_t private_action{0};
    std::int64_t moment{0};
    std::uint64_t moment_ns{0};
    std::int32_t ext_id{0};
    bool from_trade_repl{false};
    bool from_user_book{false};
    bool from_current_day{false};
};

struct PlazaTradeInput {
    bool multileg{false};
    std::int64_t trade_id{0};
    std::int32_t sess_id{0};
    std::int32_t isin_id{0};
    std::string price_text;
    bool has_price{false};
    std::int64_t price_mantissa{0};
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

struct PlazaCommittedSnapshotInput {
    std::uint64_t logical_sequence{0};
    PlazaSourceHealthInput source_health{};
    std::vector<PlazaOrderInput> orders;
    std::vector<PlazaTradeInput> trades;
};

struct TwimeOrderView {
    bool present{false};
    TwimeOrderInputKind last_kind{TwimeOrderInputKind::NewIntent};
    std::uint64_t cl_ord_id{0};
    std::int64_t order_id{0};
    std::int64_t prev_order_id{0};
    std::int32_t trading_session_id{0};
    std::int32_t security_id{0};
    std::int32_t cl_ord_link_id{0};
    std::string account;
    std::string compliance_id;
    Side side{Side::Unknown};
    bool multileg{false};
    bool has_price{false};
    std::int64_t price_mantissa{0};
    bool has_order_qty{false};
    std::uint32_t order_qty{0};
    std::int64_t reject_code{0};
    std::uint64_t last_logical_sequence{0};
    std::uint64_t last_logical_step{0};
    bool terminal_reject{false};
    bool terminal_cancel{false};
};

struct PlazaOrderView {
    bool present{false};
    bool multileg{false};
    std::int64_t public_order_id{0};
    std::int64_t private_order_id{0};
    std::int32_t sess_id{0};
    std::int32_t isin_id{0};
    std::string client_code;
    std::string login_from;
    std::string comment;
    std::string price_text;
    bool has_price{false};
    std::int64_t price_mantissa{0};
    std::int64_t public_amount{0};
    std::int64_t public_amount_rest{0};
    std::int64_t private_amount{0};
    std::int64_t private_amount_rest{0};
    std::int64_t id_deal{0};
    std::int64_t xstatus{0};
    std::int64_t xstatus2{0};
    Side side{Side::Unknown};
    std::int8_t public_action{0};
    std::int8_t private_action{0};
    std::int64_t moment{0};
    std::uint64_t moment_ns{0};
    std::int32_t ext_id{0};
    bool from_trade_repl{false};
    bool from_user_book{false};
    bool from_current_day{false};
    std::uint64_t last_logical_sequence{0};
};

struct TwimeTradeView {
    bool present{false};
    TwimeTradeInputKind last_kind{TwimeTradeInputKind::Execution};
    std::uint64_t cl_ord_id{0};
    std::int64_t order_id{0};
    std::int64_t trade_id{0};
    std::int32_t trading_session_id{0};
    std::int32_t security_id{0};
    Side side{Side::Unknown};
    bool multileg{false};
    bool has_price{false};
    std::int64_t price_mantissa{0};
    bool has_last_qty{false};
    std::uint32_t last_qty{0};
    bool has_order_qty{false};
    std::uint32_t order_qty{0};
    std::uint64_t last_logical_sequence{0};
    std::uint64_t last_logical_step{0};
};

struct PlazaTradeView {
    bool present{false};
    bool multileg{false};
    std::int64_t trade_id{0};
    std::int32_t sess_id{0};
    std::int32_t isin_id{0};
    std::string price_text;
    bool has_price{false};
    std::int64_t price_mantissa{0};
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
    std::uint64_t last_logical_sequence{0};
};

struct ReconciledOrderSnapshot {
    OrderStatus status{OrderStatus::Unknown};
    MatchMode match_mode{MatchMode::None};
    ReconciliationSource last_update_source{ReconciliationSource::Unknown};
    std::uint64_t last_update_logical_sequence{0};
    std::uint64_t last_update_logical_step{0};
    bool plaza_revalidation_required{false};
    std::string fault_reason;
    TwimeOrderView twime;
    PlazaOrderView plaza;
};

struct ReconciledTradeSnapshot {
    TradeStatus status{TradeStatus::TwimeOnly};
    MatchMode match_mode{MatchMode::None};
    ReconciliationSource last_update_source{ReconciliationSource::Unknown};
    std::uint64_t last_update_logical_sequence{0};
    std::uint64_t last_update_logical_step{0};
    bool plaza_revalidation_required{false};
    std::string fault_reason;
    TwimeTradeView twime;
    PlazaTradeView plaza;
};

struct TwimeSourceSummary {
    bool present{false};
    twime_trade::TwimeSessionState session_state{twime_trade::TwimeSessionState::Created};
    bool transport_open{false};
    bool session_active{false};
    bool reject_seen{false};
    std::int64_t last_reject_code{0};
    std::uint64_t next_expected_inbound_seq{1};
    std::uint64_t next_outbound_seq{1};
    std::uint64_t reconnect_attempts{0};
    std::uint64_t faults{0};
    std::uint64_t remote_closes{0};
    std::uint64_t last_transition_time_ms{0};
};

struct PlazaSourceSummary {
    bool present{false};
    bool connector_open{false};
    bool connector_online{false};
    bool snapshot_active{false};
    bool required_private_streams_ready{false};
    bool invalidated{false};
    std::uint64_t last_lifenum{0};
    std::string last_replstate;
    std::size_t required_stream_count{0};
    std::size_t online_stream_count{0};
    std::size_t snapshot_complete_stream_count{0};
    std::string last_invalidation_reason;
};

struct Plaza2TwimeReconcilerHealthSnapshot {
    std::uint64_t logical_step{0};
    std::size_t total_provisional_orders{0};
    std::size_t total_confirmed_orders{0};
    std::size_t total_rejected_orders{0};
    std::size_t total_canceled_orders{0};
    std::size_t total_filled_orders{0};
    std::size_t total_diverged_orders{0};
    std::size_t total_ambiguous_orders{0};
    std::size_t total_stale_provisional_orders{0};
    std::size_t total_unmatched_twime_orders{0};
    std::size_t total_unmatched_plaza_orders{0};
    std::size_t total_matched_trades{0};
    std::size_t total_diverged_trades{0};
    std::size_t total_ambiguous_trades{0};
    std::size_t total_unmatched_twime_trades{0};
    std::size_t total_unmatched_plaza_trades{0};
    std::size_t plaza_revalidation_pending_orders{0};
    std::size_t plaza_revalidation_pending_trades{0};
    TwimeSourceSummary twime;
    PlazaSourceSummary plaza;
};

TwimeNormalizeResult normalize_twime_outbound_request(const twime_sbe::TwimeEncodeRequest& request,
                                                      std::uint64_t logical_sequence);
TwimeNormalizeResult normalize_twime_inbound_journal_entry(const twime_trade::TwimeJournalEntry& entry,
                                                           std::uint64_t logical_sequence);
TwimeNormalizedInputBatch
collect_twime_reconciliation_inputs(std::span<const twime_sbe::TwimeEncodeRequest> outbound_requests,
                                    std::span<const twime_trade::TwimeJournalEntry> inbound_journal,
                                    const twime_trade::TwimeSessionHealthSnapshot* session_health = nullptr,
                                    const twime_trade::TwimeSessionMetrics* session_metrics = nullptr,
                                    std::uint64_t starting_sequence = 1);
TwimeSourceHealthInput make_twime_source_health_input(const twime_trade::TwimeSessionHealthSnapshot& session_health,
                                                      const twime_trade::TwimeSessionMetrics& session_metrics);
PlazaCommittedSnapshotInput
make_plaza_committed_snapshot(const plaza2::private_state::Plaza2PrivateStateProjector& projector,
                              std::uint64_t logical_sequence);

class Plaza2TwimeReconciler {
  public:
    explicit Plaza2TwimeReconciler(std::uint64_t stale_after_steps = 4);
    ~Plaza2TwimeReconciler();

    Plaza2TwimeReconciler(const Plaza2TwimeReconciler&) = delete;
    Plaza2TwimeReconciler& operator=(const Plaza2TwimeReconciler&) = delete;
    Plaza2TwimeReconciler(Plaza2TwimeReconciler&&) noexcept;
    Plaza2TwimeReconciler& operator=(Plaza2TwimeReconciler&&) noexcept;

    [[nodiscard]] Plaza2TwimeReconciler clone() const;

    void reset();
    void set_stale_after_steps(std::uint64_t stale_after_steps);
    void advance_steps(std::uint64_t steps = 1);

    void update_twime_source_health(const TwimeSourceHealthInput& input);
    void apply_twime_order_input(const TwimeOrderInput& input);
    void apply_twime_trade_input(const TwimeTradeInput& input);
    void apply_twime_inputs(const TwimeNormalizedInputBatch& batch);
    void apply_plaza_snapshot(const PlazaCommittedSnapshotInput& snapshot);

    [[nodiscard]] const Plaza2TwimeReconcilerHealthSnapshot& health() const noexcept;
    [[nodiscard]] std::span<const ReconciledOrderSnapshot> orders() const noexcept;
    [[nodiscard]] std::span<const ReconciledTradeSnapshot> trades() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::plaza2_twime_reconciler
