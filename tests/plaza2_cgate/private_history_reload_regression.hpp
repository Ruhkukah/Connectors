#pragma once

#include "private_state_deletion_regression.hpp"

#include <limits>

namespace moex::plaza2::test {
inline void private_history_reload_regression() {
    using namespace deletion;
    using enum pr::EventKind;
    using enum gen::StreamCode;
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    const auto clear = [](Harness& h, gen::TableCode table, std::int64_t revision,
                          gen::StreamCode code = kFortsTradeRepl) {
        h.projector.on_event(
            {}, {.kind = kClearDeleted, .stream_code = code, .table_code = table, .signed_value = revision}, h.state);
    };
    for (const auto table : {kFortsTradeReplOrdersLog, kFortsTradeReplUserDeal, pr::kNoTableCode}) {
        Harness h;
        h.begin(kFortsTradeReplOrdersLog, true);
        clear(h, table, max);
        require(!h.projector.clone().take_row_changes().regular_trade_history_reloaded,
                "uncommitted full TRADE clear became an absence reconciliation proof");
        h.projector.on_event({}, {.kind = kClose, .stream_code = kFortsTradeRepl}, h.state);
        h.begin(kFortsTradeReplOrdersLog, true); // Discard the aborted clear.
        h.commit();
        require(!h.projector.take_row_changes().regular_trade_history_reloaded,
                "aborted full clear followed by Close survived as a committed reload");
        h.begin(kFortsTradeReplOrdersLog, true);
        clear(h, table, max);
        h.commit();
        const auto copy = h.projector.clone().take_row_changes();
        require(copy.regular_trade_history_reloaded && copy.regular_trade_history_truncated,
                "committed full regular TRADE clear omitted its reload proof");
        h.projector.on_event({}, {.kind = kClose}, h.state);
        require(h.projector.take_row_changes().regular_trade_history_reloaded,
                "Close discarded a previously committed reload proof");
        require(!h.projector.take_row_changes().regular_trade_history_reloaded,
                "taking row changes did not acknowledge the reload proof");
    }
    Harness epoch;
    epoch.projector.on_event({}, {.kind = kLifeNum, .stream_code = kFortsTradeRepl, .numeric_value = 7}, epoch.state);
    epoch.projector.on_event({}, {.kind = kLifeNum, .stream_code = kFortsTradeRepl, .numeric_value = 7}, epoch.state);
    require(!epoch.projector.take_row_changes().regular_trade_history_reloaded,
            "first/repeated TRADE LifeNum invented a history reload");
    epoch.projector.on_event({}, {.kind = kLifeNum, .stream_code = kFortsTradeRepl, .numeric_value = 8}, epoch.state);
    require(epoch.projector.take_row_changes().regular_trade_history_reloaded,
            "changed TRADE LifeNum omitted its committed reload proof");
    Harness global;
    global.begin(kFortsTradeReplOrdersLog, true);
    clear(global, pr::kNoTableCode, max, pr::kNoStreamCode);
    global.commit();
    require(global.projector.take_row_changes().regular_trade_history_reloaded,
            "committed global clear omitted its regular TRADE reload proof");
    for (const auto table : {kFortsTradeReplOrdersLog, kFortsTradeReplUserDeal}) {
        Harness h;
        h.begin(table, true);
        h.row(table, 10, 1);
        h.commit();
        (void)h.projector.take_row_changes();
        h.begin(table, true);
        h.row(table, 10, 2, 1, true);
        h.commit();
        const auto tombstone = h.projector.take_row_changes();
        require(tombstone.regular_trade_history_truncated && !tombstone.regular_trade_history_reloaded,
                "regular row retirement was mistaken for a complete history reload");
        h.begin(table, true);
        clear(h, table, 100);
        h.commit();
        const auto floor = h.projector.take_row_changes();
        require(floor.regular_trade_history_truncated && !floor.regular_trade_history_reloaded,
                "finite TRADE purge floor was mistaken for a complete history reload");
    }
    for (const auto table :
         {kFortsTradeReplMultilegOrdersLog, kFortsTradeReplUserMultilegDeal, kFortsUserorderbookReplOrders}) {
        Harness h;
        h.begin(table, true);
        clear(h, table, max, stream(table));
        h.commit();
        require(!h.projector.take_row_changes().regular_trade_history_reloaded,
                "excluded/periodic table replacement invented a regular TRADE reload");
    }
    Harness overflow;
    overflow.begin(kFortsTradeReplOrdersLog, true);
    for (std::int64_t i = 1; i <= static_cast<std::int64_t>(ps::kPrivateRowChangeCapacity) + 1; ++i)
        overflow.row(kFortsTradeReplOrdersLog, i, i);
    overflow.commit();
    const auto gap = overflow.projector.take_row_changes();
    require(gap.resync_required && gap.regular_trade_history_truncated && !gap.regular_trade_history_reloaded,
            "delta overflow invented a complete history reload");
    overflow.begin(kFortsTradeReplOrdersLog, true);
    clear(overflow, kFortsTradeReplOrdersLog, max);
    overflow.commit();
    overflow.begin(kFortsTradeReplOrdersLog, true);
    for (std::int64_t i = 1; i <= static_cast<std::int64_t>(ps::kPrivateRowChangeCapacity) + 1; ++i)
        overflow.row(kFortsTradeReplOrdersLog, i, i);
    overflow.commit();
    require(overflow.projector.take_row_changes().regular_trade_history_reloaded,
            "delta overflow discarded a previously committed reload proof");
    overflow.begin(kFortsTradeReplOrdersLog, true);
    clear(overflow, kFortsTradeReplOrdersLog, max);
    overflow.commit();
    overflow.projector.reset();
    require(!overflow.projector.take_row_changes().regular_trade_history_reloaded,
            "projector reset retained the previous reload proof");
}
} // namespace moex::plaza2::test
