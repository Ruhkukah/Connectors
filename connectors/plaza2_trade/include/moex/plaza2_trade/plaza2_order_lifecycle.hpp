#pragma once

#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include "moex/plaza2_trade/plaza2_trade_codec.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2_trade {

enum class OrderLifecycleState : std::uint8_t {
    DefinitelyNotSent = 0,
    PossiblySent = 1,
    Posted = 2,
    Rejected = 3,
    Working = 4,
    PartiallyFilled = 5,
    Filled = 6,
    CancelPending = 7,
    Cancelled = 8,
    UnresolvedOrphanIncident = 9,
    Idle = 10,
    Authorized = 11,
    AddPending = 12,
};

[[nodiscard]] std::string_view order_lifecycle_state_name(OrderLifecycleState state) noexcept;

struct MatchedOwnTrade {
    std::int64_t deal_id{0};
    std::int64_t quantity{0};
    std::string price;
    bool matched_by_public_order_id{false};
    bool matched_by_private_order_id{false};
    bool matched_by_ext_id{false};
};

struct OrderObservation {
    std::int64_t public_order_id{0};
    std::int64_t private_order_id{0};
    std::vector<std::int64_t> public_order_id_aliases;
    std::vector<std::int64_t> private_order_id_aliases;
    std::int32_t ext_id{0};
    std::string client_code;
    std::int64_t original_quantity{0};
    std::int64_t remaining_quantity{0};
    std::int64_t executed_quantity{0};
    OrderLifecycleState state{OrderLifecycleState::Posted};
    bool from_trade_replication{false};
    bool from_user_orderbook{false};
    bool from_current_day_snapshot{false};
    bool from_own_trades{false};
    bool identity_conflict{false};
    std::uint64_t trade_repl_commit_sequence{0};
    std::uint64_t user_orderbook_commit_sequence{0};
    std::vector<MatchedOwnTrade> matched_own_trades;
};

[[nodiscard]] std::optional<OrderObservation>
observe_order(std::int32_t ext_id, std::string_view client_code, Plaza2TradeSide side,
              std::int64_t expected_original_quantity, std::span<const plaza2::private_state::OwnOrderSnapshot> orders,
              std::span<const plaza2::private_state::OwnTradeSnapshot> trades);

struct OrderReplyObservation {
    std::uint32_t user_id{0};
    Plaza2TradeCommandKind command_kind{Plaza2TradeCommandKind::AddOrder};
    bool timed_out{false};
    bool accepted{false};
    std::int32_t code{0};
    std::optional<std::int64_t> order_id;
};

struct OrderLifecyclePollResult {
    bool ok{true};
    bool deadline_reached{false};
    std::string error;
    std::vector<OrderReplyObservation> replies;
    std::vector<OrderObservation> observations;
};

struct PreSendPlan;

class OrderLifecycleTransport {
  public:
    virtual ~OrderLifecycleTransport() = default;
    // The controller binds the exact, already-authorized static plan before
    // opening a journal or allowing any transport-side AddOrder path.  A
    // transport-neutral scripted implementation may accept this binding as a
    // no-op; concrete transports must retain and enforce the bound identity.
    [[nodiscard]] virtual plaza2::cgate::Plaza2Error bind_authorized_plan(const PreSendPlan& plan) = 0;
    [[nodiscard]] virtual plaza2::cgate::Plaza2PublisherMessageResult post(const Plaza2TradeEncodedCommand& command,
                                                                           std::uint32_t user_id) = 0;
    [[nodiscard]] virtual plaza2::cgate::Plaza2PublisherMessageResult
    post_exact_ext_id_recovery(const Plaza2TradeEncodedCommand& command, std::uint32_t user_id) = 0;
    [[nodiscard]] virtual OrderLifecyclePollResult poll(std::chrono::steady_clock::time_point deadline) = 0;
    [[nodiscard]] virtual OrderLifecyclePollResult reconcile() = 0;
};

class OrderLifecycleClock {
  public:
    virtual ~OrderLifecycleClock() = default;
    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
};

class SystemOrderLifecycleClock final : public OrderLifecycleClock {
  public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override;
};

struct OrderSmokeSnapshot {
    bool instrument_exists{false};
    bool tradable_session{false};
    bool aggr20_two_sided{false};
    bool limits_snapshot_applicable{false};
    std::string tick_size;
    std::string top_bid;
    std::string top_ask;
    std::string market_data_source;
    std::uint64_t aggr20_source_sequence{0};
    std::int64_t aggr20_source_revision{0};
    std::string aggr20_observed_at_utc;
    std::uint64_t aggr20_age_ms{0};
    std::string trading_day;
    std::string session_id;
    std::string session_state;
    std::string refdata_source;
    std::uint64_t refdata_source_sequence{0};
    std::int64_t refdata_source_revision{0};
    std::string limits_source;
    std::uint64_t limits_commit_sequence{0};
};

struct OrderSmokePolicy {
    std::string version;
    std::string sha256;
    std::uint32_t max_distance_ticks{0};
    std::uint64_t max_aggr20_age_ms{0};
    bool require_zero_starting_position{false};
};

struct OrderLifecycleConfig {
    std::string run_id;
    std::string profile_id;
    std::string profile_fingerprint;
    bool profile_enabled{false};
    plaza2::cgate::Plaza2Environment environment{plaza2::cgate::Plaza2Environment::Test};

    bool dry_run{true};
    bool send_test_order{false};
    bool any_arm_flag{false};
    std::string authorized_plan_sha256;

