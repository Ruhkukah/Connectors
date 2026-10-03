#pragma once

#include "moex/connector_host/trading_host.hpp"
#include "fake_cgate_control.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fixtures/cgate99_messages.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace moex::connector_host {
namespace private_delta_host_detail {
namespace fake = plaza2::test::fake;
namespace gen = plaza2::generated;
namespace cg = plaza2::cgate;
using plaza2::test::require;

inline fake::Field integer(gen::FieldCode code, std::int64_t value) {
    return {.field_code = code, .signed_value = value};
}
inline fake::Field text(gen::FieldCode code, const std::string& value) {
    return {.field_code = code, .kind = fake::FieldKind::Text, .text = value};
}
inline fake::Event own_order(std::int64_t id, std::int32_t isin, const std::string& account) {
    using enum gen::FieldCode;
    return {
        .stream_code = gen::StreamCode::kFortsTradeRepl,
        .table_code = gen::TableCode::kFortsTradeReplOrdersLog,
        .revision = id,
        .fields = {integer(kFortsTradeReplOrdersLogReplId, id), integer(kFortsTradeReplOrdersLogPublicOrderId, id),
                   integer(kFortsTradeReplOrdersLogPrivateOrderId, id), integer(kFortsTradeReplOrdersLogSessId, 321),
                   integer(kFortsTradeReplOrdersLogIsinId, isin), integer(kFortsTradeReplOrdersLogDir, 1),
                   integer(kFortsTradeReplOrdersLogPublicAction, 1), integer(kFortsTradeReplOrdersLogPrivateAction, 1),
                   integer(kFortsTradeReplOrdersLogPublicAmount, 1), integer(kFortsTradeReplOrdersLogPrivateAmount, 1),
                   integer(kFortsTradeReplOrdersLogPublicAmountRest, 1),
                   integer(kFortsTradeReplOrdersLogPrivateAmountRest, 1),
                   text(kFortsTradeReplOrdersLogClientCode, account), text(kFortsTradeReplOrdersLogPrice, "103000")}};
}
inline fake::Event own_trade(std::int64_t deal, std::int32_t isin, const std::string& account,
                             std::int64_t order_id = 20003) {
    using enum gen::FieldCode;
    return {.stream_code = gen::StreamCode::kFortsTradeRepl,
            .table_code = gen::TableCode::kFortsTradeReplUserDeal,
            .revision = deal,
            .fields = {integer(kFortsTradeReplUserDealReplId, deal), integer(kFortsTradeReplUserDealIdDeal, deal),
                       integer(kFortsTradeReplUserDealSessId, 321), integer(kFortsTradeReplUserDealIsinId, isin),
                       integer(kFortsTradeReplUserDealPublicOrderIdBuy, order_id),
                       integer(kFortsTradeReplUserDealPrivateOrderIdBuy, order_id),
                       integer(kFortsTradeReplUserDealXamount, 1), text(kFortsTradeReplUserDealCodeBuy, account),
                       text(kFortsTradeReplUserDealCodeSell, "OTHER"), text(kFortsTradeReplUserDealPrice, "103000")}};
}
inline void bootstrap(CgateTradingHost& host) {
    const auto error = host.start();
    require(!error, "private-delta host start: " + error.message);
    for (int i = 0; i < 30; ++i) {
        const auto poll_error = host.poll();
        require(!poll_error, "private-delta host bootstrap: " + poll_error.message);
    }
    const auto status = host.status();
    require(status.find("\"reconstructing\":false") != std::string::npos &&
                status.find("\"order_entry_ready\":true") != std::string::npos,
            "private-delta fixture did not finish current-session reconstruction");
}
inline OrderRequest add(std::string key, std::int32_t isin) {
    return {.client_order_id = std::move(key), .isin_id = isin, .price = "103000", .quantity = 1};
}

inline void terminal_trade_stale_userbook(TradingHostConfig config, const fake::Control& control,
                                          const std::filesystem::path& root) {
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    for (const bool userbook_first : {false, true}) {
        for (const bool interleaved : {false, true}) {
            fake::Scenario scenario{
                .suppress_initial_orders = true, .zero_position = true, .client_code = account, .session_id = 321};
            scenario.options[static_cast<std::size_t>(fake::Option::DelayUserorderbook)] = "1";
            control.configure(scenario);
            const auto label = std::string("terminal-trade-") + (userbook_first ? "uob-first-" : "trade-first-") +
                               (interleaved ? "interleaved" : "sequential");
            config.journal_path = root / (label + ".ndjson");
            config.identity_state_path = root / (label + ".state");
            const auto posts = control.commands().size();
            CgateTradingHost host(config);
            require(!host.start(), "terminal TRADE fixture start");
            for (int i = 0; i < 30; ++i)
                require(!host.poll(), "terminal TRADE fixture initial streams");
            require(host.status().find("\"reconstructing\":true") != std::string::npos,
                    "terminal TRADE fixture failed to hold startup USERORDERBOOK barrier");
            constexpr std::int64_t id = 91001;
            auto terminal_order = own_order(id, isin, account);
            for (auto& field : terminal_order.fields) {
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
            auto deal = own_trade(92001, isin, account, id);
            for (auto& field : deal.fields)
                if (field.field_code == kFortsTradeReplUserDealXamount)
                    field.signed_value = 2;
            fake::Event stale_book{.stream_code = gen::StreamCode::kFortsUserorderbookRepl,
                                   .table_code = gen::TableCode::kFortsUserorderbookReplOrders,
                                   .revision = 93001,
                                   .fields = {integer(kFortsUserorderbookReplOrdersReplId, id),
                                              integer(kFortsUserorderbookReplOrdersPublicOrderId, id),
                                              integer(kFortsUserorderbookReplOrdersPrivateOrderId, id),
                                              integer(kFortsUserorderbookReplOrdersSessId, 321),
                                              integer(kFortsUserorderbookReplOrdersIsinId, isin),
                                              integer(kFortsUserorderbookReplOrdersDir, 1),
                                              integer(kFortsUserorderbookReplOrdersPublicAction, 1),
                                              integer(kFortsUserorderbookReplOrdersPrivateAction, 1),
                                              integer(kFortsUserorderbookReplOrdersPublicAmount, 2),
                                              integer(kFortsUserorderbookReplOrdersPrivateAmount, 2),
                                              integer(kFortsUserorderbookReplOrdersPublicAmountRest, 2),
                                              integer(kFortsUserorderbookReplOrdersPrivateAmountRest, 2),
                                              text(kFortsUserorderbookReplOrdersClientCode, account),
                                              text(kFortsUserorderbookReplOrdersPrice, "103000")}};
            const auto first =
                userbook_first ? gen::StreamCode::kFortsUserorderbookRepl : gen::StreamCode::kFortsTradeRepl;
            const auto second =
                userbook_first ? gen::StreamCode::kFortsTradeRepl : gen::StreamCode::kFortsUserorderbookRepl;
            const auto rows = [&](gen::StreamCode stream) {
                if (stream == gen::StreamCode::kFortsTradeRepl) {
                    control.enqueue(terminal_order);
                    control.enqueue(deal);
                } else
                    control.enqueue(stale_book);
            };
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = first});
            rows(first);
            if (!interleaved)
                control.enqueue({.kind = fake::EventKind::Commit, .stream_code = first});
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = second});
            rows(second);
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = second});
            if (interleaved)
                control.enqueue({.kind = fake::EventKind::Commit, .stream_code = first});
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsPosRepl});
            control.enqueue(
                {.stream_code = gen::StreamCode::kFortsPosRepl,
                 .table_code = gen::TableCode::kFortsPosReplPosition,
                 .revision = 94001,
                 .fields = {text(kFortsPosReplPositionClientCode, account), integer(kFortsPosReplPositionIsinId, isin),
                            integer(kFortsPosReplPositionAccountType, 2), integer(kFortsPosReplPositionXpos, 2)}});
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsPosRepl});
            control.enqueue({.kind = fake::EventKind::Online, .stream_code = gen::StreamCode::kFortsUserorderbookRepl});
            for (int i = 0; i < 5; ++i)
                require(!host.poll(), "terminal TRADE fixture reconstruction");
            const auto status = host.status();
            require(status.find("\"reconstructing\":false") != std::string::npos &&
                        status.find("\"xpos\":2") != std::string::npos,
                    "terminal TRADE reconstruction lost the independent filled position");
            require(status.find("\"state\":\"Working\"") == std::string::npos &&
                        status.find("\"state\":\"PartFilled\"") == std::string::npos &&
                        status.find("\"state\":\"Unknown\"") == std::string::npos &&
                        status.find("\"operator_action_required\":true") == std::string::npos,
                    "stale USERORDERBOOK resurrected a terminal TRADE order during reconstruction");
            std::ostringstream diagnostics;
            host.report_outstanding_orders(diagnostics);
            require(diagnostics.str().empty() && control.commands().size() == posts,
                    "terminal TRADE reconstruction manufactured an outstanding cancellation or send");
            require(!host.stop(), "terminal TRADE fixture stop");
        }
    }
}
} // namespace private_delta_host_detail

