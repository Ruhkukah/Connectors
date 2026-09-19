#pragma once

#include "moex/plaza2_trade/plaza2_test_trade_transport.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace moex::connector_host {

enum class ConnectorHostState { Created, Started, Ready, Stopping, Stopped, Failed, Recovering };
enum class HostPurpose { Qualify, OrderTest };

// Explicit native configuration, not the older topology-only profile format.
// Session/target IDs are operator inputs; the host checks authoritative membership.
struct Plaza2HostConfig {
    HostPurpose purpose{HostPurpose::Qualify};
    plaza2_trade::Plaza2TestTradeTransportConfig transport;
    plaza2_trade::OrderLifecycleConfig order;
    // Optional explicit binding for the raw ASTS SECBOARD value carried by
    // FORTS_REFDATA_REPL.fut_vcb.board_md. This is underlying-board metadata,
    // not the DTC Exchange/venue identifier.
    std::string target_underlying_board;
    // Currency is not inferred from an endpoint, symbol, or tick size. This
    // optional operator binding is never claimed as REFDATA-proven by itself.
    std::string target_currency;
    // A read-only market-data host never creates publisher/reply handles and
    // rejects all order authorization APIs. It remains TEST-only. This flag
    // must equal transport.host.read_only_market_data in direct configs.
    bool read_only_market_data{false};
    // Wall-clock seam for current-session display checks; production defaults
    // to system_clock::now. Does not participate in order authorization.
    std::function<std::chrono::system_clock::time_point()> market_data_now;
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
    plaza2_trade::Plaza2RecoveryStatus recovery;
    plaza2::cgate::Plaza2Environment environment{plaza2::cgate::Plaza2Environment::Test};
    plaza2_trade::Plaza2TestSessionHostMode mode{plaza2_trade::Plaza2TestSessionHostMode::LiveTestPreSend};
    std::string runtime_compatibility;
    std::string runtime_scheme_sha256;
    std::string connection_app_name;
    bool publisher_handle_open{false};
    bool reply_handle_open{false};
    bool private_snapshot_state_ready{false};
    bool aggr_snapshot_state_ready{false};
    bool aggr_transport_active{false};
    bool aggr_session_data_ready{false};
    bool aggr_target_authoritative{false};
    std::uint64_t aggr_stream_epoch{0};
    std::uint64_t aggr_source_snapshot_version{0};
    std::uint64_t aggr_source_snapshot_hash{0};
    bool aggr_ready{false};
    plaza2_trade::Plaza2TransportHealth transport_health;
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
    bool participant_limit_row_present{false};
    bool participant_identity_exact{false};
    bool exchange_money_limit_check_enabled{false};
    bool limits_set{false};
    bool order_epoch_active{false};
    bool order_authorized{false};
    bool order_submission_attempted{false};
    bool session_terms_present{false};
    bool session_terms_current{false};
    bool session_price_bounds_valid{false};
    bool order_price_within_exchange_bounds{false};
    bool order_price_tick_aligned{false};
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
    plaza2::cgate::Plaza2Error causal_error;
    std::uint64_t causal_error_time_ns{};
    std::string causal_operation, causal_callback_error;
    plaza2_trade::Plaza2TransportHealth causal_health;
};

// Explicitly sampled qualification data; never copied by the normal polling path.
struct ConnectorHostQualificationSnapshot {
    plaza2::cgate::Plaza2Aggr20Snapshot book;
    std::vector<plaza2::private_state::InstrumentSnapshot> instruments;
    std::vector<plaza2::private_state::PositionSnapshot> positions;
    std::vector<plaza2::private_state::OwnOrderSnapshot> active_orders;
    std::vector<plaza2::private_state::SystemMessageSnapshot> system_messages;
    std::string connection_app_name;
    plaza2::cgate::Plaza2PublisherRateMetrics rate;
    std::size_t visible_limit_rows{}, matching_client_limit_rows{}, matching_broker_limit_rows{}, unknown_limit_rows{};
    bool client_code_is_brokerage_account{false};
    std::optional<plaza2::private_state::LimitSnapshot> broker_limit, client_limit;
    struct LimitDiagnostic {
        std::int64_t repl_id;
        plaza2::private_state::LimitParticipantKind kind;
        std::size_t code_length;
        bool equals_broker, equals_client, limits_set, auto_update;
        std::string money_free, money_blocked, money_amount;
        std::string private_account_code; // Explicit opt-in qualification artifact only.
    };
    std::vector<LimitDiagnostic> limit_diagnostics;
    bool aggr_online{false};
    bool aggr_snapshot_complete{false};
};

struct ConnectorHostMarketDataLevel {
    std::int64_t price_scaled{0};
    std::int64_t volume{0};
    std::int32_t side{0};
    std::uint64_t source_repl_id{0};
    std::int64_t source_repl_rev{0};
    std::uint64_t exchange_moment{0};
    std::uint64_t exchange_moment_ns{0};
    std::string price;

