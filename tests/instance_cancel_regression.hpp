#pragma once

#include "fixtures/cgate99_messages.hpp"
#include "moex/connector_host/order_manager.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void instance_cancel_regression() {
    namespace ps = plaza2::private_state;
    namespace tr = plaza2_trade;
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    check(!OrderManagerConfig{}.sole_instance, "instrument-wide cancellation was enabled by default");
    Fixture fixture;
    fixture.config.max_commands_per_second = 30;
    fixture.config.reply_timeout = std::chrono::milliseconds(100);
    fixture.config.ext_id_begin = 100;
    fixture.config.ext_id_end = 109;
    fixture.config.ext_id_range_configured = true;
    auto manager = fixture.manager();
    for (const auto key : {"known", "unresolved", "conflict"})
        check(manager.place({.client_order_id = key, .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "instance cancellation Add refused");
    fixture.poll(manager, 0);
    const auto unresolved_add = fixture.sent[1].id;
    const auto conflict_add = fixture.sent[2].id;
    manager.on_reply(fixture.sent[0].id, {.msgid = 179, .order_id = 99001}, OrderManager::Clock::time_point{});
    check(manager.place({.client_order_id = "unsent", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
          "unsent instance cancellation fixture refused");
    const auto now = OrderManager::Clock::time_point{} + std::chrono::milliseconds(100);
    manager.on_timeout(unresolved_add, now);
    manager.on_timeout(conflict_add, now);
    const auto native = [](std::int64_t id, std::int32_t ext, std::string login, std::int32_t isin = 42) {
        ps::OwnOrderSnapshot row;
        row.public_order_id = row.private_order_id = id;
        row.sess_id = 100;
        row.isin_id = isin;
        row.client_code = "ABCD001";
        row.login_from = std::move(login);
        row.ext_id = ext;
        row.price = "100";
        row.public_amount = row.public_amount_rest = 2;
        row.public_action = row.dir = 1;
        row.from_trade_repl = true;
        return row;
    };
    const std::array rows{native(99005, 105, "owner-login"),
                          native(99006, 106, "another-instance"),
                          native(99007, 700, "owner-login"),
                          native(99008, 0, ""),
                          native(99009, manager.orders().at("conflict").ext_id, "another-instance"),
                          native(99010, 107, "owner-login", 43)};
    manager.observe_orders(rows, true);
    check(manager.orders().at("known").instance_owned && manager.orders().at("unresolved").instance_owned &&
              manager.orders().at("recovered:100:99005").instance_owned &&
              !manager.orders().at("recovered:100:99006").instance_owned &&
              !manager.orders().at("recovered:100:99007").instance_owned &&
              !manager.orders().at("recovered:100:99008").instance_owned,
          "native instance ownership did not require the configured login and ext-ID range");
    check(manager.cancel_all(42).empty(), "instance cancel-all refused");
    fixture.poll(manager, 101);
    bool known_cancelled{}, recovered_cancelled{}, unresolved_scoped{};
    for (const auto& sent : fixture.sent) {
        if (sent.kind == tr::Plaza2TradeCommandKind::DelUserOrders) {
            official_cgate99::DelUserOrders wire{};
            std::memcpy(&wire, sent.payload.data(), sizeof(wire));
            check(wire.ext_id == manager.orders().at("unresolved").ext_id && wire.isin_id == 42,
                  "default cancel-all sent an unowned or instrument-wide DUO");
            unresolved_scoped = true;
        } else if (sent.kind == tr::Plaza2TradeCommandKind::DelOrder) {
            official_cgate99::DelOrder wire{};
            std::memcpy(&wire, sent.payload.data(), sizeof(wire));
            check(wire.order_id == 99001 || wire.order_id == 99005,
                  "default cancel-all targeted another instance, manual order or other instrument");
            known_cancelled |= wire.order_id == 99001;
            recovered_cancelled |= wire.order_id == 99005;
        }
    }
    check(known_cancelled && recovered_cancelled && unresolved_scoped,
          "instance cancel-all omitted known, recovered or unresolved own risk");
    check(manager.orders().at("unsent").state == OrderState::Cancelled &&
              std::count_if(fixture.sent.begin(), fixture.sent.end(),
                            [](const auto& sent) { return sent.kind == tr::Plaza2TradeCommandKind::AddOrder; }) == 3,
          "instance cancel-all posted an unsent Add instead of retiring it");
    check(manager.orders().at("conflict").operator_action_required &&
              !manager.orders().at("recovered:100:99006").cancel_requested &&
              !manager.orders().at("recovered:100:99007").cancel_requested &&
              !manager.orders().at("recovered:100:99008").cancel_requested,
          "instance cancel-all overrode an ownership conflict or foreign order");
    manager.on_reply(unresolved_add, {.msgid = 179, .order_id = 99002}, now);
    fixture.poll(manager, 102);
    check(std::any_of(fixture.sent.begin(), fixture.sent.end(),
                      [](const auto& sent) {
                          if (sent.kind != tr::Plaza2TradeCommandKind::DelOrder)
                              return false;
                          official_cgate99::DelOrder wire{};
                          std::memcpy(&wire, sent.payload.data(), sizeof(wire));
                          return wire.order_id == 99002;
                      }),
          "instance cancellation discarded unresolved Add correlation");

    // An explicit day link restores the same logical ownership after the old
    // terminal row has been compacted; a foreign ancestor stays foreign.
    Fixture retention;
    retention.config.ext_id_begin = 100;
    retention.config.ext_id_end = 109;
    retention.config.ext_id_range_configured = true;
    auto relisted = retention.manager();
    std::array ancestors{native(99100, 104, "owner-login"), native(99101, 105, "another-instance")};
    relisted.observe_orders(ancestors, true);
    for (auto& row : ancestors) {
        row.public_amount_rest = 0;
        row.public_action = 0;
    }
    relisted.observe_orders(ancestors);
    auto next_session = native(99102, 106, "owner-login", 43);
    next_session.sess_id = 101;
    relisted.observe_orders(std::span(&next_session, 1));
    check(!relisted.orders().contains("recovered:100:99100"), "terminal ownership fixture did not compact");
    std::array children{native(99103, 0, ""), native(99104, 0, "")};
    for (std::size_t i = 0; i < children.size(); ++i) {
        children[i].id_ord1 = ancestors[i].public_order_id;
        children[i].sess_id = 102;
    }
    relisted.observe_orders(children);
    check(relisted.orders().at("recovered:100:99100").instance_owned &&
              !relisted.orders().at("recovered:100:99101").instance_owned,
          "day relist changed archived logical instance ownership");
    check(relisted.cancel_all(42).empty(), "relisted instance cancellation refused");
    retention.poll(relisted, 0);
    check(retention.sent.size() == 1 && retention.sent.front().kind == tr::Plaza2TradeCommandKind::DelOrder,
          "day relist cancelled the foreign ancestor's descendant");
    official_cgate99::DelOrder relist_cancel{};
    std::memcpy(&relist_cancel, retention.sent.front().payload.data(), sizeof(relist_cancel));
    check(relist_cancel.order_id == 99103, "day relist did not cancel this instance's proven descendant");

    // The publisher's exact179 ties a previously unowned ext0 native row to
    // this instance's submitted command without claiming other recovered rows.
    Fixture authority;
    auto confirmed = authority.manager();
    check(confirmed.place({.client_order_id = "official", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
          "official instance ownership Add refused");
    authority.poll(confirmed, 0);
    const auto official = native(99105, 0, "");
    confirmed.observe_orders(std::span(&official, 1));
    check(!confirmed.orders().at("recovered:100:99105").instance_owned, "ext0 row claimed instance ownership");
    confirmed.on_reply(authority.sent.front().id, {.msgid = 179, .order_id = 99105}, OrderManager::Clock::time_point{});
    check(confirmed.orders().at("official").instance_owned && !confirmed.orders().contains("recovered:100:99105"),
          "official179 adoption lost submitted instance ownership");
    check(confirmed.cancel_all(42).empty(), "officially adopted instance cancellation refused");
    authority.poll(confirmed, 1);
    check(authority.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
          "official179 adopted order did not receive a scoped cancellation");

    // A full reload can lose another instance's row. Its conservative risk
    // remains visible, but that absence does not authorize a cancellation.
    for (const bool sole : {false, true}) {
        Fixture reload;
        reload.config.sole_instance = sole;
        reload.config.ext_id_begin = 100;
        reload.config.ext_id_end = 109;
        reload.config.ext_id_range_configured = true;
        auto missing = reload.manager();
        const auto unowned = native(99106, 700, "another-instance");
        const std::string key = "recovered:100:99106";
        missing.observe_orders(std::span(&unowned, 1), true);
        check(!missing.orders().at(key).instance_owned, "foreign reload row claimed instance ownership");
        missing.reconcile_snapshot({}, 1700000000);
        for (int i = 0; i < 3; ++i) {
            missing.prove_absence(1700000061 + i, true);
            reload.poll(missing, 61000 + 1000 * i);
        }
        if (sole) {
            check(reload.sent.size() == 1 && reload.sent.front().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
                  "declared sole-instance reload recovery lost its cancellation authority");
            official_cgate99::DelUserOrders wire{};
            std::memcpy(&wire, reload.sent.front().payload.data(), sizeof(wire));
            check(wire.ext_id == unowned.ext_id, "sole-instance missing-order recovery targeted another ext ID");
            continue;
        }
        const auto& unresolved = missing.orders().at(key);
        check(reload.sent.empty(), "missing other-instance order caused an unrequested recovery cancellation");
        check(unresolved.state == OrderState::Unknown && unresolved.operator_action_required &&
                  unresolved.snapshot_missing && unresolved.remaining == 2 && !unresolved.cancel_requested,
              "uncancelled foreign reload exposure lost its warning or conservative remaining quantity");
        check(missing.cancel(key).empty(), "explicit tracked foreign-ID cancellation was prohibited");
        reload.poll(missing, 64000);
        check(reload.sent.size() == 1 && reload.sent.front().kind == tr::Plaza2TradeCommandKind::DelOrder,
              "explicit foreign-ID cancellation failed to remain exact");
        official_cgate99::DelOrder wire{};
        std::memcpy(&wire, reload.sent.front().payload.data(), sizeof(wire));
        check(wire.order_id == unowned.public_order_id, "explicit tracked cancel targeted another native ID");
    }
}
} // namespace moex::connector_host
