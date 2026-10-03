#pragma once
#include "add_reply_host_regression.hpp"

namespace moex::connector_host {
inline void move_link_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                      const std::filesystem::path& root) {
    using namespace late_move_host_detail;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    config.session.reply_timeout_ms = 10;
    config.orders.reply_timeout = std::chrono::milliseconds(10);
    config.orders.risk.max_notional_scaled = 100'000'000'000LL;
    config.utc_now = [] { return std::int64_t{1700000005}; };
    config.journal_path = root / "native-move-link.ndjson";
    config.identity_state_path = root / "native-move-link.state";
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = account,
                       .session_id = 321});
    CgateTradingHost host(config);
    private_delta_host_detail::bootstrap(host);
    const auto poll = [&] {
        const auto error = host.poll();
        require(!error, "native replacement link poll: " + error.message);
    };
    const auto begin = control.commands().size();
    require(host.place({.client_order_id = "linked-move", .isin_id = isin, .price = "103000", .quantity = 3}).empty(),
            "native replacement link seed refused");
    const auto add = control.commands().back();
    official_cgate99::FORTS_MSG179 accepted{};
    accepted.order_id = 64001;
    reply(control, add.user_id, 179, accepted);
    order(control, 64001, 64001, 3, 3, isin, account);
    poll();
    require(host.move("linked-move", "103250", 3).empty(), "native replacement link Move refused");
    const auto move = control.commands().back();
    const auto wire = add_reply_host_detail::wire<official_cgate99::MoveOrder>(move);
    now += std::chrono::milliseconds(20);
    poll();
    order(control, 64001, 65001, 3, 0, isin, account);
    poll();
    const auto replacement = [&](std::int64_t previous, std::int64_t revision) {
        using enum gen::FieldCode;
        auto row = private_delta_host_detail::own_order(64002, isin, account);
        row.revision = revision;
        for (auto& field : row.fields) {
            if (field.field_code == kFortsTradeReplOrdersLogPublicAmount ||
                field.field_code == kFortsTradeReplOrdersLogPrivateAmount ||
                field.field_code == kFortsTradeReplOrdersLogPublicAmountRest ||
                field.field_code == kFortsTradeReplOrdersLogPrivateAmountRest)
                field.signed_value = 3;
            if (field.field_code == kFortsTradeReplOrdersLogPrice)
                field.text = "103250";
        }
        row.fields.push_back(private_delta_host_detail::integer(kFortsTradeReplOrdersLogExtId, wire.ext_id1));
        row.fields.push_back(private_delta_host_detail::integer(kFortsTradeReplOrdersLogPrevorderId, previous));
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue(row);
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        poll();
    };
    replacement(99999, 65002);
    require(logical_order(host, "linked-move").find("\"state\":\"Unknown\"") != std::string::npos,
            "native Move adopted an ext_id row without its exact parent link");
    replacement(64001, 65003);
    const auto resolved = logical_order(host, "linked-move");
    require(resolved.find("\"order_id\":64002") != std::string::npos &&
                resolved.find("\"state\":\"Working\"") != std::string::npos &&
                resolved.find("\"remaining\":3") != std::string::npos,
            "native prevorder_id was not decoded or did not resolve the lost176");
    require(control.commands().size() == begin + 2,
            "native lost176 recovery emitted an unrequested cancellation or a duplicate Move");
    official_cgate99::FORTS_MSG176 late{};
    late.order_id1 = 64002;
    reply(control, move.user_id, 176, late);
    poll();
    require(host.move("linked-move", "103500", 4).empty(), "exact native replacement stayed blocked after recovery");
    require(!host.stop(), "native replacement link stop failed");
}
} // namespace moex::connector_host
