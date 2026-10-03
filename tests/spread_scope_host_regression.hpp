#pragma once
#include "late_move_host_regression.hpp"
namespace moex::connector_host {
inline void spread_scope_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                         const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    config.journal_path = root / "spread-scope.ndjson";
    config.identity_state_path = root / "spread-scope.state";
    control.configure({.suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = config.orders.broker_code + config.orders.client_code,
                       .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsRefdataRepl});
    control.enqueue({.stream_code = gen::StreamCode::kFortsRefdataRepl,
                     .table_code = gen::TableCode::kFortsRefdataReplFutSessContents,
                     .revision = 82000,
                     .fields = {integer(kFortsRefdataReplFutSessContentsReplId, 82000),
                                integer(kFortsRefdataReplFutSessContentsIsinId, isin),
                                integer(kFortsRefdataReplFutSessContentsSessId, 321),
                                integer(kFortsRefdataReplFutSessContentsIsSpread, 1),
                                text(kFortsRefdataReplFutSessContentsMinStep, "1"),
                                text(kFortsRefdataReplFutSessContentsSettlementPrice, "105000"),
                                text(kFortsRefdataReplFutSessContentsLimitUp, "10000"),
                                text(kFortsRefdataReplFutSessContentsLimitDown, "10000")}});
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsRefdataRepl});
    require(!host.poll(), "spread scope terms commit");
    const auto before = control.commands().size();
    require(
        !host.place({.client_order_id = "excluded-calendar-spread", .isin_id = isin, .price = "103000", .quantity = 1})
             .empty(),
        "declared futures scope admitted calendar-spread Add");
    require(control.commands().size() == before, "excluded spread reached native publisher");
    require(host.cancel_all(isin).empty() && control.commands().size() == before + 1,
            "spread guard blocked risk-reducing cancel");
    require(!host.stop(), "spread scope stop");
}
} // namespace moex::connector_host
