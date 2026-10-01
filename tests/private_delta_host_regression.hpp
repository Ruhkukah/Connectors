#pragma once

#include "moex/connector_host/trading_host.hpp"
#include "fake_cgate_control.hpp"
#include "plaza2_runtime_test_support.hpp"

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
inline fake::Event own_trade(std::int64_t deal, std::int32_t isin, const std::string& account) {
    using enum gen::FieldCode;
    return {.stream_code = gen::StreamCode::kFortsTradeRepl,
            .table_code = gen::TableCode::kFortsTradeReplUserDeal,
            .revision = deal,
            .fields = {integer(kFortsTradeReplUserDealReplId, deal), integer(kFortsTradeReplUserDealIdDeal, deal),
                       integer(kFortsTradeReplUserDealSessId, 321), integer(kFortsTradeReplUserDealIsinId, isin),
                       integer(kFortsTradeReplUserDealPublicOrderIdBuy, 20003),
                       integer(kFortsTradeReplUserDealPrivateOrderIdBuy, 20003),
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
} // namespace private_delta_host_detail

inline void private_delta_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                          const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    require(!config.isin_ids.empty(), "private-delta fixture needs an instrument");
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    config.utc_now = [] { return std::int64_t{1700000005}; };
    config.orders.risk.max_open_orders = 20000;
    config.orders.risk.max_notional_scaled = std::numeric_limits<std::int64_t>::max();
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
        require(status.find("\"private_state_error\":\"\"") != std::string::npos &&
                    status.find("\"order_entry_ready\":true") != std::string::npos,
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
    // Exercise both table reclamation and a listener close after a committed
    // private fill, before the owner has observed that fill.
    for (const bool close : {false, true}) {
        control.configure(scenario);
        auto gap = config;
        gap.orders.max_commands_per_second = 1;
        gap.journal_path = root / (close ? "private-close-gap.ndjson" : "private-clear-gap.ndjson");
        gap.identity_state_path = root / (close ? "private-close-gap.state" : "private-clear-gap.state");
        CgateTradingHost host(gap);
        bootstrap(host);
        require(host.place(add("rate-window-seed", isin)).empty(), "history-gap seed Add refused");
        for (int i = 0; i < 3; ++i)
            require(!host.poll(), "history-gap seed Add failed");
        const auto before = control.commands().size();
        require(host.place(add("queued-before-history-gap", isin)).empty(), "history-gap queued Add refused");
        cg::Plaza2Error error;
        // A scheduling pause may split a bounded poll just after the commit.
        // Repeat a fresh small transaction if that fill was already drained.
        for (int attempt = 0; attempt < 10 && !error; ++attempt) {
            const auto deal_id = 900000 + attempt;
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
            control.enqueue(own_trade(deal_id, isin, account));
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
            control.enqueue({.kind = close ? fake::EventKind::Close : fake::EventKind::ClearDeleted,
                             .stream_code = gen::StreamCode::kFortsTradeRepl,
                             .table_code = gen::TableCode::kFortsTradeReplUserDeal,
                             .revision = std::numeric_limits<std::int64_t>::max()});
            if (!close)
                control.enqueue({.kind = fake::EventKind::Online, .stream_code = gen::StreamCode::kFortsTradeRepl});
            const auto calls = control.process_count();
            const auto callbacks = close ? 4u : 5u;
            for (int i = 0; i < 10 && !error && control.process_count() - calls < callbacks; ++i)
                error = host.poll();
            if (close && !error)
                now += std::chrono::seconds(1);
        }
        require(error.code == cg::Plaza2ErrorCode::RuntimeCallFailed &&
                    error.message.find("private-state delta history lost") != std::string::npos,
                close ? "undrained committed fill was silently discarded on listener close"
                      : "undrained committed fill was silently discarded by ClearDeleted");
        require(control.commands().size() == before, "queued Add dispatched while private history was lost");
        const auto status = host.status();
        require(status.find("\"operator_action_required\":true") != std::string::npos &&
                    status.find("\"order_entry_ready\":false") != std::string::npos,
                "private history gap did not expose an operator reconciliation requirement");
        now += std::chrono::seconds(5); // The limiter is now eligible; the history latch must still stop sends.
        host.set_kill_switch(false);
        require(!host.place(add("after-history-gap", isin)).empty(), "history gap accepted another new Add");
        const auto calls = control.process_count();
        for (int i = 0; i < 3; ++i) {
            const auto later = host.poll();
            require(later.code == cg::Plaza2ErrorCode::RuntimeCallFailed && later.message == error.message,
                    "private history gap was cleared by a later owner poll");
        }
        require(control.process_count() == calls && control.commands().size() == before,
                "latched history gap resumed gateway processing or queued sends");
        require(!host.stop(), "history-gap host stop failed");
    }
}
} // namespace moex::connector_host
