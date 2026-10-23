#pragma once
#include "fake_cgate_control.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"

namespace moex::connector_host::regression {
inline void explicit_test_risk(TradingHostConfig& config) {
    config.orders.login_from = "owner-login";
    config.orders.ext_id_range_configured = true;
    config.orders.command_rate_configured = true;
    config.orders.risk.quantity_configured = true;
    config.orders.risk.open_orders_configured = true;
    for (const auto isin : config.isin_ids) {
        config.orders.risk.max_notional_by_isin[isin] = INT64_MAX;
        config.orders.risk.max_position_by_isin[isin] = 100;
    }
}
inline void risk_limits_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                        const std::filesystem::path& root) {
    namespace test = plaza2::test;
    namespace fake = test::fake;
    namespace gen = plaza2::generated;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    config.journal_path = root / "risk-limits.ndjson";
    config.identity_state_path = root / "risk-limits.state";
    auto missing = config;
    missing.orders.command_rate_configured = false;
    bool rate_refused{};
    try {
        CgateTradingHost invalid(missing);
    } catch (const std::invalid_argument&) {
        rate_refused = true;
    }
    test::require(rate_refused, "allow-orders admitted an implicit command rate");
    missing = config;
    missing.orders.login_from.clear();
    bool identity_refused{};
    try {
        CgateTradingHost invalid(missing);
    } catch (const std::invalid_argument&) {
        identity_refused = true;
    }
    test::require(identity_refused, "allow-orders admitted no explicit order login");
    missing = config;
    missing.orders.ext_id_range_configured = false;
    identity_refused = false;
    try {
        CgateTradingHost invalid(missing);
    } catch (const std::invalid_argument&) {
        identity_refused = true;
    }
    test::require(identity_refused, "allow-orders admitted no instance-specific ext_id range");
    missing = config;
    missing.orders.risk.quantity_configured = false;
    bool refused{};
    try {
        CgateTradingHost invalid(missing);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    test::require(refused, "allow-orders admitted missing explicit quantity risk flag");
    missing = config;
    missing.orders.risk.max_position_by_isin.clear();
    refused = false;
    try {
        CgateTradingHost invalid(missing);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    test::require(refused, "allow-orders admitted missing target position cap");
    config.orders.risk.max_position_by_isin[isin] = 3;
    {
        auto lag = config;
        lag.journal_path = root / "position-lag.ndjson";
        lag.identity_state_path = root / "position-lag.state";
        control.configure({.suppress_auto_replies = true,
                           .suppress_initial_orders = true,
                           .zero_position = true,
                           .client_code = account,
                           .session_id = 321});
        CgateTradingHost host(lag);
        test::require(!host.start(), "native POS-lag startup");
        for (int i = 0; i < 30; ++i)
            test::require(!host.poll(), "native POS-lag bootstrap");
        const auto posts_before_fill = control.commands().size();
        const auto integer = [](gen::FieldCode field, std::int64_t value) {
            return fake::Field{.field_code = field, .signed_value = value};
        };
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue(
            {.stream_code = gen::StreamCode::kFortsTradeRepl,
             .table_code = gen::TableCode::kFortsTradeReplUserDeal,
             .revision = 77001,
             .fields = {
                 integer(kFortsTradeReplUserDealReplId, 77001),
                 integer(kFortsTradeReplUserDealIdDeal, 77001),
                 integer(kFortsTradeReplUserDealSessId, 321),
                 integer(kFortsTradeReplUserDealIsinId, isin),
                 integer(kFortsTradeReplUserDealPrivateOrderIdBuy, 88001),
                 integer(kFortsTradeReplUserDealXamount, 2),
                 {.field_code = kFortsTradeReplUserDealCodeBuy, .kind = fake::FieldKind::Text, .text = account},
                 {.field_code = kFortsTradeReplUserDealCodeSell, .kind = fake::FieldKind::Text, .text = "OTHER"},
                 {.field_code = kFortsTradeReplUserDealPrice, .kind = fake::FieldKind::Text, .text = "103000"}}});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        test::require(!host.poll(), "native fill before POS");
        const auto request = [&](const char* key, std::int32_t quantity) {
            return host.place({.client_order_id = key, .isin_id = isin, .price = "103000", .quantity = quantity});
        };
        test::require(!request("lag-before-pos", 2).empty() && control.commands().size() == posts_before_fill,
                      "native own fill2+POS0 admitted BUY2 under position cap3");
        const auto position = [&](std::int64_t last_deal, std::int64_t bought) {
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsPosRepl});
            control.enqueue(
                {.stream_code = gen::StreamCode::kFortsPosRepl,
                 .table_code = gen::TableCode::kFortsPosReplPosition,
                 .revision = 80000 + bought,
                 .fields = {
                     integer(kFortsPosReplPositionReplId, 80000),
                     integer(kFortsPosReplPositionIsinId, isin),
                     integer(kFortsPosReplPositionAccountType, 2),
                     integer(kFortsPosReplPositionXpos, 2),
                     integer(kFortsPosReplPositionXbuysQty, bought),
                     integer(kFortsPosReplPositionLastDealId, last_deal),
                     {.field_code = kFortsPosReplPositionClientCode, .kind = fake::FieldKind::Text, .text = account}}});
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsPosRepl});
            test::require(!host.poll(), "native POS catch-up");
        };
        position(99000, 2);
        test::require(!request("unobserved-pos-deal", 1).empty() && control.commands().size() == posts_before_fill,
                      "an arbitrary larger POS deal ID released the fill reservation");
        position(77001, 1);
        test::require(!request("incomplete-pos-counters", 1).empty() && control.commands().size() == posts_before_fill,
                      "incomplete POS counters released the fill reservation");
        position(77001, 2);
        test::require(request("caught-up-pos", 1).empty() && control.commands().size() == posts_before_fill + 1,
                      "exact POS fill proof double-counted a fill after catch-up");
        test::require(!host.stop(), "native POS-lag shutdown");
    }
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = account,
                       .session_id = 321});
    CgateTradingHost native(config);
    test::require(!native.start(), "native risk startup");
    for (int i = 0; i < 30; ++i)
        test::require(!native.poll(), "native risk bootstrap");
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsPosRepl});
    control.enqueue(
        {.stream_code = gen::StreamCode::kFortsPosRepl,
         .table_code = gen::TableCode::kFortsPosReplPosition,
         .revision = 80000,
         .fields = {{.field_code = kFortsPosReplPositionReplId, .signed_value = 80000},
                    {.field_code = kFortsPosReplPositionIsinId, .signed_value = isin},
                    {.field_code = kFortsPosReplPositionClientCode, .kind = fake::FieldKind::Text, .text = account},
                    {.field_code = kFortsPosReplPositionAccountType, .signed_value = 2},
                    {.field_code = kFortsPosReplPositionXpos, .signed_value = 2}}});
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsPosRepl});
    test::require(!native.poll(), "native POS commit");
    const auto posts = control.commands().size();
    test::require(native.place({.client_order_id = "over-position", .isin_id = isin, .price = "103000", .quantity = 2})
                          .find("position") != std::string::npos,
                  "actual ownPOS2+BUY2 escaped cap3");
    test::require(control.commands().size() == posts, "native position refusal reached publisher");
    test::require(
        native.place({.client_order_id = "headroom", .isin_id = isin, .price = "103000", .quantity = 1}).empty(),
        "actual ownPOS headroom refused");
    test::require(control.commands().size() == posts + 1, "native valid headroom was not sent");
    test::require(
        !native.place({.client_order_id = "working-risk", .isin_id = isin, .price = "103000", .quantity = 1}).empty() &&
            control.commands().size() == posts + 1,
        "native pendingBUY was omitted from position risk");
    test::require(!native.stop(), "native risk shutdown");
}
} // namespace moex::connector_host::regression
