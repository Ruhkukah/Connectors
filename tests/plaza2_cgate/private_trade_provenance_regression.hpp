#pragma once

#include "private_state_deletion_regression.hpp"

namespace moex::plaza2::test {
inline void private_trade_provenance_regression() {
    using namespace deletion;
    using enum pr::EventKind;
    using enum gen::StreamCode;
    Harness h;
    const auto check = [](const ps::OwnTradeSnapshot& trade, std::int64_t rev, std::uint64_t life) {
        require(trade.repl_rev == rev && trade.trade_lifenum == life,
                "committed own trade lost its user_deal revision/TRADE LifeNum provenance");
    };
    h.begin(kFortsTradeReplUserDeal, true);
    h.row(kFortsTradeReplUserDeal, 10, 11);
    h.commit();
    check(h.projector.own_trades().front(), 11, 0);
    check(h.projector.take_row_changes().trades.front(), 11, 0);
    h.projector.on_event({}, {.kind = kLifeNum, .stream_code = kFortsTradeRepl, .numeric_value = 7}, h.state);
    h.begin(kFortsTradeReplUserDeal, true);
    h.row(kFortsTradeReplUserDeal, 10, 12);
    h.commit();
    check(h.projector.own_trades().front(), 12, 7);
    check(h.projector.take_row_changes().trades.front(), 12, 7);
    h.begin(kFortsTradeReplUserDeal); // Synthetic transaction retains rollback visibility.
    h.row(kFortsTradeReplUserDeal, 10, 99);
    check(h.projector.own_trades().front(), 12, 7);
    require(h.projector.clone().take_row_changes().trades.empty(),
            "uncommitted trade provenance was published as a delta");
    h.begin(kFortsTradeReplUserDeal); // Abort the staged revision99.
    h.commit();
    check(h.projector.own_trades().front(), 12, 7);
    auto copy = h.projector.clone();
    h.projector.on_event({}, {.kind = kClose, .stream_code = kFortsTradeRepl}, h.state);
    check(h.projector.own_trades().front(), 12, 7);
    h.projector.on_event({}, {.kind = kLifeNum, .stream_code = kFortsTradeRepl, .numeric_value = 8}, h.state);
    require(h.projector.own_trades().empty(), "changed TRADE LifeNum retained previous-life user_deal rows");
    h.begin(kFortsTradeReplUserDeal, true);
    h.row(kFortsTradeReplUserDeal, 10, 1);
    h.commit();
    check(h.projector.own_trades().front(), 1, 8);
    check(copy.own_trades().front(), 12, 7);
    const auto current = h.projector.take_row_changes();
    require(current.resync_required || (current.trades.size() == 1 && current.trades.front().repl_rev == 1 &&
                                        current.trades.front().trade_lifenum == 8),
            "new-life trade delta carried stale revision provenance");
    (void)h.projector.take_row_changes();
    h.begin(kFortsTradeReplUserDeal, true);
    h.row(kFortsTradeReplUserDeal, 10, 1); // Replaying the same row must not invent new revision proof.
    h.commit();
    check(h.projector.take_row_changes().trades.front(), 1, 8);
}
} // namespace moex::plaza2::test
