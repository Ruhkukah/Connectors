#pragma once

#include "moex/connector_host/connector_host.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace moex::connector_host::dtc {

// DTC v8 uses a little-endian uint16 size (including this four-byte header)
// followed by a little-endian uint16 message type.  The protobuf payload is
// deliberately kept behind this framing boundary so transport tests can run
// without a live CGate runtime or a generated protobuf compiler.
constexpr std::size_t kDtcFrameHeaderSize = 4;
constexpr std::size_t kDtcMaxFrameSize = UINT16_MAX;

enum class DtcMessageType : std::uint16_t {
    EncodingRequest = 6,
    EncodingResponse = 7,
    Heartbeat = 3,
    LogonRequest = 1,
    LogonResponse = 2,
    MarketDataFeedStatus = 100,
    MarketDataFeedSymbolStatus = 116,
    MarketDataRequest = 101,
    MarketDataReject = 103,
    MarketDepthRequest = 102,
    MarketDepthReject = 121,
    MarketDepthSnapshotLevelFloat = 145,
    MarketDepthUpdateLevelFloatWithMilliseconds = 140,
    MarketDataUpdateTrade = 107,
    MarketDataUpdateBidAsk = 108,
    SecurityDefinitionForSymbolRequest = 506,
    SecurityDefinitionResponse = 507,
    SecurityDefinitionReject = 509,
    Logoff = 5,
};

struct DtcFrame {
    std::uint16_t message_type{0};
    std::vector<std::uint8_t> payload;
};

// Incremental, allocation-bounded DTC frame decoder.  A single append may
// contain a fragment, one frame, or multiple coalesced frames.  Invalid
// lengths fence the decoder and require an explicit reset by the session
// owner; callers must not silently resynchronize in the middle of a stream.
class DtcFrameDecoder final {
  public:
    explicit DtcFrameDecoder(std::size_t max_frame_size = kDtcMaxFrameSize);

    [[nodiscard]] bool append(std::span<const std::uint8_t> bytes,
                              std::vector<DtcFrame>& completed,
                              std::string& error);
    void reset() noexcept;
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffer_.size(); }

  private:
    std::size_t max_frame_size_;
    std::vector<std::uint8_t> buffer_;
};

enum class DtcDepthSide : std::int32_t {
    Bid = 1,
    Ask = 2,
};

// The connector-side market-data contract is source-shaped, not a UI model.
// It carries only the target AGGR20 state that is already owned by
// ConnectorHost.  AGGR20 is depth, not a public trade tape; no trade events
// are inferred here.
struct DtcMarketDataLevel {
    std::int64_t price_scaled{0};
    std::int64_t volume{0};
    DtcDepthSide side{DtcDepthSide::Bid};
    std::uint64_t source_repl_id{0};
    std::int64_t source_repl_rev{0};
    std::uint64_t exchange_moment_ns{0};
    std::string price;
};

struct DtcMarketDataSnapshot {
    std::uint64_t connector_generation{0};
    std::uint64_t source_repl_id{0};
    std::int64_t source_repl_rev{0};
    std::uint64_t exchange_moment_ns{0};
    std::uint64_t sampled_at_unix_ns{0};
    std::int64_t isin_id{0};
    std::string symbol;
    std::string board;
    std::string min_step;
    bool source_online{false};
    bool snapshot_complete{false};
    bool two_sided{false};
    bool valid{false};
    std::string invalid_reason;
    std::vector<DtcMarketDataLevel> levels;
};

struct DtcReadOnlyCapabilities {
    bool market_data{false};
    bool market_depth{false};
    bool security_definitions{false};
    // These remain false until account/order state has a separately tested
    // reconciliation and authorization contract.
    bool accounts{false};
    bool positions{false};
    bool orders{false};
    bool order_entry{false};
};

class DtcMarketDataSource {
  public:
    virtual ~DtcMarketDataSource() = default;
    [[nodiscard]] virtual DtcMarketDataSnapshot snapshot() const = 0;
    [[nodiscard]] virtual DtcReadOnlyCapabilities capabilities() const noexcept = 0;
};

// Adapter from the single-threaded ConnectorHost owner to the provider-
// neutral DTC market-data boundary.  The caller owns the host and must invoke
// this source only from the same owner/control plane that polls the host.
class ConnectorHostDtcMarketDataSource final : public DtcMarketDataSource {
  public:
    ConnectorHostDtcMarketDataSource(ConnectorHost& host, std::string board);

    [[nodiscard]] DtcMarketDataSnapshot snapshot() const override;
    [[nodiscard]] DtcReadOnlyCapabilities capabilities() const noexcept override;

  private:
    ConnectorHost& host_;
    std::string board_;
};

} // namespace moex::connector_host::dtc
