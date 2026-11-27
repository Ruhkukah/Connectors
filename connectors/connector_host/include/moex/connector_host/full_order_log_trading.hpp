#pragma once

#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2_trade/cgate_session.hpp"

namespace moex::connector_host {
// The book remains externally owned on the trading host's owner thread.
inline plaza2_trade::FullOrderLogTradingAuthority
make_full_order_log_trading_authority(const plaza2::cgate::Plaza2FullOrderLog& book, std::int32_t isin) {
    const auto& bids = book.levels(isin, true);
    const auto& asks = book.levels(isin, false);
    return {.session_id = book.committed_session_id(isin),
            .epoch = book.epoch(),
            .revision = book.revision(isin),
            .exchange_moment_ns = book.exchange_moment_ns(isin),
            .best_bid = bids.empty() ? 0 : bids.rbegin()->first,
            .best_ask = asks.empty() ? 0 : asks.begin()->first,
            .best_bid_quantity = bids.empty() ? 0 : bids.rbegin()->second,
            .best_ask_quantity = asks.empty() ? 0 : asks.begin()->second,
            .committed_at = book.committed_at(),
            .valid = book.valid(),
            .crossed = book.crossed(isin)};
}
} // namespace moex::connector_host
