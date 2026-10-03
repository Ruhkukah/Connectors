#pragma once

#include "moex/plaza2_trade/cgate_session.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace moex::connector_host {

enum class ConnectorHostState { Created, Started, Ready, Stopping, Stopped, Failed, Recovering };
enum class HostPurpose { Qualify, Trade };
struct HostTransportConfig {
    plaza2_trade::CgateSessionConfig host;
    std::int64_t target_isin_id{};
    std::int32_t target_session_id{};
};
struct HostAccountConfig {
    std::string broker_code, client_code;
    std::int32_t isin_id{};
};

// Instruments are configured; the trading day follows committed exchange state.
struct Plaza2HostConfig {
    HostPurpose purpose{HostPurpose::Qualify};
    HostTransportConfig transport;
    HostAccountConfig order;
    std::vector<std::int64_t> isin_ids;
    // Optional explicit binding for the raw ASTS SECBOARD value carried by
    // FORTS_REFDATA_REPL.fut_vcb.board_md. This is underlying-board metadata,
    // not the DTC Exchange/venue identifier.
    std::string target_underlying_board;
    // Currency is not inferred from an endpoint, symbol, or tick size. This
    // optional operator binding is never claimed as REFDATA-proven by itself.
    std::string target_currency;
    // A read-only market-data host never creates publisher/reply handles and
    // exposes no order API. This flag must equal transport.host.read_only_market_data.
    bool read_only_market_data{false};
    // Wall-clock seam for current-session display checks; production defaults
    // to system_clock::now. Does not participate in order authorization.
    std::function<std::chrono::system_clock::time_point()> market_data_now;
};

struct ConnectorHostSnapshot {
    ConnectorHostState state{ConnectorHostState::Created};
    plaza2_trade::Plaza2RecoveryStatus recovery;
    plaza2::cgate::Plaza2Environment environment{plaza2::cgate::Plaza2Environment::Test};
    plaza2_trade::CgateSessionMode mode{plaza2_trade::CgateSessionMode::Live};
    std::string runtime_compatibility, runtime_scheme_sha256, connection_app_name;
    bool publisher_handle_open{}, reply_handle_open{};
    bool private_snapshot_state_ready{}, aggr_snapshot_state_ready{}, aggr_ready{};
    plaza2_trade::Plaza2TransportHealth transport_health;
    bool publisher_ready{}, reply_ready{}, private_streams_ready{}, observation_ready{};
    std::vector<plaza2::private_state::StreamHealthSnapshot> streams;
    std::int64_t target_isin_id{};
    std::int32_t session_id{};
    std::string target, min_step, bid, ask;
    std::optional<std::int32_t> session_status, instrument_status;
    bool new_order_allowed{};
    plaza2::cgate::Plaza2PublisherCallCounts publisher_calls;
    std::string last_error;
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
    // True only for the supported RUB monetary and tick denomination path.
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
    std::uint64_t source_snapshot_hash{0}; // Reserved legacy DTC wire field; always zero.
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
    std::optional<plaza2::cgate::Plaza2Aggr20SysEventSnapshot> session_ready_event;
    bool market_data_display_allowed{false};
    // Market-data consistency is independent of order-entry permissions.
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

[[nodiscard]] std::int32_t current_session_id(const plaza2::private_state::Plaza2PrivateStateProjector& data,
                                              std::int64_t now_seconds = 0);
[[nodiscard]] bool order_entry_ready(const plaza2_trade::CgateSession& host, std::int64_t isin_id,
                                     std::int32_t session_id = 0, bool allow_opening_auction = false);
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
    [[nodiscard]] bool has_publisher_or_reply_handles() const noexcept;
    [[nodiscard]] ConnectorHostMarketDataSnapshot market_data_snapshot() const;
    [[nodiscard]] bool public_deals_enabled() const noexcept;
    [[nodiscard]] plaza2::cgate::Plaza2PublicDealsSnapshot
    public_deals_snapshot(std::uint64_t after_sequence = 0) const;
    [[nodiscard]] ConnectorHostMarketDataSnapshot market_data_snapshot(std::int64_t isin_id) const;
    [[nodiscard]] bool order_entry_ready(std::int64_t isin_id) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::connector_host
