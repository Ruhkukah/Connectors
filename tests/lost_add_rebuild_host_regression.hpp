#pragma once

#include "late_move_host_regression.hpp"

namespace moex::connector_host {
inline void lost_add_rebuild_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                             const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    config.session.reply_timeout_ms = 10;
    config.orders.reply_timeout = std::chrono::milliseconds(10);
    config.utc_now = [] { return std::int64_t{1700000005}; };
    config.journal_path = root / "lost-add-rebuild.ndjson";
    config.identity_state_path = root / "lost-add-rebuild.state";
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = account,
                       .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    const auto poll = [&] { require(!host.poll(), "lost Add rebuilding owner poll"); };
    const auto first = control.commands().size();
    require(host.place({.client_order_id = "lost-during-rebuild", .isin_id = isin, .price = "103000", .quantity = 2})
                .empty(),
            "lost Add rebuilding seed refused");
    poll();
    auto commands = control.commands();
    require(commands.size() == first + 1 && commands.back().name == "AddOrder", "lost Add seed not posted");
    const auto add_user_id = commands.back().user_id;
    official_cgate99::AddOrder add{};
    std::memcpy(&add, commands.back().payload.data(), sizeof(add));
    now += std::chrono::milliseconds(20);
    poll();
    commands = control.commands();
    require(commands.back().name == "DelUserOrders", "lost Add did not begin scoped recovery");
    const auto cancel_user_id = commands.back().user_id;

    // A committed USERORDERBOOK update discarded before the owner drains it
    // requires reconstruction. TRADE remains ONLINE throughout this gap.
    control.set(fake::Option::DelayUserorderbook);
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsUserorderbookRepl});
    control.enqueue(
        {.stream_code = gen::StreamCode::kFortsUserorderbookRepl,
         .table_code = gen::TableCode::kFortsUserorderbookReplOrders,
         .revision = 71000,
         .fields = {integer(kFortsUserorderbookReplOrdersReplId, 71000),
                    integer(kFortsUserorderbookReplOrdersPublicOrderId, 71000),
                    integer(kFortsUserorderbookReplOrdersPrivateOrderId, 71000),
                    integer(kFortsUserorderbookReplOrdersSessId, 321),
                    integer(kFortsUserorderbookReplOrdersIsinId, isin), integer(kFortsUserorderbookReplOrdersDir, 1),
                    integer(kFortsUserorderbookReplOrdersPublicAction, 1),
                    integer(kFortsUserorderbookReplOrdersPrivateAction, 1),
                    integer(kFortsUserorderbookReplOrdersPublicAmount, 1),
                    integer(kFortsUserorderbookReplOrdersPrivateAmount, 1),
                    integer(kFortsUserorderbookReplOrdersPublicAmountRest, 1),
                    integer(kFortsUserorderbookReplOrdersPrivateAmountRest, 1),
                    text(kFortsUserorderbookReplOrdersClientCode, account),
                    text(kFortsUserorderbookReplOrdersPrice, "103000")}});
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsUserorderbookRepl});
    control.enqueue({.kind = fake::EventKind::Close, .stream_code = gen::StreamCode::kFortsUserorderbookRepl});
    late_move_host_detail::reply(control, cancel_user_id, 186, official_cgate99::FORTS_MSG186{});
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue({.stream_code = gen::StreamCode::kFortsTradeRepl,
                     .table_code = gen::TableCode::kFortsTradeReplHeartbeat,
                     .revision = 71001,
                     .fields = {{.field_code = kFortsTradeReplHeartbeatServerTime,
                                 .kind = fake::FieldKind::Timestamp,
                                 .unsigned_value = 1700020005}}});
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    poll();
    const auto unresolved = late_move_host_detail::logical_order(host, "lost-during-rebuild");
    require(host.status().find("\"reconstructing\":true") != std::string::npos,
            "lost Add fixture did not hold the private snapshot barrier");
    require(unresolved.find("\"order_id\":0") != std::string::npos &&
                unresolved.find("\"state\":\"Cancelled\"") == std::string::npos &&
                unresolved.find("NotFound") == std::string::npos,
            "lost Add was declared NotFound while its current snapshot was still rebuilding");

    auto row = own_order(71002, isin, account);
    row.fields.push_back(integer(kFortsTradeReplOrdersLogExtId, add.ext_id));
    row.fields.push_back(text(kFortsTradeReplOrdersLogLoginFrom, config.orders.login_from));
    for (auto& field : row.fields) {
        if (field.field_code == kFortsTradeReplOrdersLogPublicAmount ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAmount)
            field.signed_value = 2;
        if (field.field_code == kFortsTradeReplOrdersLogPublicAmountRest ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAmountRest)
            field.signed_value = 0;
        if (field.field_code == kFortsTradeReplOrdersLogPublicAction ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAction)
            field.signed_value = 2;
    }
    auto fill = own_trade(71003, isin, account, 71002);
    for (auto& field : fill.fields)
        if (field.field_code == kFortsTradeReplUserDealXamount)
            field.signed_value = 2;
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(row);
    control.enqueue(fill);
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    poll();
    poll(); // Observe CLOSED before advancing the bounded listener retry.
    now += std::chrono::seconds(2);
    control.clear(fake::Option::DelayUserorderbook);
    for (int i = 0; i < 30; ++i)
        poll();
    const auto resolved = late_move_host_detail::logical_order(host, "lost-during-rebuild");
    require(host.status().find("\"reconstructing\":false") != std::string::npos &&
                resolved.find("\"order_id\":71002") != std::string::npos &&
                resolved.find("\"state\":\"Filled\"") != std::string::npos &&
                resolved.find("\"executed\":2") != std::string::npos &&
                resolved.find("\"remaining\":0") != std::string::npos &&
                host.status().find("recovered:321:71002") == std::string::npos,
            "complete current snapshot did not retain the exact owned lost-Add fill");
    official_cgate99::FORTS_MSG179 late{};
    late.order_id = 71002;
    late_move_host_detail::reply(control, add_user_id, 179, late);
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(row);
    control.enqueue(fill);
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    poll();
    require(late_move_host_detail::logical_order(host, "lost-during-rebuild").find("\"executed\":2") !=
                std::string::npos,
            "late179/replayed native fill was credited twice");
    std::size_t adds{};
    for (std::size_t i = first; i < control.commands().size(); ++i)
        adds += control.commands()[i].name == "AddOrder";
    require(adds == 1, "rebuilding blindly retried the lost Add");
    require(!host.stop(), "lost Add rebuilding stop failed");
}
} // namespace moex::connector_host
