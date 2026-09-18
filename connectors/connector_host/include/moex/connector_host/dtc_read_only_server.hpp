#pragma once

#include "moex/connector_host/dtc_market_data.hpp"

#include <chrono>
#include <memory>

namespace moex::connector_host::dtc {

// Kairos authority extension: DTC USER_MESSAGE (700), protobuf string field 1
// starts with "moex.source_authority.v1 " followed by UTF-8 JSON; field 2 is
// IsPopupMessage=0. Sent after final 507/feed+symbol availability and before
// every complete snapshot. It carries the actual source flags and stream
// epoch; order_entry_allowed and exchange_confirmed are always false.
inline constexpr std::uint16_t kDtcSourceAuthorityMessage = 700;

struct DtcReadOnlyServerConfig {
    std::uint16_t port{0}; // zero asks the OS for an unused loopback port
    // Replay remains the compatibility default. A live ConnectorHost runner
    // must opt into LiveTest so replay diagnostics can never be presented as
    // live TEST evidence by accident.
    DtcSourceMode source_mode{DtcSourceMode::Replay};
    // Zero preserves the replay/client-selected symbol-ID behavior. A live
    // runner supplies a fixed local DTC symbol ID for its startup receipt.
    std::uint32_t symbol_id{0};
    std::size_t max_frame_bytes{4096};
    std::size_t max_queued_bytes{65536};
    std::size_t max_depth_levels{20}; // per side; hard ceiling 20 (AGGR20)
    std::chrono::milliseconds idle_timeout{30000};
    std::chrono::milliseconds write_timeout{5000};
    // Metadata absent from DtcMarketDataSource must be explicitly supplied by
    // the configured source-mode owner. Empty/invalid metadata prevents
    // security definitions. LiveTest does not use these replay compatibility
    // fields as fallbacks.
    std::string currency;
    std::string description;
    float contract_size{0};
    // DTC 507 float tag 8: currency value of one tick per contract. Supplied
    // explicitly; it must not be guessed from tick size or contract size.
    float currency_value_per_increment{0};
    // Optional local gateway authentication. These are DTC-client credentials
    // and must not be populated from the T1 credentials.
    bool require_local_auth{false};
    std::string local_username;
    std::string local_password;
};

// POSIX TCP, one bounded client, always binds 127.0.0.1. No worker threads:
// start(), poll(), stop(), and the source all belong to the same owner thread.
// poll() never blocks and calls snapshot() only on that owner. Source snapshot()
// itself must be bounded/nonblocking. It emits complete replacement snapshots,
// not invented deltas or trades. Call poll regularly (e.g. every 1-10ms).
// Unknown requests (including ALL trading/account operations) get LOGOFF with
// DoNotReconnect and are disconnected. No execution dependency is present.
class DtcReadOnlyServer final {
  public:
    explicit DtcReadOnlyServer(DtcMarketDataSource& source, DtcReadOnlyServerConfig config = {});
    ~DtcReadOnlyServer();
    DtcReadOnlyServer(const DtcReadOnlyServer&) = delete;
    DtcReadOnlyServer& operator=(const DtcReadOnlyServer&) = delete;

    bool start(std::string& error);
    void poll();
    void stop() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::uint32_t symbol_id() const noexcept;
    [[nodiscard]] bool has_client() const noexcept;
    [[nodiscard]] std::size_t queued_bytes() const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::connector_host::dtc
