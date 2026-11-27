#pragma once

#include "moex/plaza2/cgate/plaza2_public_deals.hpp"
#include <functional>
#include <memory>

namespace moex::plaza2::cgate {

// Consumes the same negotiated raw DEALS events as the FOL OI and capture path.
class FullOrderLogDeals {
  public:
    explicit FullOrderLogDeals(std::span<const std::int32_t> isins);
    ~FullOrderLogDeals();

    [[nodiscard]] Plaza2Error on_event(const Plaza2ListenerEvent& event);
    void on_error(const Plaza2Error& error) noexcept;
    [[nodiscard]] Plaza2PublicDealsSnapshot snapshot(std::int32_t isin, std::uint64_t after = 0,
                                                     std::int32_t expected_session = 0) const;

    // Invoked only after a complete live transaction is validated.
    std::function<void(const Plaza2PublicDeal&)> on_trade;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace moex::plaza2::cgate
