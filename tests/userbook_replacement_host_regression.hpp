#pragma once

#include "late_move_host_regression.hpp"

namespace moex::connector_host::regression {
inline void userbook_replacement_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                                 const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    config.session.process_timeout_ms = 0;
    config.journal_path = root / "userbook-replacement.ndjson";
    config.identity_state_path = root / "userbook-replacement.state";
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = account,
                       .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    require(
        host.place({.client_order_id = "book-replacement", .isin_id = isin, .price = "103000", .quantity = 2}).empty(),
        "USERORDERBOOK replacement fixture Add refused");
    official_cgate99::FORTS_MSG179 accepted{};
    accepted.order_id = 70001;
    late_move_host_detail::reply(control, control.commands().back().user_id, 179, accepted);
    late_move_host_detail::order(control, accepted.order_id, 70001, 2, 1, isin, account);
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(own_trade(70002, isin, account, accepted.order_id));
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    require(!host.poll(), "USERORDERBOOK replacement fixture reply/fill");
    const auto opens = control.opens(gen::StreamCode::kFortsTradeRepl);
    const auto book = [&](std::int64_t id, std::int64_t revision, std::int64_t act = 0, bool sparse = false) {
        fake::Event row{.stream_code = gen::StreamCode::kFortsUserorderbookRepl,
                        .table_code = gen::TableCode::kFortsUserorderbookReplOrders,
                        .revision = revision,
                        .fields = {integer(kFortsUserorderbookReplOrdersReplId, 80000),
                                   integer(kFortsUserorderbookReplOrdersReplAct, act)}};
        if (!sparse) {
            const std::array fields{integer(kFortsUserorderbookReplOrdersPublicOrderId, id),
                                    integer(kFortsUserorderbookReplOrdersPrivateOrderId, id),
                                    integer(kFortsUserorderbookReplOrdersSessId, 321),
                                    integer(kFortsUserorderbookReplOrdersIsinId, isin),
                                    integer(kFortsUserorderbookReplOrdersDir, 1),
                                    integer(kFortsUserorderbookReplOrdersPublicAction, 1),
                                    integer(kFortsUserorderbookReplOrdersPrivateAction, 1),
                                    integer(kFortsUserorderbookReplOrdersPublicAmount, 2),
                                    integer(kFortsUserorderbookReplOrdersPrivateAmount, 2),
                                    integer(kFortsUserorderbookReplOrdersPublicAmountRest, 1),
                                    integer(kFortsUserorderbookReplOrdersPrivateAmountRest, 1),
                                    text(kFortsUserorderbookReplOrdersClientCode, account),
                                    text(kFortsUserorderbookReplOrdersPrice, "103000")};
            row.fields.insert(row.fields.end(), fields.begin(), fields.end());
        }
        control.enqueue(row);
    };
    const auto commit = [&](std::int64_t revision) {
        control.enqueue({.stream_code = gen::StreamCode::kFortsUserorderbookRepl,
                         .table_code = gen::TableCode::kFortsUserorderbookReplInfo,
                         .revision = revision,
                         .fields = {integer(kFortsUserorderbookReplInfoPublicationState, 1),
                                    integer(kFortsUserorderbookReplInfoTradesRev, revision),
                                    integer(kFortsUserorderbookReplInfoTradesLifenum, 7)}});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsUserorderbookRepl});
        require(!host.poll(), "USERORDERBOOK replacement owner poll");
    };
    for (const auto replacement : {0, 1, 2}) {
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsUserorderbookRepl});
        book(70001, 90000 + replacement * 10);
        commit(90001 + replacement * 10);
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsUserorderbookRepl});
        if (replacement == 2)
            book(70004, 90002 + replacement * 10);
        else
            book(70001, 90002 + replacement * 10, 1, replacement == 1);
        commit(90003 + replacement * 10);
        const auto order = late_move_host_detail::logical_order(host, "book-replacement");
        require(control.opens(gen::StreamCode::kFortsTradeRepl) == opens &&
                    host.status().find("\"reconstructing\":false") != std::string::npos &&
                    order.find("\"order_id\":70001") != std::string::npos &&
                    order.find("\"executed\":1") != std::string::npos &&
                    order.find("\"remaining\":1") != std::string::npos &&
                    order.find("\"execution_baseline_known\":true") != std::string::npos,
                "USERORDERBOOK tombstone/replID reuse reopened TRADE or lost regular order authority");
    }
    require(host.move("book-replacement", "103250", 2).empty(),
            "USERORDERBOOK replacement blocked the tracked regular Move");
    require(control.commands().back().name == "MoveOrder", "regular Move was not sent after book replacement");
    require(!host.stop(), "USERORDERBOOK replacement fixture stop");
    std::ifstream journal(config.journal_path);
    const std::string records{std::istreambuf_iterator<char>(journal), std::istreambuf_iterator<char>()};
    require(records.find("\"event\":\"private_history_gap\"") == std::string::npos,
            "USERORDERBOOK replacement unnecessarily reported a private history gap");
}
} // namespace moex::connector_host::regression
