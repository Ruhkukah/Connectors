#pragma once

#include "late_move_host_regression.hpp"

namespace moex::connector_host::regression {
inline void multileg_history_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                             const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    config.session.process_timeout_ms = 0;
    config.journal_path = root / "multileg-history.ndjson";
    config.identity_state_path = root / "multileg-history.state";
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = account,
                       .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    require(
        host.place({.client_order_id = "regular-partial", .isin_id = isin, .price = "103000", .quantity = 2}).empty(),
        "multileg history fixture Add refused");
    official_cgate99::FORTS_MSG179 accepted{};
    accepted.order_id = 70001;
    late_move_host_detail::reply(control, control.commands().back().user_id, 179, accepted);
    late_move_host_detail::order(control, accepted.order_id, 70001, 2, 1, isin, account);
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(own_trade(70002, isin, account, accepted.order_id));
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    require(!host.poll(), "multileg history fixture seed reply/fill");
    const auto before = late_move_host_detail::logical_order(host, "regular-partial");
    require(before.find("\"executed\":1") != std::string::npos && before.find("\"remaining\":1") != std::string::npos &&
                before.find("\"execution_baseline_known\":true") != std::string::npos,
            "multileg history fixture lacked the established regular execution baseline");
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    for (const auto table :
         {gen::TableCode::kFortsTradeReplMultilegOrdersLog, gen::TableCode::kFortsTradeReplUserMultilegDeal})
        control.enqueue({.kind = fake::EventKind::ClearDeleted,
                         .stream_code = gen::StreamCode::kFortsTradeRepl,
                         .table_code = table,
                         .revision = std::numeric_limits<std::int64_t>::max(),
                         .flags = 8});
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    require(!host.poll(), "multileg-only history cleanup poll");
    const auto after = late_move_host_detail::logical_order(host, "regular-partial");
    require(host.status().find("\"reconstructing\":false") != std::string::npos &&
                after.find("\"execution_baseline_known\":true") != std::string::npos &&
                after.find("\"executed\":1") != std::string::npos,
            "excluded multileg cleanup rebuilt or invalidated a regular order");
    const auto posts = control.commands().size();
    require(host.move("regular-partial", "103250", 2).empty(),
            "excluded multileg cleanup refused the tracked regular Move");
    const auto commands = control.commands();
    require(commands.size() == posts + 1 && commands.back().name == "MoveOrder" &&
                commands.back().payload.size() == sizeof(official_cgate99::MoveOrder),
            "regular Move after multileg cleanup did not reach the native publisher");
    official_cgate99::MoveOrder moved{};
    std::memcpy(&moved, commands.back().payload.data(), sizeof(moved));
    require(moved.order_id1 == 70001 && moved.amount1 == 2,
            "multileg cleanup changed the established identity or total Move amount");
    require(!host.stop(), "multileg history fixture stop");
    std::ifstream journal(config.journal_path);
    const std::string records{std::istreambuf_iterator<char>(journal), std::istreambuf_iterator<char>()};
    require(records.find("\"event\":\"private_history_gap\"") == std::string::npos,
            "excluded multileg cleanup unnecessarily rebuilt regular private orders");
}
} // namespace moex::connector_host::regression
