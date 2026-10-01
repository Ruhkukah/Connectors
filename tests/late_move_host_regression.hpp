#pragma once

#include "private_delta_host_regression.hpp"
#include "fixtures/cgate99_messages.hpp"

#include <cstring>

namespace moex::connector_host {
namespace late_move_host_detail {
namespace fake = plaza2::test::fake;
namespace gen = plaza2::generated;
using plaza2::test::require;

template <class T> inline void reply(const fake::Control& control, std::uint32_t user_id, int msgid, const T& wire) {
    std::vector<std::byte> payload(sizeof(wire));
    std::memcpy(payload.data(), &wire, payload.size());
    control.enqueue(
        {.kind = fake::EventKind::Reply, .message_id = msgid, .user_id = user_id, .payload = std::move(payload)});
}
inline void order(const fake::Control& control, std::int64_t id, std::int64_t revision, std::int64_t total,
                  std::int64_t remaining, std::int32_t isin, const std::string& account) {
    using enum gen::FieldCode;
    auto row = private_delta_host_detail::own_order(id, isin, account);
    row.revision = revision;
    for (auto& field : row.fields) {
        if (field.field_code == kFortsTradeReplOrdersLogPublicAmount ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAmount)
            field.signed_value = total;
        if (field.field_code == kFortsTradeReplOrdersLogPublicAmountRest ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAmountRest)
            field.signed_value = remaining;
        if (field.field_code == kFortsTradeReplOrdersLogPublicAction ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAction)
            field.signed_value = remaining ? 1 : 0;
    }
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(row);
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
}
inline std::string logical_order(const CgateTradingHost& host, std::string_view key) {
    const auto status = host.status();
    const auto begin = status.find("\"client_order_id\":\"" + std::string(key) + "\"");
    require(begin != std::string::npos, "native late-Move logical order missing");
    return status.substr(begin, status.find('}', begin) - begin + 1);
}
} // namespace late_move_host_detail

inline void late_move_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                      const std::filesystem::path& root) {
    using namespace late_move_host_detail;
    require(!config.isin_ids.empty(), "native late-Move fixture needs an instrument");
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    config.session.reply_timeout_ms = 10;
    config.orders.reply_timeout = std::chrono::milliseconds(10);
    config.orders.risk.max_notional_scaled = 60'000'000'000LL;
    config.orders.risk.max_open_orders = 100;
    config.utc_now = [] { return std::int64_t{1700000005}; };
    config.journal_path = root / "native-late-move.ndjson";
    config.identity_state_path = root / "native-late-move.state";
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = account,
                       .session_id = 321});
    CgateTradingHost host(config);
    private_delta_host_detail::bootstrap(host);
    const auto poll = [&] {
        const auto error = host.poll();
        require(!error, "native late-Move owner poll: " + error.message);
    };
    const auto begin = control.commands().size();
    require(host.place({.client_order_id = "late-move", .isin_id = isin, .price = "103000", .quantity = 3}).empty(),
            "native late-Move seed Add refused");
    poll();
    auto commands = control.commands();
    require(commands.size() == begin + 1 && commands.back().name == "AddOrder", "native late-Move seed post");
    official_cgate99::FORTS_MSG179 accepted{};
    accepted.order_id = 63001;
    reply(control, commands.back().user_id, 179, accepted);
    order(control, 63001, 63001, 3, 3, isin, account);
    poll();
    require(host.move("late-move", "103250", 5).empty(), "native late-Move request refused");
    poll();
    commands = control.commands();
    require(commands.back().name == "MoveOrder", "native late-Move command not published");
    const auto move_user_id = commands.back().user_id;
    now += std::chrono::milliseconds(20);
    poll(); // Expire both Session tracking and the owner's Move timer.
    order(control, 63001, 65000, 3, 0, isin, account);
    poll();
    const auto uncertain = logical_order(host, "late-move");
    require(uncertain.find("\"state\":\"Unknown\"") != std::string::npos,
            "native old-ID deletion prematurely settled a timed-out replacement");
    require(!host.place({.client_order_id = "before-late-proof", .isin_id = isin, .price = "103000", .quantity = 1})
                 .empty(),
            "native old-ID deletion released the uncertain replacement exposure");
    official_cgate99::FORTS_MSG176 late{};
    late.order_id1 = 63002;
    reply(control, move_user_id, 176, late);
    poll();
    const auto identified = logical_order(host, "late-move");
    require(identified.find("\"order_id\":63002") != std::string::npos,
            "Session discarded176 after its timer expired before the manager received it");
    require(
        !host.place({.client_order_id = "before-native-terminal", .isin_id = isin, .price = "103000", .quantity = 1})
             .empty(),
        "late176 released risk before replacement terminal replication");
    commands = control.commands();
    std::size_t moves{};
    bool replacement_cancel{};
    for (std::size_t i = begin; i < commands.size(); ++i) {
        moves += commands[i].name == "MoveOrder";
        if (commands[i].name == "DelOrder" && commands[i].payload.size() == sizeof(official_cgate99::DelOrder)) {
            official_cgate99::DelOrder cancel{};
            std::memcpy(&cancel, commands[i].payload.data(), sizeof(cancel));
            replacement_cancel |= cancel.order_id == 63002;
        }
    }
    require(moves == 1 && replacement_cancel, "late176 resubmitted Move or failed to target the replacement cancel");
    order(control, 63002, 65001, 5, 0, isin, account);
    poll();
    require(logical_order(host, "late-move").find("\"state\":\"Cancelled\"") != std::string::npos,
            "native replacement termination did not settle the logical order");
    require(host.place({.client_order_id = "after-native-terminal", .isin_id = isin, .price = "103000", .quantity = 1})
                .empty(),
            "native replacement termination retained the uncertain exposure reservation");
    poll();
    require(control.commands().back().name == "AddOrder", "reconciled native replacement never allowed a new Add");
    require(!host.stop(), "native late-Move host stop failed");
}
} // namespace moex::connector_host