    friend bool operator==(const ConnectorHostMarketDataLevel&, const ConnectorHostMarketDataLevel&) = default;
};

// Narrow owner-thread market-data view used by the DTC boundary. It never
// copies private account/order state and never reads the global AGGR
// qualification book for a target decision.
struct ConnectorHostMarketDataSnapshot {
    std::uint64_t connector_generation{0};
    std::uint64_t market_data_authority_epoch{0};
    std::uint64_t stream_epoch{0};
    std::int64_t target_isin_id{0};
    std::int32_t target_session_id{0};
    std::string symbol;
    // Raw FORTS_REFDATA_REPL.fut_vcb.board_md (ASTS SECBOARD identifier).
    // It is distinct from the gateway-defined DTC Exchange identifier.
    std::string underlying_board;
    std::string min_step;
    std::string description;
    // When the committed fut_vcb join is resolved, these are its raw source
    // values. Otherwise they retain explicit operator bindings only.
    std::string currency;
    // These values are raw authoritative REFDATA text. Empty means that the
    // current source cannot prove the corresponding DTC 507 value.
    std::string contract_size;
    std::string currency_value_per_increment;
    bool refdata_vcb_join_current{false};
    bool refdata_vcb_join_ambiguous{false};
    bool refdata_board_proven{false};
    // This is deliberately narrower than raw fut_vcb.curr presence: it is
    // true only for a supported monetary/tick denomination path (Phase5
    // currently exact RUB), not for an arbitrary quotation code.
    bool refdata_currency_proven{false};
    bool target_is_future{false};
    bool target_is_spread{false};
    bool target_is_multileg{false};
    std::string future_vcb_base_contract_code;
    std::int32_t future_vcb_base_contract_id{0};
    plaza2::private_state::SourceRowProvenance definition_source_provenance;
    plaza2::private_state::SourceRowProvenance future_instruments_provenance;
    plaza2::private_state::SourceRowProvenance future_sess_contents_provenance;
    plaza2::private_state::SourceRowProvenance session_provenance;
    plaza2::private_state::SourceRowProvenance future_vcb_provenance;
    std::string invalid_reason;
    std::uint64_t source_snapshot_version{0};
    std::uint64_t source_snapshot_hash{0};
    std::uint64_t source_repl_id{0};
    std::int64_t source_repl_rev{0};
    // AGGR20 target's committed maximum replRev, explicitly used as the
    // target-source watermark. It is not a DTC batch sequence.
    std::uint64_t snapshot_watermark{0};
    std::uint64_t exchange_moment{0};
    std::uint64_t exchange_moment_ns{0};
    std::chrono::steady_clock::time_point committed_at{};
    bool transport_active{false};
    bool snapshot_complete{false};
    bool session_data_ready{false};
    bool target_authoritative{false};
    bool aggr_online{false};
    bool book_snapshot_current{false};
    std::optional<plaza2::cgate::Plaza2Aggr20SysEventSnapshot> session_ready_witness;
    plaza2::cgate::SessionReadyWitnessKind session_ready_witness_kind{plaza2::cgate::SessionReadyWitnessKind::None};
    bool market_data_display_allowed{false};
    // Data consistency and trading state are intentionally separate. A
    // closed/clearing session may retain a valid frozen synchronized book.
    bool source_consistent{false};
    bool market_data_live{false};
    bool session_tradable{false};
    bool instrument_tradable{false};
    bool order_entry_allowed{false};
    bool refdata_metadata_current{false};
    bool valid{false};
    bool two_sided{false};
    std::vector<ConnectorHostMarketDataLevel> levels;
};

[[nodiscard]] std::string_view host_state_name(ConnectorHostState state) noexcept;
[[nodiscard]] std::string render_snapshot(const ConnectorHostSnapshot& snapshot, bool json,
                                          const ConnectorHostQualificationSnapshot* qualification = nullptr);

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
    [[nodiscard]] bool has_publisher_or_reply_handles() const noexcept;
    [[nodiscard]] ConnectorHostMarketDataSnapshot market_data_snapshot() const;
    [[nodiscard]] plaza2_trade::DeepPassiveProposal first_order_price_proposal() const;
    [[nodiscard]] ConnectorHostQualificationSnapshot qualification_snapshot(bool private_identity = false) const;
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
    [[nodiscard]] plaza2_trade::RecoveredOrderReconciliation reconcile_recovered_order();
    [[nodiscard]] plaza2_trade::RecoveredCancelPlan prepare_recovered_cancel(const std::filesystem::path& artifact);
    [[nodiscard]] plaza2_trade::OrderLifecycleResult cancel_recovered_order(const std::filesystem::path& artifact,
                                                                            std::string_view authorized_sha256);

    [[nodiscard]] plaza2::cgate::Plaza2Error finish_order_epoch();
    [[nodiscard]] plaza2_trade::RestartReconciliationResult reconcile();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::connector_host