    std::int32_t isin_id{0};
    std::string base_contract_code;
    std::int8_t instrument_mask{0};
    std::string broker_code;
    std::string client_code;
    Plaza2TradeSide side{Plaza2TradeSide::Buy};
    Plaza2TradeOrderType order_type{Plaza2TradeOrderType::Limit};
    std::string price;
    std::int32_t quantity{0};
    std::int32_t ext_id{0};
    std::uint32_t add_user_id{0};
    std::uint32_t cancel_user_id{0};
    std::uint32_t recovery_user_id{0};
    std::string comment;

    OrderSmokeSnapshot smoke;
    OrderSmokePolicy policy;
    std::chrono::milliseconds add_observation_timeout{std::chrono::seconds(60)};
    std::chrono::milliseconds cancel_observation_timeout{std::chrono::seconds(60)};
    std::uint32_t max_poll_attempts{1024};
    std::filesystem::path journal_root;
};

enum class PreSendFailure : std::uint8_t {
    None = 0,
    ConflictingMode,
    DryRunArmed,
    DisabledProfile,
    NonTestProfile,
    InvalidQuantity,
    InvalidOrderType,
    InstrumentMissing,
    SessionNotTradable,
    InvalidPrice,
    PriceNotTickAligned,
    Aggr20NotFreshTwoSided,
    MarketablePrice,
    DistanceCeilingExceeded,
    LimitsSnapshotMissing,
    InvalidIdentifier,
    DuplicateIdentifier,
    PlanHashMismatch,
    CommandValidationFailed,
    JournalFailure,
};

[[nodiscard]] std::string_view pre_send_failure_name(PreSendFailure failure) noexcept;

struct PreSendPlan {
    bool ok{false};
    PreSendFailure failure{PreSendFailure::None};
    std::string message;
    // The canonical hash is an authorization hash for static order intent. It
    // deliberately excludes dynamic market/session observations; those belong
    // in the execution-safety receipt produced immediately before posting.
    std::string canonical_json;
    std::string reviewed_evidence_json;
    std::string sha256;
    Plaza2TradeEncodedCommand add_command;
    Plaza2TradeEncodedCommand exact_ext_id_recovery_command;
};

[[nodiscard]] PreSendPlan build_pre_send_plan(const OrderLifecycleConfig& config);
[[nodiscard]] bool write_pre_send_plan(const std::filesystem::path& output_directory, const PreSendPlan& plan,
                                       std::string& error);

struct RestartReconciliationResult {
    bool ok{true};
    bool run_found{false};
    bool resolved{false};
    bool locks_retained{true};
    OrderLifecycleState state{OrderLifecycleState::UnresolvedOrphanIncident};
    std::filesystem::path journal_path;
    std::string message;
};

// Read-only startup reconciliation over a previously unfinished local run.
// It never submits a command. Locks are removed only after a fresh, matching
// terminal observation and a published resolution record.
[[nodiscard]] RestartReconciliationResult
reconcile_unfinished_run(const OrderLifecycleConfig& config,
                         std::span<const plaza2::private_state::OwnOrderSnapshot> orders,
                         std::span<const plaza2::private_state::OwnTradeSnapshot> trades);

struct OrderLifecycleResult {
    bool ok{false};
    bool market_safe_terminal{false};
    bool journal_ok{true};
    bool journal_degraded{false};
    bool evidence_consistent{true};
    bool orphan_incident_written{false};
    OrderLifecycleState state{OrderLifecycleState::DefinitelyNotSent};
    std::string message;
    std::optional<OrderObservation> observation;
    plaza2::cgate::Plaza2PublisherMessageResult add_submission;
    plaza2::cgate::Plaza2PublisherMessageResult cancel_submission;
    plaza2::cgate::Plaza2PublisherMessageResult recovery_submission;
    std::optional<OrderReplyObservation> add_reply;
    std::optional<OrderReplyObservation> cancel_reply;
    std::optional<OrderReplyObservation> recovery_reply;
    std::vector<OrderLifecycleState> transitions;
    std::filesystem::path journal_path;
};

class OrderLifecycleController {
  public:
    OrderLifecycleController(OrderLifecycleConfig config, OrderLifecycleTransport& transport,
                             OrderLifecycleClock& clock);
    [[nodiscard]] OrderLifecycleResult run();

  private:
    OrderLifecycleConfig config_;
    OrderLifecycleTransport& transport_;
    OrderLifecycleClock& clock_;
};

// A persistent, serial-order application controller.  It reuses the same
// OrderLifecycleTransport and therefore keeps the underlying PLAZA session
// host warm while one order epoch is opened, submitted, polled, cancelled,
// and closed before the next epoch begins.
class PersistentOrderController final {
  public:
    PersistentOrderController(OrderLifecycleConfig config, OrderLifecycleTransport& transport,
                              OrderLifecycleClock& clock);
    ~PersistentOrderController();
    PersistentOrderController(const PersistentOrderController&) = delete;
    PersistentOrderController& operator=(const PersistentOrderController&) = delete;
    PersistentOrderController(PersistentOrderController&&) noexcept;
    PersistentOrderController& operator=(PersistentOrderController&&) noexcept;

    // The caller must already have installed/bound the exact authorized plan
    // in the transport.  This transition opens the application order epoch;
    // it does not allocate or post a publisher message.
    [[nodiscard]] plaza2::cgate::Plaza2Error begin(const PreSendPlan& plan);
    [[nodiscard]] OrderLifecycleResult submit_order();
    [[nodiscard]] OrderLifecycleResult poll_order();
    [[nodiscard]] OrderLifecycleResult cancel_order();
    [[nodiscard]] plaza2::cgate::Plaza2Error finish_order_epoch();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool authorized() const noexcept;
    [[nodiscard]] bool submission_attempted() const noexcept;
    [[nodiscard]] bool new_order_allowed() const noexcept;
    [[nodiscard]] OrderLifecycleState state() const noexcept;
    [[nodiscard]] const OrderLifecycleResult& last_result() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::plaza2_trade