inline void private_delta_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                          const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    require(!config.isin_ids.empty(), "private-delta fixture needs an instrument");
    terminal_trade_stale_userbook(config, control, root);
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    config.utc_now = [] { return std::int64_t{1700000005}; };
    config.orders.risk.max_open_orders = 20000;
    config.orders.risk.max_notional_scaled = std::numeric_limits<std::int64_t>::max();
    config.orders.risk.max_notional_by_isin[isin] = std::numeric_limits<std::int64_t>::max();
    config.orders.risk.max_position_by_isin[isin] = 20000;
    fake::Scenario scenario{
        .suppress_initial_orders = true, .zero_position = true, .client_code = account, .session_id = 321};
    control.configure(scenario);
    {
        auto overflow = config;
        overflow.journal_path = root / "private-delta-overflow.ndjson";
        overflow.identity_state_path = root / "private-delta-overflow.state";
        CgateTradingHost host(overflow);
        bootstrap(host);
        constexpr std::int64_t first = 100000;
        constexpr auto count = plaza2::private_state::kPrivateRowChangeCapacity + 1;
        constexpr auto last = first + static_cast<std::int64_t>(count) - 1;
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        for (std::size_t i = 0; i < count; ++i)
            control.enqueue(own_order(first + static_cast<std::int64_t>(i), isin, account));
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        const auto start_calls = control.process_count();
        // Each call drains at most 100 native callbacks, including an unfinished
        // transaction. The full committed snapshot is needed after the final TN.
        for (int i = 0; i < 1000 && control.process_count() - start_calls < count + 2; ++i) {
            const auto error = host.poll();
            require(!error, "bounded private-delta overflow was treated as lost history: " + error.message);
        }
        const auto status = host.status();
        require(
            status.find("\"client_order_id\":\"recovered:321:" + std::to_string(first) + "\"") != std::string::npos &&
                status.find("\"client_order_id\":\"recovered:321:" + std::to_string(last) + "\"") != std::string::npos,
            "overflow reconciliation lost the first or last native working order");
        std::size_t recovered{}, position{};
        constexpr std::string_view marker = "\"client_order_id\":\"recovered:321:";
        while ((position = status.find(marker, position)) != std::string::npos) {
            ++recovered;
            position += marker.size();
        }
        require(recovered == count, "overflow reconciliation retained only part of the committed snapshot");
        require(status.find("\"order_entry_ready\":true") != std::string::npos,
                "lossless overflow did not restore order entry");
        const auto before = control.commands().size();
        require(host.place(add("after-lossless-overflow", isin)).empty(),
                "lossless overflow blocked a reconciled new Add");
        for (int i = 0; i < 3; ++i)
            require(!host.poll(), "Add after lossless overflow failed");
        require(control.commands().size() == before + 1,
                "Add after lossless overflow never reached the native publisher");
        require(!host.stop(), "overflow host stop failed");
    }
    // A committed update and a stream close can share one owner poll. Recover
    // using complete current snapshots while retaining the existing manager.
    for (const bool close : {true, false}) {
        auto recovery_scenario = scenario;
        recovery_scenario.suppress_auto_replies = true;
        control.configure(recovery_scenario);
        auto recovery = config;
        recovery.orders.max_commands_per_second = 1;
        recovery.journal_path = root / (close ? "private-close-resync.ndjson" : "private-clear-resync.ndjson");
        recovery.identity_state_path = root / (close ? "private-close-resync.state" : "private-clear-resync.state");
        CgateTradingHost host(recovery);
        bootstrap(host);
        const auto poll = [&] {
            const auto error = host.poll();
            require(!error, "ordinary private-stream resync killed the existing driver: " + error.message);
        };
        const auto purge_markers = [&] {
            for (const auto table :
                 {gen::TableCode::kFortsTradeReplOrdersLog, gen::TableCode::kFortsTradeReplMultilegOrdersLog,
                  gen::TableCode::kFortsTradeReplUserDeal, gen::TableCode::kFortsTradeReplUserMultilegDeal}) {
                const bool multileg = table == gen::TableCode::kFortsTradeReplMultilegOrdersLog ||
                                      table == gen::TableCode::kFortsTradeReplUserMultilegDeal;
                for (const auto revision : {multileg ? std::numeric_limits<std::int64_t>::max() : std::int64_t{1},
                                            multileg ? std::numeric_limits<std::int64_t>::max() : std::int64_t{2}})
                    control.enqueue({.kind = fake::EventKind::ClearDeleted,
                                     .stream_code = gen::StreamCode::kFortsTradeRepl,
                                     .table_code = table,
                                     .revision = revision,
                                     .flags = 8});
            }
        };
        // Native CGate repeats regular purge floors and multileg clear-all
        // markers at initial snapshot and reopen without retiring regular fills.
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        purge_markers();
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        poll();
        const auto reply = [&](std::uint32_t id, std::int64_t order_id = 20003) {
            official_cgate99::FORTS_MSG179 accepted{};
            accepted.order_id = order_id;
            std::vector<std::byte> payload(sizeof(accepted));
            std::memcpy(payload.data(), &accepted, payload.size());
            control.enqueue(
                {.kind = fake::EventKind::Reply, .message_id = 179, .user_id = id, .payload = std::move(payload)});
        };
        const auto order = [&](std::int64_t revision, std::int64_t remaining) {
            auto row = own_order(20003, isin, account);
            row.revision = revision;
            for (auto& field : row.fields) {
                if (field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPublicAmount ||
                    field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPrivateAmount)
                    field.signed_value = 3;
                if (field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPublicAmountRest ||
                    field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPrivateAmountRest)
                    field.signed_value = remaining;
            }
            control.enqueue(row);
        };
        auto seed = add("same-logical-order", isin);
        seed.quantity = 3;
        const auto seed_begin = control.commands().size();
        require(host.place(seed).empty(), "resync seed Add refused");
        poll();
        const auto original_post = control.commands().back();
        require(original_post.name == "AddOrder", "resync seed Add was not posted");
        reply(original_post.user_id);
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        order(20003, 3);
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        poll();
        std::optional<std::uint32_t> posted_before_loss;
        if (!close) {
            require(host.place(add("posted-before-history-loss", isin)).empty(), "pre-loss pending Add refused");
            now += std::chrono::seconds(1);
            poll();
            posted_before_loss = control.commands().back().user_id;
        }
        if (close) {
            auto second = add("second-working-order", isin);
            second.quantity = 4;
            require(host.place(second).empty(), "second reconnect Add refused");
            now += std::chrono::seconds(1);
            poll();
            reply(control.commands().back().user_id, 20005);
            auto second_row = own_order(20005, isin, account);
            for (auto& field : second_row.fields) {
                if (field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPublicAmount ||
                    field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPrivateAmount)
                    field.signed_value = 4;
                if (field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPublicAmountRest ||
                    field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPrivateAmountRest)
                    field.signed_value = 3;
            }
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
            control.enqueue(second_row);
            control.enqueue(own_trade(900010, isin, account, 20005));
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
            poll();
        }
        const auto before = control.commands().size();
        require(host.place(add("queued-during-resync", isin)).empty(), "resync queued Add refused");
        if (close)
            control.set(fake::Option::DelayUserorderbook);
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        order(900000, 2);
        control.enqueue(own_trade(900000, isin, account));
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue({.kind = close ? fake::EventKind::Close : fake::EventKind::ClearDeleted,
                         .stream_code = gen::StreamCode::kFortsTradeRepl,
                         .table_code = gen::TableCode::kFortsTradeReplUserDeal,
                         .revision = std::numeric_limits<std::int64_t>::max()});
        if (close)
            control.enqueue({.kind = fake::EventKind::Close, .stream_code = gen::StreamCode::kFortsUserorderbookRepl});
        else
            // CLEARDELETED outside a transaction is applied at ONLINE. Keep
            // that boundary in the same owner batch as the undrained commit.
            control.enqueue({.kind = fake::EventKind::Online, .stream_code = gen::StreamCode::kFortsTradeRepl});
        const auto calls = control.process_count();
        for (int i = 0; i < 10 && control.process_count() - calls < 6; ++i)
            poll();
        require(control.commands().size() == before, "queued Add dispatched before private resync");
        if (close) {
            const auto waiting = host.status();
            require(waiting.find("\"reconstructing\":true") != std::string::npos &&
                        waiting.find("\"order_entry_ready\":false") != std::string::npos,
                    "closed private streams did not wait for complete snapshot reconstruction");
            now += std::chrono::seconds(5);
            for (int i = 0; i < 10; ++i)
                poll();
            require(control.commands().size() == before && !host.place(add("before-current-snapshots", isin)).empty(),
                    "delayed USERORDERBOOK snapshot did not preserve the recovery barrier");
        }
        // The reopened source publishes the same working identity and deal.
        // Replaying this deal again must not increment its execution twice.
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        if (close)
            purge_markers();
        order(900001, 2);
        control.enqueue(own_trade(900000, isin, account));
        if (close) {
            auto second_row = own_order(20005, isin, account);
            second_row.revision = 900011;
            for (auto& field : second_row.fields) {
                if (field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPublicAmount ||
                    field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPrivateAmount)
                    field.signed_value = 4;
                if (field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPublicAmountRest ||
                    field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogPrivateAmountRest)
                    field.signed_value = 3;
            }
            control.enqueue(second_row);
            control.enqueue(own_trade(900010, isin, account, 20005));
        }
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue({.kind = fake::EventKind::Online, .stream_code = gen::StreamCode::kFortsTradeRepl});
        if (close)
            control.clear(fake::Option::DelayUserorderbook);
        now += std::chrono::seconds(5);
        for (int i = 0; i < 30; ++i)
            poll();
        const auto status = host.status();
        const auto logical = status.find("\"client_order_id\":\"same-logical-order\"");
        require(logical != std::string::npos, "private resync lost the original logical order");
        const auto original = status.substr(logical, status.find('}', logical) - logical + 1);
        require(original.find("\"order_id\":20003") != std::string::npos &&
                    original.find("\"remaining\":2") != std::string::npos &&
                    original.find("\"executed\":1") != std::string::npos &&
                    status.find("\"recovered:321:20003\"") == std::string::npos,
                "private resync changed the order identity or lost its partial fill");
        if (close) {
            require(host.move("same-logical-order", "103000", 5).empty(),
                    "plain TRADE reconnect refused the first tracked Move");
            require(host.move("second-working-order", "103000", 6).empty(),
                    "plain TRADE reconnect refused the second tracked Move");
        } else {
            require(host.move("same-logical-order", "103000", 3).find("baseline") != std::string::npos,
                    "retired TRADE history incorrectly allowed Move");
        }
        require(status.find("\"reconstructing\":false") != std::string::npos &&
                    status.find("\"order_entry_ready\":true") != std::string::npos,
                "complete private snapshots did not restore readiness in the same host");
        const auto resumed = control.commands();
        require(resumed.size() == before + 1 && resumed.back().name == "AddOrder",
                "queued Add did not resume once after private snapshot reconstruction");
        require(std::count_if(resumed.begin() + static_cast<std::ptrdiff_t>(seed_begin), resumed.end(),
                              [&](const auto& command) {
                                  return command.user_id == original_post.user_id && command.name == "AddOrder";
                              }) == 1,
                "private resync blindly resubmitted the original Add");
        reply(resumed.back().user_id, 20004);
        poll();
        require(host.move("queued-during-resync", "103000", 2).empty(),
                "Add posted after the barrier inherited old history loss");
        if (posted_before_loss) {
            reply(*posted_before_loss, 20006);
            poll();
            require(host.move("posted-before-history-loss", "103000", 2).find("baseline") != std::string::npos,
                    "late179 invented the pre-loss unresolved Add's retired fill history");
        }
        if (close) {
            for (int i = 0; i < 3; ++i) {
                now += std::chrono::seconds(1);
                poll();
            }
            std::map<std::int64_t, std::int32_t> moved;
            for (const auto& command : control.commands()) {
                if (command.name != "MoveOrder")
                    continue;
                require(command.payload.size() == sizeof(official_cgate99::MoveOrder), "reconnect Move payload size");
                official_cgate99::MoveOrder decoded{};
                std::memcpy(&decoded, command.payload.data(), command.payload.size());
                const std::int64_t order_id = decoded.order_id1;
                const std::int32_t amount = decoded.amount1;
                moved.emplace(order_id, amount);
            }
            require(moved.at(20003) == 5 && moved.at(20005) == 6,
                    "reconnected Moves changed established IDs or total amount1");
            const auto current = host.status();
            const auto second = current.find("\"client_order_id\":\"second-working-order\"");
            require(second != std::string::npos &&
                        current.substr(second, current.find('}', second) - second + 1).find("\"executed\":1") !=
                            std::string::npos,
                    "TRADE reopen counted the second order's replayed fill twice");
        }
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue(own_trade(900000, isin, account));
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        poll();
        const auto replay = host.status();
        const auto replay_begin = replay.find("\"client_order_id\":\"same-logical-order\"");
        require(replay.substr(replay_begin, replay.find('}', replay_begin) - replay_begin + 1).find("\"executed\":1") !=
                    std::string::npos,
                "private snapshot replay counted a fill twice");
        require(host.place(add("after-private-resync", isin)).empty(), "recovered driver refused new commands");
        require(host.cancel("same-logical-order").empty(), "private resync blocked cancellation");
        now += std::chrono::seconds(5);
        poll();
        require(control.commands().back().name == "DelOrder", "recovered cancellation never reached publisher");
        require(!host.stop(), "recovered private host stop failed");
        std::ifstream journal(recovery.journal_path);
        const std::string records{std::istreambuf_iterator<char>(journal), std::istreambuf_iterator<char>()};
        require(records.find("\"event\":\"private_history_gap\"") != std::string::npos,
                "private resync did not record its recovery gap");
    }
}
} // namespace moex::connector_host
