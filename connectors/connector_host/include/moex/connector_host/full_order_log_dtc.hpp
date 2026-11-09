#pragma once
#include "moex/connector_host/dtc_market_data.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include <array>
#include <functional>

namespace moex::connector_host::dtc {
// One instrument view of the committed anonymous book; no trading capability.
class DtcFullOrderLogSource final : public DtcMarketDataSource {
  public:
    DtcFullOrderLogSource(const plaza2::cgate::Plaza2FullOrderLog& book, std::int32_t isin,
                          DtcMarketDataSnapshot metadata = {}, std::size_t max_changes = 40000);
    using PublicDealsReader = std::function<plaza2::cgate::Plaza2PublicDealsSnapshot(std::uint64_t, std::int32_t)>;
    void set_public_deals_reader(PublicDealsReader reader) {
        public_deals_reader_ = std::move(reader);
    }
    [[nodiscard]] plaza2::cgate::Plaza2PublicDealsSnapshot
    public_deals(std::uint64_t after_sequence = 0) const override;
    void update_metadata(DtcMarketDataSnapshot metadata);
    void configure_depth_limit(std::size_t depth) override;
    void committed(); // before server.publish_depth_commit(), on the CGate owner
    [[nodiscard]] DtcMarketDataSnapshot snapshot() const override;
    [[nodiscard]] DtcMarketDataSnapshot status_snapshot() const override;
    [[nodiscard]] DtcReadOnlyCapabilities capabilities() const noexcept override {
        return {.market_data = bool(public_deals_reader_), .market_depth = true, .security_definitions = true};
    }
    [[nodiscard]] bool incremental_depth() const noexcept override {
        return true;
    }
    [[nodiscard]] std::span<const DtcMarketDataLevel> depth_changes() const noexcept override {
        return changes_;
    }

  private:
    const plaza2::cgate::Plaza2FullOrderLog& book_;
    std::int32_t isin_;
    DtcMarketDataSnapshot metadata_;
    PublicDealsReader public_deals_reader_;
    std::vector<DtcMarketDataLevel> changes_;
    using Level = std::pair<std::int64_t, std::int64_t>;
    std::array<std::vector<Level>, 2> published_, scratch_;
    std::size_t depth_{20};
    std::pair<std::uint64_t, std::uint64_t> published_key_{};
    bool configured_{false}, wire_prices_valid_{true};
    void load_top(bool bid, std::vector<Level>& into) const;
};
} // namespace moex::connector_host::dtc
