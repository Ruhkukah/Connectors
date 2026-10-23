#pragma once
#include "moex/connector_host/order_manager.hpp"
#include "fixtures/cgate99_messages.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void manual_entry_regression() {
    const auto require = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    Fixture reserved;
    auto guarded = reserved.manager();
    const auto error =
        guarded.place({.client_order_id = "recovered:operator-spoof", .isin_id = 42, .price = "100", .quantity = 2});
    reserved.poll(guarded, 0);
    const bool prefix_refused = !error.empty() && reserved.sent.empty() && guarded.queued() == 0 &&
                                guarded.orders().empty() &&
                                std::none_of(reserved.log.begin(), reserved.log.end(), [](const auto& value) {
                                    return value.starts_with("reservation{") || value.starts_with("command{");
                                });

    Fixture manual;
    auto entry = manual.manager();
    require(entry.place({.client_order_id = "manual-entry", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
            "ordinary manual Add refused");
    manual.poll(entry, 0);
    require(manual.sent.size() == 1 && manual.sent.back().payload.size() == sizeof(official_cgate99::AddOrder),
            "manual Add did not reach actual encoded transport");
    official_cgate99::AddOrder add{};
    std::memcpy(&add, manual.sent.back().payload.data(), sizeof(add));
    const bool add_manual = add.compliance_id[0] == 'M' && add.compliance_id[1] == 0;
    entry.on_reply(manual.sent.back().id, {.msgid = 179, .order_id = 98201}, OrderManager::Clock::time_point{});
    require(entry.move("manual-entry", "101", 2).empty(), "ordinary manual Move refused");
    manual.poll(entry, 1000);
    require(manual.sent.size() == 2 && manual.sent.back().payload.size() == sizeof(official_cgate99::MoveOrder),
            "manual Move did not reach actual encoded transport");
    official_cgate99::MoveOrder move{};
    std::memcpy(&move, manual.sent.back().payload.data(), sizeof(move));
    const bool move_manual = move.compliance_id[0] == 'M' && move.compliance_id[1] == 0;

    Fixture native;
    auto recovered = native.manager();
    plaza2::private_state::OwnOrderSnapshot row;
    row.public_order_id = row.private_order_id = 98202;
    row.sess_id = 100;
    row.isin_id = 42;
    row.client_code = "ABCD001";
    row.login_from = "owner-login";
    row.dir = 1;
    row.price = "100";
    row.public_amount = row.public_amount_rest = 2;
    row.public_action = 1;
    row.from_trade_repl = true;
    recovered.observe_orders(std::span(&row, 1), true);
    require(recovered.orders().contains("recovered:100:98202") && recovered.cancel("recovered:100:98202").empty(),
            "reserved public prefix blocked legitimate internal native recovery/cancellation");
    native.poll(recovered, 0);
    require(native.sent.size() == 1 && native.sent.back().payload.size() == sizeof(official_cgate99::DelOrder),
            "internally recovered order cancellation did not reach transport");
    official_cgate99::DelOrder cancel{};
    std::memcpy(&cancel, native.sent.back().payload.data(), sizeof(cancel));
    require(cancel.order_id == 98202, "internal recovery cancellation changed the exact native identity");
    if (!prefix_refused || !add_manual || !move_manual)
        throw std::runtime_error("manual-entry safeguards: reserved_prefix=" + std::to_string(prefix_refused) +
                                 " Add_M=" + std::to_string(add_manual) + " Move_M=" + std::to_string(move_manual));
}
} // namespace moex::connector_host
