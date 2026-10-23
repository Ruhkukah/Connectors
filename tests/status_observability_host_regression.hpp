#pragma once
#include "fake_cgate_control.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"

namespace moex::connector_host::regression {
inline void status_observability_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                                 const std::filesystem::path& root) {
    namespace fake = plaza2::test::fake;
    namespace gen = plaza2::generated;
    using enum gen::FieldCode;
    const auto check = plaza2::test::require;
    const auto account = config.orders.broker_code + config.orders.client_code;
    const auto isin = config.isin_ids.front();
    config.journal_path = root / "status-observability.ndjson";
    config.identity_state_path = root / "status-observability.state";
    config.orders.risk.max_position_by_isin[isin] = 3;
    config.measure_timings = true;
    control.configure(
        {.suppress_initial_orders = true, .zero_position = true, .client_code = account, .session_id = 321});
    CgateTradingHost host(config);
    check(!host.start(), "status observability startup");
    for (int i = 0; i < 30; ++i)
        check(!host.poll(), "status observability bootstrap");
    auto status = host.status();
    check(status.find("\"symbol\":\"RTS-6.26\"") != std::string::npos &&
              status.find("\"reference_price_scaled\":10250000000") != std::string::npos &&
              status.find("\"min_step_scaled\":25000000") != std::string::npos &&
              status.find("\"lower_price_scaled\":9250000000") != std::string::npos &&
              status.find("\"upper_price_scaled\":11250000000") != std::string::npos,
          "status omitted committed current-session symbol/reference/grid/bounds");
    check(status.find("\"trade_replay\":{\"anchor_ready\":true") != std::string::npos &&
              status.find("\"accepted_lifenum\":7") != std::string::npos &&
              status.find("\"trades_rev\":44") != std::string::npos &&
              status.find("\"pos_anchor\":{\"online\":true,\"snapshot_complete\":true") != std::string::npos,
          "status omitted accepted replay epoch or committed POS calendar anchor");
    check(status.find(config.orders.login_from) == std::string::npos &&
              status.find("\"login_configured\":true") != std::string::npos &&
              status.find("\"ext_id_begin\":" + std::to_string(config.orders.ext_id_begin)) != std::string::npos &&
              status.find("\"ext_id_end\":" + std::to_string(config.orders.ext_id_end)) != std::string::npos,
          "status omitted configured identity bounds or exposed private login");
    check(status.find("\"book\":{\"valid\":true,\"stream_last_callback_utc_ns\":0") == std::string::npos &&
              status.find("\"best_bid\":{\"price_scaled\":10250000000,\"quantity\":7}") != std::string::npos &&
              status.find("\"best_ask\":{\"price_scaled\":10275000000,\"quantity\":5}") != std::string::npos &&
              status.find("\"book_commit_monotonic_ns\":") != std::string::npos,
          "status omitted valid scoped quote quantities or local receipt evidence");
    control.set(fake::Option::PubReplyOrderId, "68001");
    check(host.place({.client_order_id = "status-sell",
                      .isin_id = isin,
                      .side = plaza2_trade::Plaza2TradeSide::Sell,
                      .price = "103000",
                      .quantity = 1})
              .empty(),
          "status observability order refused");
    check(!host.poll(), "status observability accepted order");
    status = host.status();
    const auto order = status.substr(status.find("\"client_order_id\":\"status-sell\""));
    check(order.find("\"side\":\"sell\",\"price\":\"103000\",\"quantity\":1") != std::string::npos,
          "status omitted logical order side/price/quantity");
    const auto integer = [](gen::FieldCode field, std::int64_t value) {
        return fake::Field{.field_code = field, .signed_value = value};
    };
    const auto commit = [&](gen::StreamCode stream) {
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = stream});
        check(!host.poll(), "status observability committed callback");
    };
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(
        {.stream_code = gen::StreamCode::kFortsTradeRepl,
         .table_code = gen::TableCode::kFortsTradeReplUserDeal,
         .revision = 77001,
         .fields = {integer(kFortsTradeReplUserDealReplId, 77001),
                    integer(kFortsTradeReplUserDealIdDeal, 77001),
                    integer(kFortsTradeReplUserDealSessId, 321),
                    integer(kFortsTradeReplUserDealIsinId, isin),
                    integer(kFortsTradeReplUserDealPrivateOrderIdBuy, 88001),
                    integer(kFortsTradeReplUserDealXamount, 2),
                    {.field_code = kFortsTradeReplUserDealCodeBuy, .kind = fake::FieldKind::Text, .text = account},
                    {.field_code = kFortsTradeReplUserDealPrice, .kind = fake::FieldKind::Text, .text = "103000"}}});
    commit(gen::StreamCode::kFortsTradeRepl);
    const auto posts = control.commands().size();
    status = host.status();
    check(status.find("\"pending_fill_reservations\":{\"buy_quantity\":2,\"sell_quantity\":0") != std::string::npos,
          "status omitted actual owned fill reservation while POS lagged");
    check(host.status() == status && control.commands().size() == posts,
          "read-only status changed risk state or posted a command");
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsPosRepl});
    control.enqueue(
        {.stream_code = gen::StreamCode::kFortsPosRepl,
         .table_code = gen::TableCode::kFortsPosReplPosition,
         .revision = 80001,
         .fields = {integer(kFortsPosReplPositionReplId, 80001),
                    integer(kFortsPosReplPositionIsinId, isin),
                    integer(kFortsPosReplPositionAccountType, 2),
                    integer(kFortsPosReplPositionXpos, 2),
                    integer(kFortsPosReplPositionXbuysQty, 2),
                    integer(kFortsPosReplPositionLastDealId, 77001),
                    {.field_code = kFortsPosReplPositionClientCode, .kind = fake::FieldKind::Text, .text = account}}});
    commit(gen::StreamCode::kFortsPosRepl);
    status = host.status();
    check(status.find("\"xpos\":2,\"account_type\":2,\"last_deal_id\":77001,\"buys\":2,\"sells\":0") !=
              std::string::npos,
          "status omitted current own POS exact deal and counters");
    check(status.find("\"pending_fill_reservations\":{\"buy_quantity\":2,\"sell_quantity\":0") != std::string::npos &&
              host.status() == status,
          "status silently reconciled cached reservations against new POS");
    check(
        !host.place({.client_order_id = "over-position", .isin_id = isin, .price = "103000", .quantity = 2}).empty() &&
            control.commands().size() == posts,
        "telemetry changed existing position admission semantics");
    status = host.status();
    check(status.find("\"pending_fill_reservations\":{\"buy_quantity\":0,\"sell_quantity\":0") != std::string::npos &&
              status.find(
                  "\"cached_position_proof\":{\"trade_lifenum\":7,\"calendar_revision\":44,\"last_deal_id\":77001") !=
                  std::string::npos,
          "status omitted reservation release after existing admission reconciled exact POS proof");
    control.enqueue({.kind = fake::EventKind::Close, .stream_code = gen::StreamCode::kFortsAggrRepl});
    check(!host.poll(), "status quote stream close");
    status = host.status();
    check(status.find("\"best_bid\":null,\"best_ask\":null") != std::string::npos,
          "status exposed old quote after AGGR invalidation");
    check(!host.stop(), "status observability shutdown");
    control.configure({});
}
} // namespace moex::connector_host::regression
