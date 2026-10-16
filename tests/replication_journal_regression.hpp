#pragma once

#include "private_delta_host_regression.hpp"
#include "late_move_host_regression.hpp"

#include <iostream>

namespace moex::connector_host::regression {
inline void replication_journal_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                           const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    config.journal_path = root / "replication-journal.ndjson";
    config.identity_state_path = root / "replication-journal.state";
    config.session.process_timeout_ms = 0;
    control.configure(
        {.suppress_initial_orders = true, .zero_position = true, .client_code = account, .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    const auto transaction = [&](fake::Event row) {
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = row.stream_code});
        control.enqueue(row);
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = row.stream_code});
    };
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    constexpr std::size_t foreign_rows = 1000;
    for (std::size_t i = 0; i < foreign_rows; ++i)
        control.enqueue(own_order(70000 + static_cast<std::int64_t>(i), isin, "SPD5_FOREIGN_ACCOUNT"));
    control.enqueue(own_order(71001, isin, account));
    // Own exposure outside configured order-entry targets is still evidence.
    control.enqueue(own_order(71002, isin + 10000, account));
    control.enqueue(own_order(71004, isin, config.orders.client_code));
    auto tombstone = own_order(71003, isin, "");
    tombstone.fields.push_back(integer(kFortsTradeReplOrdersLogReplAct, 71001));
    control.enqueue(tombstone);
    control.enqueue(own_trade(72001, isin, account, 71001));
    auto own_sell = own_trade(72002, isin, "SPD5_COUNTERPARTY", 71001);
    for (auto& field : own_sell.fields)
        if (field.field_code == kFortsTradeReplUserDealCodeSell)
            field.text = account;
    control.enqueue(own_sell);
    control.enqueue(own_trade(72003, isin, "SPD5_FOREIGN_ACCOUNT", 70000));
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    for (const auto id : {79001, 79002})
        transaction(
            {.stream_code = gen::StreamCode::kFortsUserorderbookRepl,
             .table_code = gen::TableCode::kFortsUserorderbookReplOrders,
             .revision = id,
             .fields = {
                 integer(kFortsUserorderbookReplOrdersReplId, id),
                 integer(kFortsUserorderbookReplOrdersPrivateOrderId, id),
                 integer(kFortsUserorderbookReplOrdersSessId, 321), integer(kFortsUserorderbookReplOrdersIsinId, isin),
                 text(kFortsUserorderbookReplOrdersClientCode, id == 79001 ? account : "SPD5_FOREIGN_USERBOOK")}});
    const auto position = [&](std::int64_t id, std::int32_t instrument, const std::string& client,
                              std::int64_t account_type) {
        return fake::Event{
            .stream_code = gen::StreamCode::kFortsPosRepl,
            .table_code = gen::TableCode::kFortsPosReplPosition,
            .revision = id,
            .fields = {integer(kFortsPosReplPositionReplId, id), integer(kFortsPosReplPositionIsinId, instrument),
                       text(kFortsPosReplPositionClientCode, client),
                       integer(kFortsPosReplPositionAccountType, account_type), integer(kFortsPosReplPositionXpos, 0)}};
    };
    const auto own_type = config.orders.client_code.empty() ? 1 : 2;
    transaction(position(73001, isin, account, own_type));
    transaction(position(73002, isin + 10000, account, own_type));
    transaction(position(73003, isin, "SPD5_FOREIGN_POSITION", own_type));
    transaction(position(73004, isin, account, own_type == 1 ? 2 : 1));
    for (const auto [id, instrument] : {std::pair{74001, isin}, std::pair{74002, isin + 10000}})
        transaction({.stream_code = gen::StreamCode::kFortsInstrumentstateRepl,
                     .table_code = gen::TableCode::kFortsInstrumentstateReplInstrumentState,
                     .revision = id,
                     .fields = {integer(kFortsInstrumentstateReplInstrumentStateReplId, id),
                                integer(kFortsInstrumentstateReplInstrumentStateIsinId, instrument),
                                integer(kFortsInstrumentstateReplInstrumentStatePublicState, 1)}});
    transaction({.stream_code = gen::StreamCode::kFortsSessionstateRepl,
                 .table_code = gen::TableCode::kFortsSessionstateReplSessionState,
                 .revision = 75001,
                 .fields = {integer(kFortsSessionstateReplSessionStateReplId, 75001),
                            integer(kFortsSessionstateReplSessionStateSessId, 321),
                            integer(kFortsSessionstateReplSessionStatePublicState, 1)}});
    transaction({.stream_code = gen::StreamCode::kFortsAggrRepl,
                 .table_code = gen::TableCode::kFortsAggrReplSysEvents,
                 .revision = 76001,
                 .fields = {integer(kFortsAggrReplSysEventsReplId, 76001), integer(kFortsAggrReplSysEventsSessId, 321),
                            integer(kFortsAggrReplSysEventsEventType, 1)}});
    transaction({.stream_code = gen::StreamCode::kFortsRefdataRepl,
                 .table_code = gen::TableCode::kFortsRefdataReplSysMessages,
                 .revision = 77001,
                 .fields = {integer(kFortsRefdataReplSysMessagesReplId, 77001),
                            integer(kFortsRefdataReplSysMessagesMsgId, 77001),
                            text(kFortsRefdataReplSysMessagesText, "SPD5_EXCHANGE_MESSAGE")}});
    official_cgate99::FORTS_MSG179 reply{};
    reply.order_id = 78001;
    late_move_host_detail::reply(control, 98001, 179, reply);
    for (int i = 0; i < 40; ++i)
        require(!host.poll(), "replication journal fixture drain");
    require(!host.stop(), "replication journal fixture stop");
    std::ifstream journal(config.journal_path);
    std::string line;
    std::string rows;
    bool raw_reply{}, lifecycle{}, begin{}, commit{}, duplicate_hex{};
    std::size_t total_bytes{}, row_bytes{};
    while (std::getline(journal, line)) {
        total_bytes += line.size() + 1;
        if (line.find("\"event\":\"stream_row\"") != std::string::npos) {
            if (line.find("\"stream\":\"p2mqreply\"") != std::string::npos) {
                raw_reply |= line.find("\"message_id\":179") != std::string::npos &&
                             line.find("\"user_id\":98001") != std::string::npos &&
                             line.find("\"payload_hex\":\"\"") == std::string::npos &&
                             line.find("\"payload_hex\":") != std::string::npos;
                continue;
            }
            row_bytes += line.size() + 1;
            duplicate_hex |= line.find("\"payload_hex\":") != std::string::npos;
            rows += line;
        }
        lifecycle |= line.find("\"event\":\"listener_online\"") != std::string::npos;
        begin |= line.find("\"event\":\"transaction_begin\"") != std::string::npos;
        commit |= line.find("\"event\":\"transaction_commit\"") != std::string::npos;
    }
    std::cout << "SPD5 replication journal: foreign_input_rows=" << foreign_rows << " retained_row_bytes=" << row_bytes
              << " total_bytes=" << total_bytes << '\n';
    require(rows.find("SPD5_FOREIGN_ACCOUNT") == std::string::npos &&
                rows.find("SPD5_FOREIGN_POSITION") == std::string::npos &&
                rows.find("SPD5_FOREIGN_USERBOOK") == std::string::npos,
            "journal retained unrelated account traffic");
    const auto has = [&](std::int64_t id) {
        return rows.find("\"signed\":" + std::to_string(id)) != std::string::npos;
    };
    for (const auto id : {71001, 71002, 71003, 72001, 72002, 73001, 74001, 75001, 76001, 77001, 79001})
        require(has(id), "journal dropped required own/state/announcement row " + std::to_string(id));
    for (const auto id : {71004, 72003, 73002, 73003, 73004, 74002, 79002})
        require(!has(id), "journal retained unconfigured/foreign row " + std::to_string(id));
    require(!duplicate_hex, "replication journal duplicated decoded fields as raw hex");
    require(raw_reply && lifecycle && begin && commit, "journal lost reply bytes or recovery/transaction evidence");
    require(rows.find("SPD5_EXCHANGE_MESSAGE") != std::string::npos, "journal lost exchange announcement");
}
} // namespace moex::connector_host::regression
