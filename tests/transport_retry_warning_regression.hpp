#pragma once

#include "moex/connector_host/order_manager.hpp"
#include "fixtures/cgate99_messages.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void transport_retry_warning_regression() {
    namespace tr = moex::plaza2_trade;
    namespace cg = moex::plaza2::cgate;
    const auto require = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    const auto request = [](std::string key) {
        return OrderRequest{.client_order_id = std::move(key), .isin_id = 42, .price = "100", .quantity = 3};
    };
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("retry-first")).empty() && manager.place(request("retry-second")).empty(),
            "transport warning seed Adds refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.at(0).id, {.msgid = 179, .order_id = 1201}, OrderManager::Clock::time_point{});
    manager.on_reply(f.sent.at(1).id, {.msgid = 179, .order_id = 1202}, OrderManager::Clock::time_point{});
    require(manager.cancel("retry-first").empty(), "transport warning first cancel refused");
    f.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    for (std::int64_t ms = 0; ms <= 400000; ms += 1000)
        f.poll(manager, ms);
    const auto warnings = [&](std::string_view key) {
        return std::count_if(f.log.begin(), f.log.end(), [&](const auto& line) {
            return line.starts_with("transport_retry{") &&
                   line.find("\"client_order_id\":\"" + std::string(key) + "\"") != std::string::npos;
        });
    };
    const auto audit = [&](std::string_view kind) {
        return std::count_if(f.log.begin(), f.log.end(), [&](const auto& line) { return line.starts_with(kind); });
    };
    const auto cancellations = std::count_if(f.sent.begin(), f.sent.end(), [](const auto& sent) {
        return sent.kind == tr::Plaza2TradeCommandKind::DelOrder;
    });
    require(cancellations == 7 && !manager.operator_action_required(),
            "transport timeouts exhausted cancellation business budget or changed retry pacing");
    require(warnings("retry-first") == 1 && audit("timeout{") == 6,
            "cancellation timeouts must remain audited while the operator warning occurs once");
    for (std::size_t i = 2; i + 1 < f.sent.size(); ++i)
        require(std::count(f.log.begin(), f.log.end(), "timeout{\"user_id\":" + std::to_string(f.sent[i].id) + "}") ==
                    1,
                "a timed-out cancellation lost its exact command correlation");
    require(audit("command{") == static_cast<std::ptrdiff_t>(f.sent.size()) &&
                audit("command_result{") == static_cast<std::ptrdiff_t>(f.sent.size()),
            "transport warning suppression lost an actual command/result audit record");
    require(manager.cancel("retry-second").empty(), "transport warning second cancel refused");
    f.poll(manager, 401000);
    f.poll(manager, 462000);
    require(warnings("retry-first") == 1 && warnings("retry-second") == 1,
            "another order did not receive its own first transport warning");
    f.poll(manager, 463000);
    const auto latest_first_cancel = [&] {
        for (auto it = f.sent.rbegin(); it != f.sent.rend(); ++it) {
            if (it->kind != tr::Plaza2TradeCommandKind::DelOrder)
                continue;
            official_cgate99::DelOrder payload{};
            std::memcpy(&payload, it->payload.data(), sizeof(payload));
            if (payload.order_id == 1201)
                return it->id;
        }
        throw std::runtime_error("transport warning first cancellation identity disappeared");
    };
    for (int rejected = 0; rejected < 3; ++rejected) {
        const auto now = f.ms;
        const auto before = f.sent.size();
        manager.on_reply(latest_first_cancel(), {.msgid = 177, .code = 17},
                         OrderManager::Clock::time_point{} + std::chrono::milliseconds(now));
        require(manager.operator_action_required() == (rejected == 2),
                "transport warning suppression changed the business rejection budget");
        f.poll(manager, now + (rejected == 0 ? 1000 : 2000));
        require(f.sent.size() == before + (rejected == 2 ? 0 : 1),
                "transport warning suppression changed bounded business retries");
    }
    require(warnings("retry-first") == 1 && warnings("retry-second") == 1 &&
                audit("command{") == static_cast<std::ptrdiff_t>(f.sent.size()) &&
                audit("command_result{") == static_cast<std::ptrdiff_t>(f.sent.size()) && audit("reply{") == 5,
            "retry warnings repeated or actual cancellation/send/reply audit records were lost");

    Fixture bulk;
    auto group = bulk.manager();
    require(group.cancel_all(42).empty(), "transport warning mass cancellation refused");
    bulk.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    for (std::int64_t ms = 0; ms <= 400000; ms += 1000)
        bulk.poll(group, ms);
    require(bulk.sent.size() == 3 && group.operator_action_required() &&
                std::count_if(bulk.log.begin(), bulk.log.end(),
                              [](const auto& line) { return line.starts_with("transport_retry{"); }) == 1 &&
                std::count_if(bulk.log.begin(), bulk.log.end(),
                              [](const auto& line) { return line.starts_with("timeout{"); }) == 3,
            "bounded mass cancellation lost its timeout audit or repeated its operator warning");

    Fixture carry;
    carry.config.reply_timeout = std::chrono::milliseconds(100);
    auto carried = carry.manager();
    require(carried.place(request("retry-relist")).empty(), "retry relist seed Add refused");
    carry.poll(carried, 0);
    carried.on_reply(carry.sent.front().id, {.msgid = 179, .order_id = 1301}, OrderManager::Clock::time_point{});
    require(carried.cancel("retry-relist").empty(), "retry relist cancellation refused");
    carry.poll(carried, 1);
    carried.on_timeout(carry.sent.back().id, OrderManager::Clock::time_point{} + std::chrono::milliseconds(101));
    plaza2::private_state::OwnOrderSnapshot old;
    old.public_order_id = old.private_order_id = 1301;
    old.sess_id = carry.session;
    old.isin_id = 42;
    old.client_code = carry.config.broker_code + carry.config.client_code;
    old.price = "100";
    old.public_amount = 3;
    old.ext_id = carried.orders().at("retry-relist").ext_id;
    old.dir = 1;
    old.from_trade_repl = true;
    carried.observe_orders(std::span(&old, 1));
    auto current = old;
    current.public_order_id = current.private_order_id = 1302;
    current.sess_id = ++carry.session;
    current.ext_id = 200;
    current.public_amount_rest = 1;
    current.public_action = 1;
    carried.observe_orders(std::span(&current, 1));
    require(!carried.orders().contains("retry-relist"), "retry warning fixture did not prune the terminal order");
    auto relist = old;
    relist.public_order_id = relist.private_order_id = 1303;
    relist.id_ord1 = 1301;
    relist.sess_id = carry.session;
    relist.public_amount_rest = 3;
    relist.public_action = 1;
    carried.observe_orders(std::span(&relist, 1));
    require(carried.orders().at("retry-relist").order_id == 1303,
            "retry warning fixture did not restore the same logical day relist");
    carry.poll(carried, 102);
    require(carry.sent.size() == 3 && carry.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
            "retry warning relisted cancellation was not posted");
    official_cgate99::DelOrder relisted_cancel{};
    std::memcpy(&relisted_cancel, carry.sent.back().payload.data(), sizeof(relisted_cancel));
    require(relisted_cancel.order_id == 1303, "retry warning relisted cancellation targeted the old native ID");
    carried.on_timeout(carry.sent.back().id, OrderManager::Clock::time_point{} + std::chrono::milliseconds(202));
    require(carried.queued() == 1 &&
                std::count_if(carry.log.begin(), carry.log.end(),
                              [](const auto& line) { return line.starts_with("transport_retry{"); }) == 1,
            "terminal pruning and day relist repeated the logical order's transport warning");

    for (const bool move : {false, true}) {
        Fixture diagnostic;
        auto owner = diagnostic.manager();
        require(owner.place(request("timeout-diagnostic")).empty(), "timeout diagnostic seed Add refused");
        diagnostic.poll(owner, 0);
        if (move) {
            owner.on_reply(diagnostic.sent.front().id, {.msgid = 179, .order_id = 1203},
                           OrderManager::Clock::time_point{});
            require(owner.move("timeout-diagnostic", "101", 3).empty(), "timeout diagnostic Move refused");
            diagnostic.poll(owner, 0);
        }
        owner.on_timeout(diagnostic.sent.back().id, OrderManager::Clock::time_point{} + std::chrono::seconds(60));
        require(std::count_if(diagnostic.log.begin(), diagnostic.log.end(),
                              [](const auto& line) { return line.starts_with("timeout{"); }) == 1 &&
                    std::none_of(diagnostic.log.begin(), diagnostic.log.end(),
                                 [](const auto& line) { return line.starts_with("transport_retry{"); }),
                "cancellation warning suppression changed Add/Move timeout diagnostics");
    }
}
} // namespace moex::connector_host
