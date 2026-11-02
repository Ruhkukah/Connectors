#pragma once

#include "late_move_host_regression.hpp"

namespace moex::connector_host {
inline void early_add_row_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                          const std::filesystem::path& root) {
    namespace fake = plaza2::test::fake;
    namespace gen = plaza2::generated;
    using plaza2::test::require;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    config.session.reply_timeout_ms = 60000;
    config.orders.reply_timeout = std::chrono::seconds(60);
    config.orders.login_from = "owner-login";
    config.orders.ext_id_range_configured = true;
    config.orders.ext_id_begin = config.orders.next_ext_id = 120000;
    config.orders.ext_id_end = 129999;
    config.orders.risk.max_quantity = 2;
    config.orders.risk.max_open_orders = 2;
    config.orders.risk.max_notional_scaled = 20'600'000'000LL;
    config.orders.risk.max_notional_by_isin = {{isin, 20'600'000'000LL}};
    config.orders.risk.max_position_by_isin = {{isin, 2}};
    config.utc_now = [] { return std::int64_t{1700000005}; };
    config.journal_path = root / "native-early-add-row.ndjson";
    config.identity_state_path = root / "native-early-add-row.state";
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = account,
                       .session_id = 321});
    CgateTradingHost host(config);
    private_delta_host_detail::bootstrap(host);
    const auto poll = [&] {
        const auto error = host.poll();
        require(!error, "native early Add poll: " + error.message);
    };
    const auto before = control.commands().size();
    require(host.place(private_delta_host_detail::add("early-native-first", isin)).empty(),
            "native early Add first request refused");
    poll();
    const auto first = control.commands().back();
    require(first.name == "AddOrder", "native early Add not posted");
    official_cgate99::AddOrder wire{};
    require(first.payload.size() == sizeof(wire), "native early Add wire size");
    std::memcpy(&wire, first.payload.data(), sizeof(wire));
    auto early = private_delta_host_detail::own_order(64601, isin, account);
    early.fields.push_back(private_delta_host_detail::integer(kFortsTradeReplOrdersLogExtId, wire.ext_id));
    early.fields.push_back(private_delta_host_detail::text(kFortsTradeReplOrdersLogLoginFrom, "owner-login"));
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(early);
    poll();
    require(late_move_host_detail::logical_order(host, "early-native-first").find("\"order_id\":0") !=
                std::string::npos,
            "uncommitted native row bound the pending Add");
    require(host.status().find("recovered:321:64601") == std::string::npos,
            "uncommitted native row created recovered exposure");
    now += std::chrono::milliseconds(20);
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    poll();
    require(late_move_host_detail::logical_order(host, "early-native-first").find("\"order_id\":64601") !=
                    std::string::npos &&
                host.status().find("recovered:321:64601") == std::string::npos,
            "committed own row did not bind exactly once before179");
    require(host.place(private_delta_host_detail::add("early-native-second", isin)).empty(),
            "early committed row double-charged native position/notional/open-order caps");
    poll();
    const auto second = control.commands().back();
    require(control.commands().size() == before + 2 && second.name == "AddOrder",
            "second same-ISIN Add was held until first reply");
    require(!host.place(private_delta_host_detail::add("early-native-third", isin)).empty(),
            "early binding released real exposure past the two-order cap");
    now += std::chrono::milliseconds(20);
    official_cgate99::FORTS_MSG179 accepted{};
    accepted.order_id = 64601;
    late_move_host_detail::reply(control, first.user_id, 179, accepted);
    accepted.order_id = 64602;
    late_move_host_detail::reply(control, second.user_id, 179, accepted);
    poll();
    require(late_move_host_detail::logical_order(host, "early-native-first").find("\"state\":\"Working\"") !=
                std::string::npos,
            "matching179 lost the early bound native order");
    require(host.cancel("early-native-first").empty(), "native late cancel seed refused");
    poll();
    const auto cancelled = control.commands().back();
    require(cancelled.name == "DelOrder", "native early known-ID cancellation not posted");
    late_move_host_detail::order(control, 64601, 64611, 1, 0, isin, account);
    poll();
    official_cgate99::FORTS_MSG177 deleted{};
    deleted.amount = 1;
    late_move_host_detail::reply(control, cancelled.user_id, 177, deleted);
    poll();
    require(host.cancel("early-native-second").empty(), "native second cancellation refused");
    poll();
    const auto second_cancel = control.commands().back();
    late_move_host_detail::reply(control, second_cancel.user_id, 177, deleted);
    late_move_host_detail::order(control, 64602, 64612, 1, 0, isin, account);
    poll();
    // A terminal early fill row must not make signal closeout look flat or
    // reconciled while the matching committed user_deal is still missing.
    require(host.place({.client_order_id = "early-native-filled", .isin_id = isin, .price = "103000", .quantity = 2})
                .empty(),
            "native early fill seed refused");
    poll();
    const auto filled_add = control.commands().back();
    std::memcpy(&wire, filled_add.payload.data(), sizeof(wire));
    auto filled = private_delta_host_detail::own_order(64603, isin, account);
    for (auto& field : filled.fields) {
        if (field.field_code == kFortsTradeReplOrdersLogPublicAmount ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAmount ||
            field.field_code == kFortsTradeReplOrdersLogPublicAction ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAction)
            field.signed_value = 2;
        if (field.field_code == kFortsTradeReplOrdersLogPublicAmountRest ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAmountRest)
            field.signed_value = 0;
    }
    filled.fields.push_back(private_delta_host_detail::integer(kFortsTradeReplOrdersLogExtId, wire.ext_id));
    filled.fields.push_back(private_delta_host_detail::text(kFortsTradeReplOrdersLogLoginFrom, "owner-login"));
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(filled);
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    poll();
    host.request_shutdown_cancellation(isin);
    require(host.shutdown_reconciliation_pending(), "SIGTERM reconciliation ignored an uncredited early fill");
    accepted.order_id = 64603;
    late_move_host_detail::reply(control, filled_add.user_id, 179, accepted);
    poll();
    require(host.shutdown_reconciliation_pending(), "179 falsely completed missing-deal shutdown reconciliation");
    auto deal = private_delta_host_detail::own_trade(84603, isin, account, 64603);
    for (auto& field : deal.fields)
        if (field.field_code == kFortsTradeReplUserDealXamount)
            field.signed_value = 2;
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(deal);
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    poll();
    require(!host.shutdown_reconciliation_pending(),
            "real committed user_deal did not complete early-fill correlation");
    require(!host.stop(), "native early Add host stop failed");
    std::ifstream stream(config.journal_path);
    const std::string journal{std::istreambuf_iterator<char>(stream), {}};
    const auto late = "\"event\":\"late_reply\",\"data\":{\"user_id\":" + std::to_string(cancelled.user_id);
    require(journal.find(late) != std::string::npos && journal.find("\"event\":\"unknown_reply\"") == std::string::npos,
            "native cancellation-before177 was still logged unknown");
    require(control.commands().size() == before + 5, "early native identity/reply correction duplicated a command");
}
} // namespace moex::connector_host
