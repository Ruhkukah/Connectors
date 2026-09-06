#pragma once

#include "moex/plaza2_trade/plaza2_test_trade_transport.hpp"

#include <cstdint>
#include <string>

namespace moex::connector_host {

enum class ConnectorHostState { Created, Started, Ready, Stopping, Stopped, Failed };
enum class HostPurpose { Qualify, OrderTest };

// Explicit native configuration, not the older topology-only profile format.
// Session/target IDs are operator inputs; the host checks authoritative membership.
struct Plaza2HostConfig {
    HostPurpose purpose{HostPurpose::Qualify};
    plaza2_trade::Plaza2TestTradeTransportConfig transport;
    plaza2_trade::OrderLifecycleConfig order;
};

// Application-selected terms for one serial TEST epoch.  Collision-prone
// identifiers and the run identity remain host-managed.
struct ConnectorHostOrderRequest {
    plaza2_trade::Plaza2TradeSide side{plaza2_trade::Plaza2TradeSide::Buy};
    std::string price;
    std::string base_contract_code;
    std::string comment;
    std::int32_t quantity{1};
};

struct ConnectorHostSnapshot {
    ConnectorHostState state{ConnectorHostState::Created};
    plaza2::cgate::Plaza2Environment environment{plaza2::cgate::Plaza2Environment::Test};
    plaza2_trade::Plaza2TestSessionHostMode mode{plaza2_trade::Plaza2TestSessionHostMode::LiveTestPreSend};
    std::string runtime_compatibility;
    std::string runtime_scheme_sha256;
    bool publisher_ready{false};
    bool reply_ready{false};
    bool private_streams_ready{false};
    bool observation_ready{false};
    std::vector<plaza2::private_state::StreamHealthSnapshot> streams;
    std::uint64_t refdata_lifenum{0};
    std::int64_t target_isin_id{0};
    std::int32_t session_id{0};
    std::string target;
    std::string min_step;
    std::optional<std::int32_t> session_status;
    std::optional<std::int32_t> instrument_status;
    std::string bid;
    std::string ask;
    std::int64_t bbo_age_ms{-1};
    bool target_refdata_provenance_ready{false};
    bool target_aggr20_uncrossed{false};
    plaza2::private_state::SourceRowProvenance fut_instruments_provenance;
    plaza2::private_state::SourceRowProvenance fut_sess_contents_provenance;
    plaza2::private_state::SourceRowProvenance session_provenance;
    plaza2_trade::PositionEvidenceClass position_evidence_class{plaza2_trade::PositionEvidenceClass::Unresolved};
    bool zero_starting_position_proven{false};
    std::int64_t pos_trades_rev{0};
    std::int64_t pos_trades_lifenum{0};
    std::optional<plaza2_trade::Plaza2TradeReplayAnchor> trade_anchor;
    bool trade_replay_complete{false};
    std::size_t active_own_order_count{0};
    bool uob_periodic_consistent{false};
    bool limits_set{false};
    bool order_epoch_active{false};
    bool order_authorized{false};
    bool order_submission_attempted{false};
    bool new_order_allowed{false};
    std::optional<plaza2_trade::OrderLifecycleState> lifecycle_state;
    std::optional<plaza2_trade::OrderReplyObservation> add_reply;
    std::optional<plaza2_trade::OrderReplyObservation> cancel_reply;
    std::int64_t order_id{0};
    std::int64_t original_quantity{0};
    std::int64_t remaining_quantity{0};
    std::int64_t executed_quantity{0};
    // No terminal claim before a lifecycle completes.
    bool market_safe{false};
    bool evidence_consistent{true};
    plaza2::cgate::Plaza2PublisherCallCounts publisher_calls;
    std::string last_error;
};

[[nodiscard]] std::string_view host_state_name(ConnectorHostState state) noexcept;
[[nodiscard]] std::string render_snapshot(const ConnectorHostSnapshot& snapshot, bool json);

// Single-threaded owner. Callers receive values, never transport/projector pointers.
class ConnectorHost final {
  public:
    explicit ConnectorHost(Plaza2HostConfig config);
    ~ConnectorHost();
    ConnectorHost(const ConnectorHost&) = delete;
    ConnectorHost& operator=(const ConnectorHost&) = delete;
    [[nodiscard]] plaza2::cgate::Plaza2Error start();
    [[nodiscard]] plaza2::cgate::Plaza2Error poll();
    [[nodiscard]] plaza2::cgate::Plaza2Error stop();
    [[nodiscard]] ConnectorHostSnapshot snapshot() const;
    [[nodiscard]] plaza2_trade::PreSendPlan plan() const;
    [[nodiscard]] plaza2_trade::PreSendPlan plan_order(const ConnectorHostOrderRequest& request) const;
    // Exact canonical bytes AND SHA are mandatory. The host constructs the
    // intent and lets the existing transport validate/bind it.
    [[nodiscard]] plaza2::cgate::Plaza2Error authorize(std::string_view canonical_plan, std::string_view sha256);
    [[nodiscard]] plaza2_trade::OrderLifecycleResult submit();
    // Persistent application-order surface. The host remains started across
    // serial order epochs; only the per-order lifecycle state is closed and
    // reset after a safe terminal disposition.
    [[nodiscard]] plaza2::cgate::Plaza2Error begin_order(const ConnectorHostOrderRequest& request,
                                                         std::string_view canonical_plan, std::string_view sha256);
    // Compatibility convenience for callers that intentionally reuse the
    // current configured application terms; new application code should use
    // the explicit request overload above.
    [[nodiscard]] plaza2::cgate::Plaza2Error begin_order(std::string_view canonical_plan, std::string_view sha256);
    [[nodiscard]] plaza2_trade::OrderLifecycleResult submit_order();
    [[nodiscard]] plaza2_trade::OrderLifecycleResult poll_order();
    [[nodiscard]] plaza2_trade::OrderLifecycleResult cancel_current_order();
    [[nodiscard]] plaza2::cgate::Plaza2Error finish_order_epoch();
    [[nodiscard]] plaza2_trade::RestartReconciliationResult reconcile();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::connector_host
