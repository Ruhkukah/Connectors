#include "moex/connector_host/order_manager.hpp"
#include "moex/connector_host/event_journal.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace moex::connector_host {
namespace {
namespace tr = plaza2_trade;
namespace cg = plaza2::cgate;
using Kind = tr::Plaza2TradeCommandKind;
std::uint64_t milliseconds(OrderManager::Clock::time_point now) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}
std::string order_json(const std::string& key, const ManagedOrder& order) {
    return "{\"client_order_id\":" + json_string(key) + ",\"state\":" + json_string(order_state_name(order.state)) +
           ",\"isin_id\":" + std::to_string(order.request.isin_id) + ",\"sess_id\":" + std::to_string(order.sess_id) +
           ",\"ext_id\":" + std::to_string(order.ext_id) + ",\"order_id\":" + std::to_string(order.order_id) +
           ",\"quantity\":" + std::to_string(order.request.quantity) +
           ",\"remaining\":" + std::to_string(order.remaining) + ",\"executed\":" + std::to_string(order.executed) +
           ",\"operator_action_required\":" + (order.operator_action_required ? "true" : "false") +
           ",\"price\":" + json_string(order.request.price) + ",\"error\":" + json_string(order.last_error) + "}";
}
std::uint64_t absolute_units(std::int64_t units) {
    return units < 0 ? std::uint64_t(-(units + 1)) + 1 : std::uint64_t(units);
}
std::uint64_t capped_notional(std::uint64_t units, std::int64_t quantity, std::uint64_t cap) {
    if (quantity <= 0)
        return 0;
    if (units > cap / static_cast<std::uint64_t>(quantity))
        return cap + 1;
    return units * static_cast<std::uint64_t>(quantity);
}
OrderState settled_state(const ManagedOrder& order) {
    if (order.remaining <= 0)
        return order.executed >= order.request.quantity ? OrderState::Filled : OrderState::Cancelled;
    return order.executed > 0 ? OrderState::PartFilled : OrderState::Working;
}
} // namespace

std::string_view order_state_name(OrderState state) noexcept {
    switch (state) {
    case OrderState::PendingNew:
        return "PendingNew";
    case OrderState::Working:
        return "Working";
    case OrderState::PartFilled:
        return "PartFilled";
    case OrderState::PendingCancel:
        return "PendingCancel";
    case OrderState::PendingReplace:
        return "PendingReplace";
    case OrderState::Filled:
        return "Filled";
    case OrderState::Cancelled:
        return "Cancelled";
    case OrderState::Rejected:
        return "Rejected";
    case OrderState::Unknown:
        return "Unknown";
    }
    return "Unknown";
}
bool terminal(OrderState state) noexcept {
    return state == OrderState::Filled || state == OrderState::Cancelled || state == OrderState::Rejected;
}

OrderManager::OrderManager(OrderManagerConfig config, Send send, Ready ready, Terms terms, Log log)
    : config_(std::move(config)), send_(std::move(send)), ready_(std::move(ready)), terms_(std::move(terms)),
      log_(std::move(log)), rate_(config_.max_commands_per_second) {
    if (!rate_.valid() || !send_ || !ready_ || !terms_ || config_.reply_timeout.count() <= 0 ||
        config_.absence_margin.count() < 0 || config_.risk.max_quantity <= 0 || config_.risk.max_notional_scaled <= 0 ||
        config_.risk.max_open_orders == 0 || config_.next_ext_id <= 0 || config_.next_user_id == 0 ||
        config_.max_cancel_attempts == 0 || config_.cancel_retry_base.count() <= 0 ||
        config_.cancel_retry_max < config_.cancel_retry_base)
        throw std::invalid_argument("invalid order manager configuration");
}
void OrderManager::emit(std::string_view kind, std::string_view fields) noexcept {
    try {
        if (log_)
            log_(kind, fields);
    } catch (...) {
        logging_failed_ = true;
        config_.risk.kill_switch = true;
    }
}
OrderManager::Exposure OrderManager::exposure(const std::string& key, const ManagedOrder& order) const {
    Exposure result;
    result.unknown = order.state == OrderState::Unknown;
    result.operator_action = order.operator_action_required;
    if (terminal(order.state)) {
        // A late authoritative179 can still revive an absence-resolved Add.
        // Keep its submitted identity/correlation through session changes.
        result.terminal_session = unconfirmed_adds_.contains(key) ? 0 : order.sess_id;
        return result;
    }
    result.active = true;
    const auto price = plaza2::private_state::parse_session_decimal(order.request.price);
    result.invalid_price = !price;
    auto units = price ? absolute_units(price->units) : 0;
    auto remaining = result.unknown ? std::max(order.remaining, std::int64_t(order.request.quantity) - order.executed)
                                    : order.remaining;
    if (const auto move = move_reservations_.find(key); move != move_reservations_.end()) {
        units = std::max(units, move->second.price_units);
        remaining = std::max(remaining, std::int64_t(move->second.quantity) - order.executed);
    }
    result.notional = capped_notional(units, remaining, config_.risk.max_notional_scaled);
    return result;
}
void OrderManager::erase_exposure(const std::string& key) {
    const auto cached = exposures_.find(key);
    if (cached == exposures_.end())
        return;
    const auto& old = cached->second;
    subtract_charge(old);
    unknown_orders_.erase(key);
    operator_orders_.erase(key);
    if (const auto terminal = terminal_orders_.find(old.terminal_session); terminal != terminal_orders_.end()) {
        terminal->second.erase(key);
        if (terminal->second.empty())
            terminal_orders_.erase(terminal);
    }
    exposures_.erase(cached);
}
void OrderManager::refresh_exposure(const std::string& key) {
    erase_exposure(key);
    const auto& order = orders_.at(key);
    if (terminal(order.state))
        move_reservations_.erase(key);
    const auto next = exposure(key, order);
    exposures_.emplace(key, next);
    add_charge(next);
    if (next.unknown)
        unknown_orders_.insert(key);
    if (next.operator_action)
        operator_orders_.insert(key);
    if (next.terminal_session > 0)
        terminal_orders_[next.terminal_session].insert(key);
}
void OrderManager::add_charge(const Exposure& charge) {
    active_orders_ += charge.active;
    invalid_prices_ += charge.invalid_price;
    const auto old_low = notional_low_;
    notional_low_ += charge.notional;
    notional_high_ += notional_low_ < old_low;
}
void OrderManager::subtract_charge(const Exposure& charge) {
    active_orders_ -= charge.active;
    invalid_prices_ -= charge.invalid_price;
    if (notional_low_ < charge.notional)
        --notional_high_;
    notional_low_ -= charge.notional;
}
bool OrderManager::hold_add_row(const plaza2::private_state::OwnOrderSnapshot& row) {
    if (replaying_add_rows_)
        return false;
    const auto id = row.private_order_id > 0 ? row.private_order_id : row.public_order_id;
    if (order_index_.contains(id))
        return false; // Exact tracked identity outranks every provisional ext_id grouping.
    if (const auto ancestor = order_index_.find(row.id_ord1); ancestor != order_index_.end()) {
        const auto& prior = orders_.at(ancestor->second);
        if (row.sess_id > prior.sess_id && row.isin_id == prior.request.isin_id &&
            row.dir == static_cast<std::int8_t>(prior.request.side))
            return false;
    }
    std::string key;
    if (const auto held = held_add_index_.find(id); held != held_add_index_.end()) {
        const auto& prior = held_add_rows_.at(held->second);
        if (row.sess_id == prior.row.sess_id && row.isin_id == prior.row.isin_id && row.dir == prior.row.dir)
            key = prior.key;
    }
    if (key.empty() && row.id_ord1 > 0) {
        if (const auto ancestor = held_add_index_.find(row.id_ord1); ancestor != held_add_index_.end()) {
            const auto& prior = held_add_rows_.at(ancestor->second);
            if (row.sess_id > prior.row.sess_id && row.isin_id == prior.row.isin_id && row.dir == prior.row.dir)
                key = prior.key;
        }
    }
    if (key.empty() && row.ext_id != 0) {
        const auto reserved = ext_index_.find(row.ext_id);
        if (reserved != ext_index_.end()) {
            const auto add = unconfirmed_adds_.find(reserved->second);
            if (add != unconfirmed_adds_.end()) {
                const auto& command = pending_.at(add->second);
                const auto& original = orders_.at(reserved->second);
                if (row.sess_id == command.submitted_session && row.isin_id == original.request.isin_id &&
                    row.dir == static_cast<std::int8_t>(original.request.side))
                    key = reserved->second;
            }
        }
    }
    if (key.empty())
        return false;
    const auto identity = std::pair(row.sess_id, id);
    auto& held = held_add_rows_[identity];
    subtract_charge(held.charge);
    held.row = row;
    held.key = key;
    held.charge = {};
    const auto remaining = row.from_trade_repl ? row.public_amount_rest : row.private_amount_rest;
    if (remaining > 0) {
        held.charge.active = true;
        const auto price = plaza2::private_state::parse_session_decimal(row.price);
        held.charge.invalid_price = !price;
        held.charge.notional =
            price ? capped_notional(absolute_units(price->units), remaining, config_.risk.max_notional_scaled) : 0;
    }
    add_charge(held.charge);
    held_by_add_[key].insert(identity);
    held_add_index_[id] = identity;
    config_.next_ext_id = std::max(config_.next_ext_id, row.ext_id == INT32_MAX ? INT32_MAX : row.ext_id + 1);
    return true;
}
bool OrderManager::has_add_ancestor(std::int32_t session, std::int32_t isin, std::int8_t side) const {
    const auto scopes = add_contract_sessions_.find({isin, side});
    return scopes != add_contract_sessions_.end() && !scopes->second.empty() && *scopes->second.begin() < session;
}
void OrderManager::remember_add_evidence(const plaza2::private_state::OwnOrderSnapshot& row) {
    if (replaying_add_rows_)
        return;
    const auto id = row.private_order_id > 0 ? row.private_order_id : row.public_order_id;
    AddScope scope{row.sess_id, row.isin_id, row.dir};
    if (!unconfirmed_add_scopes_.contains(scope)) {
        if (row.id_ord1 <= 0)
            return;
        const auto parent = add_evidence_index_.find(row.id_ord1);
        if (parent != add_evidence_index_.end()) {
            const auto& previous = add_evidence_.at(parent->second);
            if (row.sess_id <= previous.row.sess_id || row.isin_id != previous.row.isin_id ||
                row.dir != previous.row.dir)
                return;
            scope = previous.submission;
        } else if (has_add_ancestor(row.sess_id, row.isin_id, row.dir)) {
            // The documented ancestor row may never have reached this process.
            // Preserve the owned explicit link; only an exact179 ancestor can
            // later authorize its association with a submitted logical Add.
            scope = {*add_contract_sessions_.at({row.isin_id, row.dir}).begin(), row.isin_id, row.dir};
        } else
            return;
        if (!unconfirmed_add_scopes_.contains(scope) && !has_add_ancestor(row.sess_id, row.isin_id, row.dir))
            return;
    }
    const auto identity = std::pair(row.sess_id, id);
    add_evidence_[identity] = {row, scope};
    add_evidence_index_[id] = identity;
    if (row.id_ord1 > 0)
        add_descendants_[row.id_ord1].insert(id);
}
void OrderManager::materialize_add_rows(const std::string& key, std::int64_t official_id) {
    if (official_id > 0) {
        // Claim exact identity globally: a provisional ext_id can have held the
        // official ID under a different concurrent Add. Only strict day links
        // descended from the official ID follow it into this logical order.
        std::vector<std::int64_t> chain{official_id};
        const auto& original = orders_.at(key);
        const auto submitted_session = std::get<0>(add_submitted_scopes_.at(key));
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const auto id = chain[i];
            const auto evidence = add_evidence_index_.find(id);
            if (evidence == add_evidence_index_.end()) {
                if (id == official_id) {
                    if (const auto descendants = add_descendants_.find(id); descendants != add_descendants_.end())
                        for (const auto next : descendants->second) {
                            const auto next_row = add_evidence_index_.find(next);
                            if (next_row == add_evidence_index_.end())
                                continue;
                            const auto& child = add_evidence_.at(next_row->second).row;
                            if (child.id_ord1 == id && child.sess_id > submitted_session &&
                                child.isin_id == original.request.isin_id &&
                                child.dir == static_cast<std::int8_t>(original.request.side))
                                chain.push_back(next);
                        }
                }
                continue;
            }
            const auto& raw = add_evidence_.at(evidence->second).row;
            if (raw.isin_id != original.request.isin_id || raw.dir != static_cast<std::int8_t>(original.request.side) ||
                raw.sess_id < submitted_session)
                continue;
            if (id != official_id) {
                if (!adopt_recovered_order(key, id, submitted_session) && !original.order_ids.contains(id)) {
                    if (const auto fills = filled_by_id_.find(id); fills != filled_by_id_.end()) {
                        if (original.executed > INT64_MAX - fills->second)
                            throw std::overflow_error("own trade quantity overflow");
                        orders_.at(key).executed += fills->second;
                    }
                }
            }
            terminal_links_.erase(id); // Replay preserved native terminal evidence too.
            const auto identity = evidence->second;
            auto& held = held_add_rows_[identity];
            if (!held.key.empty() && held.key != key) {
                auto& old = held_by_add_.at(held.key);
                old.erase(identity);
                if (old.empty())
                    held_by_add_.erase(held.key);
            }
            held.row = raw;
            held.key = key;
            held_by_add_[key].insert(identity);
            held_add_index_[id] = identity;
            if (const auto descendants = add_descendants_.find(id); descendants != add_descendants_.end())
                for (const auto next : descendants->second) {
                    const auto next_row = add_evidence_index_.find(next);
                    if (next_row == add_evidence_index_.end())
                        continue;
                    const auto& child = add_evidence_.at(next_row->second).row;
                    if (child.id_ord1 == id && child.sess_id > raw.sess_id && child.isin_id == raw.isin_id &&
                        child.dir == raw.dir)
                        chain.push_back(next);
                }
        }
    }
    std::vector<plaza2::private_state::OwnOrderSnapshot> rows;
    const auto held = held_by_add_.find(key);
    if (held != held_by_add_.end())
        for (const auto& identity : held->second)
            rows.push_back(held_add_rows_.at(identity).row);
    // The map supplies chronological session order. Bind official ancestry first;
    // observe_orders then follows id_ord1 before applying old-ID terminal rows.
    replaying_add_rows_ = true;
    try {
        if (!rows.empty())
            observe_orders(rows);
        else
            replay_deferred_trades();
    } catch (...) {
        replaying_add_rows_ = false;
        throw;
    }
    replaying_add_rows_ = false;
    // Existing held charges stay until every row has a regular tracked charge:
    // no admission can temporarily omit owned account exposure during transfer.
    if (held != held_by_add_.end()) {
        for (const auto& identity : held->second) {
            const auto& row = held_add_rows_.at(identity);
            subtract_charge(row.charge);
            const auto id = identity.second;
            if (const auto index = held_add_index_.find(id);
                index != held_add_index_.end() && index->second == identity)
                held_add_index_.erase(index);
            held_add_rows_.erase(identity);
        }
        held_by_add_.erase(held);
    }
    if (unconfirmed_adds_.erase(key)) {
        const auto scope = add_submitted_scopes_.at(key);
        auto reserved = unconfirmed_add_scopes_.find(scope);
        add_submitted_scopes_.erase(key);
        if (reserved != unconfirmed_add_scopes_.end() && --reserved->second == 0) {
            auto& sessions = add_contract_sessions_.at({std::get<1>(scope), std::get<2>(scope)});
            sessions.erase(std::get<0>(scope));
            if (sessions.empty())
                add_contract_sessions_.erase({std::get<1>(scope), std::get<2>(scope)});
            unconfirmed_add_scopes_.erase(reserved);
        }
    }
    if (orders_.contains(key))
        refresh_exposure(key);
}
bool OrderManager::has_uncertain_submission(std::int32_t session, std::int32_t isin, tr::Plaza2TradeSide side) const {
    for (const auto& [id, command] : pending_) {
        if ((command.encoded.command_kind != Kind::AddOrder && command.encoded.command_kind != Kind::MoveOrder) ||
            command.submitted_session != session)
            continue;
        const auto order = orders_.find(command.key);
        if (order != orders_.end() && order->second.request.isin_id == isin && order->second.request.side == side)
            return true;
    }
    return false;
}
bool OrderManager::adopt_recovered_order(const std::string& key, std::int64_t official_id,
                                         std::int32_t submitted_session) {
    const auto index = order_index_.find(official_id);
    if (index == order_index_.end() || index->second == key)
        return false;
    const auto previous_key = index->second;
    if (!previous_key.starts_with("recovered:") || orders_.at(previous_key).execution_baseline_known)
        throw std::runtime_error("official Add identity already belongs to another submitted order");
    auto& original = orders_.at(key);
    const auto& recovered = orders_.at(previous_key);
    if (recovered.request.isin_id != original.request.isin_id || recovered.request.side != original.request.side ||
        recovered.sess_id < submitted_session)
        throw std::runtime_error("official Add identity conflicts with recovered ownership");
    original.state = recovered.state;
    original.sess_id = recovered.sess_id;
    original.order_id = recovered.order_id;
    original.remaining = recovered.remaining;
    std::int64_t transferred_fills{};
    for (const auto id : recovered.order_ids)
        if (!original.order_ids.contains(id)) {
            const auto fill = filled_by_id_.find(id);
            if (fill != filled_by_id_.end()) {
                if (transferred_fills > INT64_MAX - fill->second)
                    throw std::overflow_error("own trade quantity overflow");
                transferred_fills += fill->second;
            }
        }
    if (original.executed > INT64_MAX - transferred_fills)
        throw std::overflow_error("own trade quantity overflow");
    original.executed += transferred_fills;
    original.request.price = recovered.request.price;
    original.confirmed_by_replication = recovered.confirmed_by_replication;
    original.operator_action_required = recovered.operator_action_required;
    original.cancel_requested = original.cancel_requested || recovered.cancel_requested;
    original.order_ids.insert(recovered.order_ids.begin(), recovered.order_ids.end());
    for (const auto id : recovered.order_ids)
        order_index_[id] = key;
    if (const auto ext = ext_index_.find(recovered.ext_id); ext != ext_index_.end() && ext->second == previous_key)
        ext_index_.erase(ext);
    const auto rekey = [&](Command& command) {
        if (command.key == previous_key)
            command.key = key;
    };
    for (auto& command : adds_)
        rekey(command);
    for (auto& command : cancels_)
        rekey(command);
    for (auto& [id, command] : pending_)
        rekey(command);
    erase_exposure(previous_key);
    orders_.erase(previous_key);
    refresh_exposure(key);
    return true;
}
void OrderManager::advance_session(std::int32_t session) {
    if (session <= current_session_)
        return;
    previous_session_ = current_session_;
    current_session_ = session;
    // Retain one observed prior session for delayed split-transaction relists.
    std::erase_if(terminal_links_, [&](const auto& entry) { return entry.second.sess_id < previous_session_; });
    if (!replaying_add_rows_)
        prune_terminal();
    if (!replaying_add_rows_) {
        std::erase_if(deferred_orders_, [&](const auto& entry) {
            const auto& row = entry.second;
            return row.sess_id < previous_session_ &&
                   !has_uncertain_submission(row.sess_id, row.isin_id, static_cast<tr::Plaza2TradeSide>(row.dir));
        });
        std::erase_if(deferred_trades_, [&](const auto& entry) {
            const auto& trade = entry.second;
            if (trade.sess_id >= previous_session_)
                return false;
            for (const bool buy : {true, false}) {
                const auto id =
                    buy ? (trade.private_order_id_buy > 0 ? trade.private_order_id_buy : trade.public_order_id_buy)
                        : (trade.private_order_id_sell > 0 ? trade.private_order_id_sell : trade.public_order_id_sell);
                const auto& code = buy ? trade.code_buy : trade.code_sell;
                if (code == config_.broker_code + config_.client_code) {
                    const auto candidate = add_evidence_index_.find(id);
                    if (candidate != add_evidence_index_.end()) {
                        const auto& evidence = add_evidence_.at(candidate->second);
                        const auto& row = evidence.row;
                        if (row.isin_id == trade.isin_id && row.dir == (buy ? 1 : 2) &&
                            (unconfirmed_add_scopes_.contains(evidence.submission) ||
                             (row.id_ord1 > 0 && has_add_ancestor(row.sess_id, row.isin_id, row.dir)) ||
                             has_uncertain_submission(row.sess_id, row.isin_id,
                                                      static_cast<tr::Plaza2TradeSide>(row.dir))))
                            return false; // A terminal raw candidate can still acquire its first late deal.
                    }
                }
                if (code == config_.broker_code + config_.client_code &&
                    (held_add_index_.contains(id) ||
                     has_uncertain_submission(trade.sess_id, trade.isin_id,
                                              buy ? tr::Plaza2TradeSide::Buy : tr::Plaza2TradeSide::Sell)))
                    return false;
            }
            return true;
        });
        // Raw candidate evidence is needed while any Add in its submitted
        // scope remains unconfirmed. Retire old resolved scopes without ever
        // erasing fill counters that still belong to a tracked live identity.
        std::erase_if(add_evidence_, [&](const auto& entry) {
            const auto& [identity, evidence] = entry;
            if (evidence.row.sess_id >= previous_session_ || unconfirmed_add_scopes_.contains(evidence.submission) ||
                (evidence.row.id_ord1 > 0 &&
                 has_add_ancestor(evidence.row.sess_id, evidence.row.isin_id, evidence.row.dir)) ||
                has_uncertain_submission(evidence.row.sess_id, evidence.row.isin_id,
                                         static_cast<tr::Plaza2TradeSide>(evidence.row.dir)))
                return false;
            const auto id = identity.second;
            if (const auto index = add_evidence_index_.find(id);
                index != add_evidence_index_.end() && index->second == identity) {
                add_evidence_index_.erase(index);
                add_descendants_.erase(id);
            }
            if (const auto parent = add_descendants_.find(evidence.row.id_ord1); parent != add_descendants_.end()) {
                parent->second.erase(id);
                if (parent->second.empty())
                    add_descendants_.erase(parent);
            }
            if (!order_index_.contains(id) && !held_add_index_.contains(id))
                filled_by_id_.erase(id);
            return true;
        });
    }
}
void OrderManager::prune_terminal() {
    bool pruned{};
    while (!terminal_orders_.empty() && terminal_orders_.begin()->first < current_session_) {
        const auto key = *terminal_orders_.begin()->second.begin();
        const auto& order = orders_.at(key);
        if (order.order_id > 0 && order.sess_id >= previous_session_)
            terminal_links_[order.order_id] = {.key = key,
                                               .price = order.request.price,
                                               .isin_id = order.request.isin_id,
                                               .sess_id = order.sess_id,
                                               .ext_id = order.ext_id,
                                               .quantity = order.request.quantity,
                                               .side = order.request.side,
                                               .executed = order.executed,
                                               .sent_utc_seconds = order.sent_utc_seconds,
                                               .cancel_requested = order.cancel_requested,
                                               .execution_baseline_known = order.execution_baseline_known};
        for (const auto id : order.order_ids) {
            if (const auto index = order_index_.find(id); index != order_index_.end() && index->second == key)
                order_index_.erase(index);
            if (const auto evidence = add_evidence_index_.find(id);
                evidence == add_evidence_index_.end() ||
                (!unconfirmed_add_scopes_.contains(add_evidence_.at(evidence->second).submission) &&
                 !has_add_ancestor(add_evidence_.at(evidence->second).row.sess_id, order.request.isin_id,
                                   static_cast<std::int8_t>(order.request.side)) &&
                 !has_uncertain_submission(order.sess_id, order.request.isin_id, order.request.side)))
                filled_by_id_.erase(id);
        }
        if (const auto index = ext_index_.find(order.ext_id); index != ext_index_.end() && index->second == key)
            ext_index_.erase(index);
        move_reservations_.erase(key);
        erase_exposure(key);
        orders_.erase(key);
        pruned = true;
    }
    if (pruned) {
        // Scan each command queue once, rather than once per pruned order.
        const auto orphan = [&](const Command& command) {
            return !command.key.empty() && !orders_.contains(command.key);
        };
        std::erase_if(adds_, orphan);
        std::erase_if(cancels_, orphan);
        std::erase_if(pending_, [&](const auto& entry) { return orphan(entry.second); });
    }
}
void OrderManager::changed(const std::string& key) {
    refresh_exposure(key);
    emit("order", order_json(key, orders_.at(key)));
}
bool OrderManager::has_outstanding_command(std::string_view key, Kind kind) const {
    const auto same = [&](const Command& command) {
        return command.key == key && command.encoded.command_kind == kind;
    };
    return std::any_of(adds_.begin(), adds_.end(), same) || std::any_of(cancels_.begin(), cancels_.end(), same) ||
           std::any_of(pending_.begin(), pending_.end(), [&](const auto& item) { return same(item.second); });
}
bool OrderManager::operator_action_required() const noexcept {
    return operator_action_required_ || !operator_orders_.empty();
}

std::string OrderManager::check_risk(const OrderRequest& request, std::size_t extra,
                                     std::string_view exclude_key) const {
    if (config_.risk.kill_switch)
        return "kill switch enabled";
    if (!ready_(request.isin_id))
        return "order entry not ready";
    if (request.quantity <= 0 || request.quantity > config_.risk.max_quantity)
        return "quantity exceeds configured limit";
    if (extra > config_.risk.max_open_orders || active_orders_ > config_.risk.max_open_orders - extra)
        return "open-order limit exceeded";
    const auto price = plaza2::private_state::parse_session_decimal(request.price);
    const auto terms = terms_(request.isin_id);
    if (!price || !terms || !terms->bounds.interval_valid || !terms->min_step || terms->min_step->units <= 0)
        return "current session price terms unavailable";
    if ((terms->bounds.lower && price->units < terms->bounds.lower->units) ||
        (terms->bounds.upper && price->units > terms->bounds.upper->units))
        return "price outside exchange limits";
    if (price->units % terms->min_step->units != 0)
        return "price is not tick aligned";
    auto low = notional_low_, high = notional_high_;
    auto invalid = invalid_prices_;
    if (const auto excluded = exposures_.find(std::string(exclude_key)); excluded != exposures_.end()) {
        invalid -= excluded->second.invalid_price;
        if (low < excluded->second.notional)
            --high;
        low -= excluded->second.notional;
    }
    if (invalid)
        return "outstanding-order price unavailable";
    auto units = absolute_units(price->units);
    auto proposed_remaining = std::int64_t(request.quantity);
    if (const auto current = orders_.find(std::string(exclude_key)); current != orders_.end()) {
        const auto& order = current->second;
        proposed_remaining = std::max<std::int64_t>(0, proposed_remaining - order.executed);
        const auto old_price = plaza2::private_state::parse_session_decimal(order.request.price);
        if (!old_price)
            return "outstanding-order price unavailable";
        units = std::max(units, absolute_units(old_price->units));
        proposed_remaining = std::max(proposed_remaining, order.remaining);
        if (const auto move = move_reservations_.find(current->first); move != move_reservations_.end()) {
            units = std::max(units, move->second.price_units);
            proposed_remaining = std::max(proposed_remaining, std::int64_t(move->second.quantity) - order.executed);
        }
    }
    const auto cap = static_cast<std::uint64_t>(config_.risk.max_notional_scaled);
    const auto proposed = capped_notional(units, proposed_remaining, cap);
    if (high || low > cap || proposed > cap - low)
        return "aggregate outstanding-order notional exceeds configured limit";
    return {};
}
std::uint32_t OrderManager::reserve_user_id() {
    if (config_.next_user_id == UINT32_MAX)
        throw std::runtime_error("user_id space exhausted");
    const auto id = config_.next_user_id++;
    emit("reservation", "{\"next_ext_id\":" + std::to_string(config_.next_ext_id) +
                            ",\"next_user_id\":" + std::to_string(config_.next_user_id) + "}");
    return id;
}
OrderManager::Command OrderManager::encode(tr::Plaza2TradeCommandRequest request, std::string key) {
    Command result{.encoded = tr::Plaza2TradeCodec{}.encode(request), .key = std::move(key)};
    if (!result.encoded.validation.ok())
        throw std::invalid_argument(result.encoded.validation.field_name + ": " + result.encoded.validation.message);
    if (const auto* cancel = std::get_if<tr::DelOrderRequest>(&request))
        result.target_order_id = cancel->order_id.value_or(0);
    result.user_id = reserve_user_id();
    emit("reservation", "{\"next_ext_id\":" + std::to_string(config_.next_ext_id) +
                            ",\"next_user_id\":" + std::to_string(config_.next_user_id) + "}");
    return result;
}
std::string OrderManager::place(OrderRequest request) {
    if (request.client_order_id.empty() || used_client_ids_.contains(request.client_order_id))
        return "client order id empty or already used";
    if (auto error = check_risk(request, 1); !error.empty())
        return error;
    if (config_.next_ext_id == INT32_MAX)
        return "ext_id space exhausted";
    const auto ext = config_.next_ext_id++;
    tr::AddOrderRequest add;
    add.broker_code = config_.broker_code;
    add.client_code = config_.client_code;
    add.isin_id = request.isin_id;
    add.dir = request.side;
    add.type = request.type;
    add.amount = request.quantity;
    add.price = request.price;
    add.comment = request.comment;
    add.ext_id = ext;
    add.is_check_limit = 0;
    try {
        auto command = encode(add, request.client_order_id);
        ManagedOrder order{.request = std::move(request), .ext_id = ext};
        order.sess_id = terms_(order.request.isin_id)->sess_id;
        order.remaining = order.request.quantity;
        const auto key = order.request.client_order_id;
        orders_.emplace(key, std::move(order));
        used_client_ids_.insert(key);
        advance_session(orders_.at(key).sess_id);
        ext_index_[ext] = key;
        adds_.push_back(std::move(command));
        changed(key);
        return {};
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
}
void OrderManager::enqueue_cancel(ManagedOrder& order) {
    if (order.operator_action_required || bulk_cancellations_.contains(order.request.isin_id))
        return;
    if (order.order_id <= 0) {
        recovery_cancel(order);
        return;
    }
    const auto& key = order.request.client_order_id;
    const auto same = [&](const Command& cmd) {
        return cmd.key == key && cmd.encoded.command_kind == Kind::DelOrder && cmd.target_order_id == order.order_id;
    };
    if (std::any_of(cancels_.begin(), cancels_.end(), same) ||
        std::any_of(pending_.begin(), pending_.end(), [&](const auto& item) { return same(item.second); }))
        return;
    tr::DelOrderRequest request;
    request.broker_code = config_.broker_code;
    request.client_code = config_.client_code;
    request.order_id = order.order_id;
    request.isin_id = order.request.isin_id;
    cancels_.push_back(encode(request, key));
    if (order.state != OrderState::Unknown)
        order.state = OrderState::PendingCancel;
    changed(key);
}
void OrderManager::recovery_cancel(ManagedOrder& order) {
    if (order.operator_action_required || bulk_cancellations_.contains(order.request.isin_id))
        return;
    const auto& key = order.request.client_order_id;
    const auto same = [&](const Command& cmd) {
        return cmd.key == key && cmd.encoded.command_kind == Kind::DelUserOrders;
    };
    if (std::any_of(cancels_.begin(), cancels_.end(), same) ||
        std::any_of(pending_.begin(), pending_.end(), [&](const auto& item) { return same(item.second); }))
        return;
    tr::DelUserOrdersRequest request;
    request.broker_code = config_.broker_code;
    request.code = config_.client_code;
    request.buy_sell = 3;
    request.non_system = 0;
    request.base_contract_code = "";
    request.ext_id = order.ext_id;
    request.isin_id = order.request.isin_id;
    request.instrument_mask = 1;
    cancels_.push_back(encode(request, key));
}
std::string OrderManager::cancel(std::string_view client_id) {
    const auto found = orders_.find(std::string(client_id));
    if (found == orders_.end())
        return "unknown client order id";
    auto& order = found->second;
    if (terminal(order.state))
        return {};
    // A new explicit operator request may retry an exhausted cancellation.
    order.operator_action_required = false;
    order.cancel_requested = true;
    const auto unsent_move = [&](const Command& cmd) {
        return cmd.key == client_id && cmd.encoded.command_kind == Kind::MoveOrder;
    };
    const auto removed_moves = std::erase_if(adds_, unsent_move) + std::erase_if(cancels_, unsent_move);
    if (removed_moves)
        move_reservations_.erase(found->first);
    refresh_exposure(found->first);
    // An Add still in the queue has never been sent and is safe to discard.
    const auto queued = std::find_if(adds_.begin(), adds_.end(), [&](const auto& cmd) {
        return cmd.key == client_id && cmd.encoded.command_kind == Kind::AddOrder;
    });
    if (queued != adds_.end()) {
        adds_.erase(queued);
        order.remaining = 0;
        order.state = OrderState::Cancelled;
        changed(found->first);
    } else
        enqueue_cancel(order);
    return {};
}
std::string OrderManager::move(std::string_view client_id, std::string price, std::int32_t quantity) {
    const auto found = orders_.find(std::string(client_id));
    if (found == orders_.end())
        return "unknown client order id";
    auto& order = found->second;
    if ((order.state != OrderState::Working && order.state != OrderState::PartFilled) || order.order_id <= 0 ||
        order.cancel_requested || has_outstanding_command(client_id, Kind::AddOrder) ||
        has_outstanding_command(client_id, Kind::MoveOrder))
        return "only a working order can be moved";
    if (!order.execution_baseline_known)
        return "recovered order has no complete fill baseline; cancel instead of moving";
    const auto earlier_fills = order.executed - filled_by_id_[order.order_id];
    if (quantity <= earlier_fills)
        return "target quantity is no greater than fills on previous order IDs; cancel instead";
    auto requested = order.request;
    requested.price = price;
    requested.quantity = quantity;
    if (auto error = check_risk(requested, 0, found->first); !error.empty())
        return error;
    tr::MoveOrderRequest request;
    request.broker_code = config_.broker_code;
    request.client_code = config_.client_code;
    request.isin_id = order.request.isin_id;
    request.regime = 3;
    request.order_id1 = order.order_id;
    request.amount1 = static_cast<std::int32_t>(quantity - earlier_fills);
    request.price1 = price;
    request.ext_id1 = order.ext_id;
    request.is_check_limit = 0;
    auto command = encode(request, found->first);
    command.replacement_price = std::move(price);
    command.replacement_quantity = quantity;
    move_reservations_[found->first] = {
        absolute_units(plaza2::private_state::parse_session_decimal(command.replacement_price)->units), quantity};
    adds_.push_back(std::move(command));
    order.state = OrderState::PendingReplace;
    changed(found->first);
    return {};
}
std::string OrderManager::cancel_all(std::int32_t isin) {
    if (isin <= 0)
        return "cancel-all requires an instrument id";
    if (next_bulk_generation_ == UINT64_MAX)
        return "mass cancellation generation exhausted";
    tr::DelUserOrdersRequest request;
    request.broker_code = config_.broker_code;
    request.code = config_.client_code;
    request.buy_sell = 3;
    request.non_system = 0;
    request.base_contract_code = "";
    request.isin_id = isin;
    request.instrument_mask = 1;
    auto command = encode(request, "");
    command.bulk_generation = next_bulk_generation_++;
    bulk_cancellations_[isin] = {.generation = command.bulk_generation};
    operator_action_required_ = false;
    const auto affected = [&](const Command& queued) {
        const auto order = orders_.find(queued.key);
        return order != orders_.end() && order->second.request.isin_id == isin;
    };
    for (const auto& queued : adds_)
        if (affected(queued) && queued.encoded.command_kind == Kind::MoveOrder)
            move_reservations_.erase(queued.key);
    for (auto& [key, order] : orders_) {
        if (order.request.isin_id != isin || terminal(order.state))
            continue;
        order.cancel_requested = true;
        order.operator_action_required = false;
        const bool unsent_add = std::any_of(adds_.begin(), adds_.end(), [&](const auto& queued) {
            return queued.key == key && queued.encoded.command_kind == Kind::AddOrder;
        });
        if (unsent_add) {
            order.remaining = 0;
            order.state = OrderState::Cancelled;
        } else if (order.state != OrderState::Unknown && order.state != OrderState::PendingReplace)
            order.state = OrderState::PendingCancel;
        changed(key);
    }
    std::erase_if(adds_, affected);
    std::erase_if(cancels_, [&](const auto& queued) {
        return affected(queued) || (queued.bulk_generation && queued.encoded.isin_id == isin);
    });
    // One exchange-side mass cancellation starts immediately, ahead of all
    // other queued risk reduction. Individual fallbacks wait for reconciliation.
    cancels_.push_front(std::move(command));
    return {};
}
void OrderManager::set_kill_switch(bool enabled) {
    config_.risk.kill_switch = enabled;
    emit("kill_switch", enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
}

void OrderManager::complete_timeout(Command command, Clock::time_point now) {
    emit("timeout", "{\"user_id\":" + std::to_string(command.user_id) + "}");
    const auto found = orders_.find(command.key);
    if (found == orders_.end()) {
        retry_cancel(std::move(command), now);
        return;
    }
    auto& order = found->second;
    if (command.encoded.command_kind == Kind::AddOrder) {
        command.deadline = Clock::time_point::max();
        pending_.emplace(command.user_id, std::move(command));
        if (!terminal(order.state)) {
            order.state = OrderState::Unknown;
            order.cancel_requested = true;
            recovery_cancel(order);
            changed(found->first);
        }
        return;
    }
    if (terminal(order.state))
        return;
    if (command.encoded.command_kind == Kind::MoveOrder) {
        order.state = OrderState::Unknown;
        order.cancel_requested = true;
        if (command.encoded.command_kind == Kind::MoveOrder) {
            // Never resend an uncertain replacement. Retain its correlation
            // for a late definitive reply and reserve its possible new order.
            command.deadline = Clock::time_point::max();
            pending_.emplace(command.user_id, std::move(command));
        }
        recovery_cancel(order);
    } else {
        retry_cancel(std::move(command), now);
    }
    changed(found->first);
}
void OrderManager::retry_cancel(Command command, Clock::time_point now, bool business_rejection) {
    auto found = orders_.find(command.key);
    if (command.bulk_generation && command.encoded.isin_id) {
        const auto bulk = bulk_cancellations_.find(*command.encoded.isin_id);
        if (bulk == bulk_cancellations_.end() || bulk->second.generation != command.bulk_generation)
            return;
    }
    if (found != orders_.end() && bulk_cancellations_.contains(found->second.request.isin_id))
        return; // The mass request superseded this outstanding individual cancel.
    if (business_rejection)
        ++command.business_failures;
    if (command.business_failures >= config_.max_cancel_attempts) {
        if (found == orders_.end()) {
            operator_action_required_ = true;
            if (command.bulk_generation && command.encoded.isin_id)
                bulk_cancellations_.erase(*command.encoded.isin_id);
        }
        if (found != orders_.end()) {
            found->second.operator_action_required = true;
            found->second.state = OrderState::Unknown;
            found->second.last_error =
                "UNRESOLVED: cancellation business-rejection limit reached; operator action required";
            changed(found->first);
        }
        emit("unresolved", "{\"client_order_id\":" + json_string(command.key) + ",\"business_rejections\":" +
                               std::to_string(command.business_failures) + ",\"operator_action_required\":true}");
        return;
    }
    auto delay = config_.cancel_retry_base;
    for (std::uint32_t i = 1; i < command.business_failures && delay < config_.cancel_retry_max; ++i)
        delay = delay >= config_.cancel_retry_max / 2 ? config_.cancel_retry_max : delay * 2;
    command.user_id = reserve_user_id();
    command.acknowledged = false;
    command.not_before = now + delay;
    cancels_.push_back(std::move(command));
}
void OrderManager::poll(Clock::time_point now, std::int64_t utc_seconds) {
    now_ = now;
    std::vector<std::uint32_t> expired;
    for (const auto& [id, command] : pending_)
        if (now >= command.deadline)
            expired.push_back(id);
    for (const auto id : expired)
        on_timeout(id, now);
    while (!cancels_.empty() || !adds_.empty()) {
        auto* queue_pointer = &cancels_;
        auto selected =
            std::find_if(cancels_.begin(), cancels_.end(), [&](const auto& cmd) { return now >= cmd.not_before; });
        if (selected == cancels_.end()) {
            queue_pointer = &adds_;
            selected = std::find_if(adds_.begin(), adds_.end(), [&](const auto& cmd) {
                const auto order = orders_.find(cmd.key);
                return now >= cmd.not_before && (order == orders_.end() || terminal(order->second.state) ||
                                                 ready_(order->second.request.isin_id));
            });
        }
        auto& queue = *queue_pointer;
        if (selected == queue.end())
            break;
        auto& command = *selected;
        auto found = orders_.find(command.key);
        const bool add_or_move =
            command.encoded.command_kind == Kind::AddOrder || command.encoded.command_kind == Kind::MoveOrder;
        if (found != orders_.end() && terminal(found->second.state)) {
            queue.erase(selected);
            continue;
        }
        if (add_or_move && found != orders_.end()) {
            auto proposed = found->second.request;
            if (command.encoded.command_kind == Kind::MoveOrder) {
                proposed.price = command.replacement_price;
                proposed.quantity = command.replacement_quantity;
            }
            const auto error = check_risk(proposed, 0, command.key);
            if (error.empty() && command.encoded.command_kind == Kind::AddOrder)
                found->second.sess_id = terms_(proposed.isin_id)->sess_id;
            if (!error.empty()) {
                if (!ready_(proposed.isin_id))
                    break; // Preserve queued work through an outage/clearing.
                found->second.last_error = error;
                if (command.encoded.command_kind == Kind::MoveOrder)
                    move_reservations_.erase(found->first);
                found->second.state = command.encoded.command_kind == Kind::AddOrder ? OrderState::Rejected
                                                                                     : settled_state(found->second);
                changed(found->first);
                queue.erase(selected);
                continue;
            }
        }
        // For cancels, exchange availability is the transport's decision;
        // session/instrument trading state must not prevent risk reduction.
        if (!rate_.admit(milliseconds(now))) {
            if (!throttled_)
                emit("throttle", "{\"active\":true,\"queued\":" + std::to_string(queued()) + "}");
            throttled_ = true;
            break;
        }
        if (throttled_)
            emit("throttle", "{\"active\":false,\"queued\":" + std::to_string(queued()) + "}");
        throttled_ = false;
        emit("command", "{\"user_id\":" + std::to_string(command.user_id) +
                            ",\"name\":" + json_string(command.encoded.command_name) + ",\"client_order_id\":" +
                            json_string(command.key) + ",\"fields\":" + command.encoded.fields_json +
                            ",\"payload_hex\":" + json_string(tr::bytes_to_hex(command.encoded.payload)) + "}");
        if (logging_failed_ && add_or_move)
            break;
        const auto result = send_(command.encoded, command.user_id);
        const auto result_fields =
            "{\"user_id\":" + std::to_string(command.user_id) +
            ",\"certainty\":" + std::to_string(static_cast<int>(result.certainty)) + ",\"error\":" +
            json_string(result.validation_error.message + result.allocation_error.message + result.post_error.message) +
            "}";
        if (result.certainty == cg::Plaza2SubmissionCertainty::DefinitelyNotSent) {
            command.not_before = now + std::chrono::seconds(1);
            // Definitive local validation failures cannot become an ambiguous Add.
            if (add_or_move && result.validation_error.code == cg::Plaza2ErrorCode::InvalidConfiguration) {
                if (found != orders_.end()) {
                    if (command.encoded.command_kind == Kind::MoveOrder)
                        move_reservations_.erase(found->first);
                    found->second.state = command.encoded.command_kind == Kind::AddOrder ? OrderState::Rejected
                                                                                         : settled_state(found->second);
                    found->second.last_error = result.validation_error.message;
                    changed(found->first);
                }
                queue.erase(selected);
            }
            emit("command_result", result_fields);
            break;
        }
        const auto sent_kind = command.encoded.command_kind;
        auto sent = std::move(command);
        queue.erase(selected);
        if (found != orders_.end() && found->second.sent_utc_seconds == 0)
            found->second.sent_utc_seconds = utc_seconds;
        sent.deadline = now + config_.reply_timeout;
        if (found != orders_.end())
            sent.submitted_session = found->second.sess_id;
        if (sent_kind == Kind::AddOrder) {
            unconfirmed_adds_[sent.key] = sent.user_id;
            const auto& order = orders_.at(sent.key);
            const auto scope =
                AddScope{sent.submitted_session, order.request.isin_id, static_cast<std::int8_t>(order.request.side)};
            add_submitted_scopes_[sent.key] = scope;
            ++unconfirmed_add_scopes_[scope];
            add_contract_sessions_[{order.request.isin_id, static_cast<std::int8_t>(order.request.side)}].insert(
                sent.submitted_session);
        }
        pending_.emplace(sent.user_id, std::move(sent));
        // Commit submission bookkeeping before a user-supplied log callback.
        emit("command_result", result_fields);
        if (result.certainty == cg::Plaza2SubmissionCertainty::PossiblySent &&
            (sent_kind == Kind::AddOrder || sent_kind == Kind::MoveOrder)) {
            // Add or replacement identity is uncertain. A DelOrder still
            // targets its known ID and awaits replication/confirmation retry.
            if (found != orders_.end()) {
                found->second.state = OrderState::Unknown;
                found->second.cancel_requested = true;
                recovery_cancel(found->second);
                changed(found->first);
            }
        }
    }
}
void OrderManager::on_timeout(std::uint32_t id, Clock::time_point now) {
    const auto found = pending_.find(id);
    if (found == pending_.end()) {
        emit("late_timeout", "{\"user_id\":" + std::to_string(id) + "}");
        return;
    }
    auto command = std::move(found->second);
    pending_.erase(found);
    complete_timeout(std::move(command), now);
}
void OrderManager::on_reply(std::uint32_t id, const tr::Plaza2TradeDecodedReply& reply, Clock::time_point now,
                            std::uint64_t trade_commit_sequence) {
    const auto pending = pending_.find(id);
    emit("reply", "{\"user_id\":" + std::to_string(id) + ",\"msgid\":" + std::to_string(reply.msgid) +
                      ",\"code\":" + std::to_string(reply.code) + ",\"message\":" + json_string(reply.message) +
                      ",\"num_orders\":" + std::to_string(reply.num_orders.value_or(-1)) +
                      ",\"order_id\":" + std::to_string(reply.order_id.value_or(0)) +
                      ",\"order_id1\":" + std::to_string(reply.order_id1.value_or(0)) +
                      ",\"order_id2\":" + std::to_string(reply.order_id2.value_or(0)) +
                      ",\"amount\":" + std::to_string(reply.amount.value_or(-1)) +
                      ",\"iceberg_order_id\":" + std::to_string(reply.iceberg_order_id.value_or(0)) +
                      ",\"penalty_remain\":" + std::to_string(reply.penalty_remain.value_or(0)) +
                      ",\"queue_size\":" + std::to_string(reply.queue_size.value_or(0)) + "}");
    if (reply.msgid == 99)
        rate_.penalize(milliseconds(now), static_cast<std::uint32_t>(std::max(0, reply.penalty_remain.value_or(0))));
    if (pending == pending_.end()) {
        emit("unknown_reply", "{\"user_id\":" + std::to_string(id) + "}");
        return;
    }
    const auto kind = pending->second.encoded.command_kind;
    const auto expected = kind == Kind::AddOrder    ? 179
                          : kind == Kind::DelOrder  ? 177
                          : kind == Kind::MoveOrder ? 176
                                                    : 186;
    if (reply.msgid != expected && reply.msgid != 99 && reply.msgid != 100) {
        emit("unexpected_reply", "{\"user_id\":" + std::to_string(id) + "}");
        return;
    }
    if (pending->second.acknowledged)
        return;
    auto command = std::move(pending->second);
    pending_.erase(pending);
    const auto found = orders_.find(command.key);
    bool superseded_bulk{};
    if (command.bulk_generation && command.encoded.isin_id) {
        const auto bulk = bulk_cancellations_.find(*command.encoded.isin_id);
        superseded_bulk = bulk == bulk_cancellations_.end() || bulk->second.generation != command.bulk_generation;
    }
    if (reply.msgid == 99) {
        if (superseded_bulk)
            return; // Keep the global penalty even when a newer mass request owns reconciliation.
        if (found != orders_.end() && command.encoded.command_kind != Kind::AddOrder &&
            (terminal(found->second.state) || bulk_cancellations_.contains(found->second.request.isin_id) ||
             (command.encoded.command_kind == Kind::MoveOrder && found->second.cancel_requested))) {
            if (command.encoded.command_kind == Kind::MoveOrder) {
                move_reservations_.erase(found->first);
                if (found->second.state == OrderState::Unknown)
                    found->second.state =
                        found->second.remaining > 0 ? OrderState::PendingCancel : settled_state(found->second);
                changed(found->first);
            }
            return; // A later cancellation superseded this rejected exchange command.
        }
        // A flood reply rejects the Add conclusively, so callers may submit a
        // new client id. Cancels retain their place in the risk-reduction queue.
        if (command.encoded.command_kind == Kind::AddOrder) {
            if (found != orders_.end()) {
                found->second.state = OrderState::Rejected;
                found->second.remaining = 0;
                found->second.operator_action_required = false;
                changed(found->first);
                materialize_add_rows(found->first);
                prune_terminal();
            }
        } else {
            // A flood penalty says the exchange did not process this command.
            // It does not spend the bounded business-rejection retry budget.
            command.user_id = reserve_user_id();
            command.acknowledged = false;
            command.not_before = now + std::chrono::milliseconds(std::max(1, reply.penalty_remain.value_or(0)));
            auto& queue = command.encoded.command_kind == Kind::MoveOrder ? adds_ : cancels_;
            queue.push_back(std::move(command));
        }
        return;
    }
    if (superseded_bulk)
        return;
    if (found == orders_.end()) {
        if (command.bulk_generation && command.encoded.isin_id) {
            const auto bulk = bulk_cancellations_.find(*command.encoded.isin_id);
            if (bulk == bulk_cancellations_.end() || bulk->second.generation != command.bulk_generation)
                return; // A newer explicit mass request owns this instrument.
            if (reply.code == 0 && reply.msgid == 186) {
                bulk->second.awaiting_reply = false;
                bulk->second.after_commit_sequence = std::max(trade_commit_sequence_, trade_commit_sequence);
                if (reply.num_orders && *reply.num_orders == 0)
                    for (auto& [key, order] : orders_)
                        if (order.request.isin_id == *command.encoded.isin_id && order.cancel_requested &&
                            order.order_id == 0)
                            order.absence_reply = true;
                // Acceptance still needs a subsequent committed TRADE view.
                // Keep the timer so a lost confirmation cannot strand the group.
                command.acknowledged = true;
                command.deadline = now + config_.reply_timeout;
                pending_.emplace(id, std::move(command));
                return;
            }
        }
        if (reply.code != 0 || reply.msgid == 100)
            retry_cancel(std::move(command), now, reply.msgid != 100 && reply.code != 0);
        return;
    }
    auto& order = found->second;
    if (command.encoded.command_kind == Kind::AddOrder && reply.msgid == 179) {
        const auto key = found->first;
        if (reply.code != 0) {
            order.state = OrderState::Rejected;
            order.remaining = 0;
            order.last_error = reply.message;
            order.operator_action_required = false;
            changed(key);
            materialize_add_rows(key);
            prune_terminal();
            return;
        }
        if (reply.order_id.value_or(0) <= 0) {
            complete_timeout(std::move(command), now);
            return; // Keep correlation for a later well-formed authoritative reply.
        }
        if (order.sess_id > command.submitted_session && order.order_ids.contains(*reply.order_id)) {
            // A known ancestor's old-session acceptance must not roll a relist back.
            materialize_add_rows(key);
            changed(key);
            prune_terminal();
            return;
        }
        if (!adopt_recovered_order(key, *reply.order_id, command.submitted_session)) {
            order.order_id = *reply.order_id;
            order.sess_id = command.submitted_session;
            if (const auto fill = filled_by_id_.find(order.order_id); fill != filled_by_id_.end())
                order.executed = fill->second;
            order.order_ids.insert(order.order_id);
            order_index_[order.order_id] = key;
            order.remaining = std::max<std::int64_t>(0, std::int64_t(order.request.quantity) - order.executed);
            order.confirmed_by_replication = false;
            order.operator_action_required = false;
            order.state = order.executed ? OrderState::PartFilled : OrderState::Working;
            refresh_exposure(key);
        }
        materialize_add_rows(key, *reply.order_id);
        if (order.cancel_requested && !terminal(order.state))
            enqueue_cancel(order);
        changed(key);
        prune_terminal();
        return;
    }
    if (command.encoded.command_kind == Kind::AddOrder && reply.msgid == 100) {
        order.last_error = reply.message;
        complete_timeout(std::move(command), now);
        return;
    }
    if (terminal(order.state))
        return; // Replication wins over a late non-Add reply.
    if (reply.msgid == 100) {
        order.last_error = reply.message;
        complete_timeout(std::move(command), now);
        return;
    }
    if (reply.code != 0) {
        order.last_error = reply.message;
        if (command.encoded.command_kind == Kind::AddOrder) {
            if (order.order_id == 0)
                order.state = OrderState::Rejected;
        } else if (command.encoded.command_kind == Kind::MoveOrder) {
            move_reservations_.erase(found->first);
            order.state = settled_state(order);
        } else
            retry_cancel(std::move(command), now, true);
        changed(found->first);
        return;
    }
    if (command.encoded.command_kind == Kind::MoveOrder && reply.order_id1.value_or(0) > 0) {
        move_reservations_.erase(found->first);
        order.order_id = *reply.order_id1;
        order.confirmed_by_replication = false;
        order.order_ids.insert(order.order_id);
        order_index_[order.order_id] = found->first;
        order.request.price = command.replacement_price;
        order.request.quantity = command.replacement_quantity;
        order.remaining = std::max<std::int64_t>(0, command.replacement_quantity - order.executed);
        order.state = order.executed ? OrderState::PartFilled : OrderState::Working;
        std::vector<plaza2::private_state::OwnOrderSnapshot> deferred;
        for (auto it = deferred_orders_.begin(); it != deferred_orders_.end();) {
            if (it->first.second == order.order_id) {
                deferred.push_back(std::move(it->second));
                it = deferred_orders_.erase(it);
            } else
                ++it;
        }
        if (!deferred.empty())
            observe_orders(deferred);
        else
            replay_deferred_trades();
        if (order.cancel_requested && !terminal(order.state))
            enqueue_cancel(order);
    } else if (command.encoded.command_kind == Kind::DelUserOrders) {
        order.absence_reply = reply.num_orders && *reply.num_orders == 0;
    }
    if (command.encoded.command_kind == Kind::DelOrder || command.encoded.command_kind == Kind::DelUserOrders) {
        command.acknowledged = true;
        command.deadline = now + config_.reply_timeout;
        pending_.emplace(id, std::move(command));
    }
    if ((command.encoded.command_kind == Kind::AddOrder && reply.order_id.value_or(0) <= 0) ||
        (command.encoded.command_kind == Kind::MoveOrder && reply.order_id1.value_or(0) <= 0)) {
        order.state = OrderState::Unknown;
        order.cancel_requested = true;
        recovery_cancel(order);
    }
    // An accepted cancel is pending until TRADE confirms terminal state.
    changed(found->first);
}

void OrderManager::observe_orders(std::span<const plaza2::private_state::OwnOrderSnapshot> rows, bool rebuilding) {
    std::vector<const plaza2::private_state::OwnOrderSnapshot*> ordered;
    auto observed_session = current_session_;
    const auto has_link = [&](const auto& row) {
        return row.id_ord1 > 0 && (order_index_.contains(row.id_ord1) || terminal_links_.contains(row.id_ord1));
    };
    // Follow the documented relist link first, then settle the old-ID deletion
    // in the same committed transaction without publishing a false terminal state.
    for (const auto& row : rows)
        if (has_link(row))
            ordered.push_back(&row);
    for (const auto& row : rows)
        if (!has_link(row))
            ordered.push_back(&row);
    for (const auto* source : ordered) {
        const auto& row = *source;
        if (row.client_code != config_.broker_code + config_.client_code || row.multileg || row.identity_conflict ||
            (row.dir != 1 && row.dir != 2) || (!row.from_trade_repl && !(rebuilding && row.from_user_book)))
            continue;
        const auto id = row.private_order_id > 0 ? row.private_order_id : row.public_order_id;
        if (id <= 0)
            continue;
        const auto remaining = row.from_trade_repl ? row.public_amount_rest : row.private_amount_rest;
        remember_add_evidence(row);
        if (hold_add_row(row))
            continue;
        if (const auto old = terminal_links_.find(id);
            old != terminal_links_.end() && row.sess_id <= old->second.sess_id)
            continue;
        std::string key;
        if (const auto index = order_index_.find(id); index != order_index_.end())
            key = index->second;
        else if (row.id_ord1 > 0 && order_index_.contains(row.id_ord1)) {
            const auto& linked = orders_.at(order_index_.at(row.id_ord1));
            if (row.sess_id > linked.sess_id && row.isin_id == linked.request.isin_id &&
                row.dir == static_cast<std::int8_t>(linked.request.side))
                key = linked.request.client_order_id;
        }
        if (key.empty() && row.id_ord1 > 0) {
            const auto archived = terminal_links_.find(row.id_ord1);
            if (archived != terminal_links_.end()) {
                const auto& link = archived->second;
                if (row.sess_id > link.sess_id && row.isin_id == link.isin_id &&
                    row.dir == static_cast<std::int8_t>(link.side)) {
                    key = link.key;
                    ManagedOrder restored;
                    restored.request = {.client_order_id = key,
                                        .isin_id = link.isin_id,
                                        .side = link.side,
                                        .price = link.price,
                                        .quantity = link.quantity};
                    restored.ext_id = link.ext_id;
                    restored.sess_id = link.sess_id;
                    restored.executed = link.executed;
                    restored.sent_utc_seconds = link.sent_utc_seconds;
                    restored.cancel_requested = link.cancel_requested;
                    restored.execution_baseline_known = link.execution_baseline_known;
                    restored.order_ids.insert(row.id_ord1);
                    orders_.emplace(key, std::move(restored));
                    order_index_[row.id_ord1] = key;
                    if (link.ext_id != 0)
                        ext_index_.try_emplace(link.ext_id, key);
                    terminal_links_.erase(archived);
                }
            }
        }
        if (key.empty() && row.ext_id != 0 && ext_index_.contains(row.ext_id)) {
            auto& candidate = orders_.at(ext_index_.at(row.ext_id));
            const auto terms = terms_(row.isin_id);
            const bool contract = row.isin_id == candidate.request.isin_id && row.sess_id == candidate.sess_id &&
                                  row.dir == static_cast<std::int8_t>(candidate.request.side) && terms &&
                                  terms->sess_id == row.sess_id;
            if (contract && has_outstanding_command(candidate.request.client_order_id, Kind::MoveOrder)) {
                // ext_id never renames a known order. Save early new-ID TRADE
                // evidence until 176 identifies the actual replacement ID.
                deferred_orders_[{row.sess_id, id}] = row;
                continue;
            }
        }
        if (key.empty()) {
            if (remaining <= 0 && !replaying_add_rows_)
                continue;
            key = "recovered:" + std::to_string(row.sess_id) + ":" + std::to_string(id);
            ManagedOrder recovered;
            recovered.request = {.client_order_id = key,
                                 .isin_id = row.isin_id,
                                 .side = row.dir == 2 ? tr::Plaza2TradeSide::Sell : tr::Plaza2TradeSide::Buy,
                                 .price = row.price,
                                 .comment = row.comment,
                                 .quantity = static_cast<std::int32_t>(std::min<std::int64_t>(
                                     INT32_MAX, std::max({row.public_amount, row.private_amount, remaining})))};
            recovered.ext_id = row.ext_id;
            recovered.sess_id = row.sess_id;
            recovered.execution_baseline_known = false;
            orders_.emplace(key, std::move(recovered));
            used_client_ids_.insert(key);
            // Do not replace another logical order's ext_id reservation.
            if (row.ext_id != 0)
                ext_index_.try_emplace(row.ext_id, key);
        }
        auto& order = orders_.at(key);
        if (row.isin_id != order.request.isin_id || row.dir != static_cast<std::int8_t>(order.request.side) ||
            row.sess_id < order.sess_id)
            continue;
        if (row.sess_id == order.sess_id && order.order_id != id && order.order_ids.contains(id))
            continue;
        observed_session = std::max(observed_session, row.sess_id);
        const bool relisted = row.id_ord1 > 0 && row.sess_id > order.sess_id && order.order_ids.contains(row.id_ord1);
        if (relisted) {
            // A documented next-session identity supersedes any old-session
            // replacement. Its late reply must never roll this identity back.
            const auto old_move = [&](const Command& command) {
                return command.key == key && command.encoded.command_kind == Kind::MoveOrder;
            };
            std::erase_if(adds_, old_move);
            std::erase_if(pending_, [&](const auto& entry) { return old_move(entry.second); });
            move_reservations_.erase(key);
        }
        const auto before = order_json(key, order);
        const auto action = row.from_trade_repl ? row.public_action : row.private_action;
        const bool replacing_current_id = order.order_id == id && has_outstanding_command(key, Kind::MoveOrder);
        const bool unresolved_replacement =
            order.order_id == id && order.state == OrderState::Unknown && move_reservations_.contains(key);
        const bool posted_move = std::any_of(pending_.begin(), pending_.end(), [&](const auto& item) {
            return item.second.key == key && item.second.encoded.command_kind == Kind::MoveOrder;
        });
        order.sess_id = row.sess_id;
        if (order.order_id != id)
            order.operator_action_required = false;
        order.order_id = id;
        order.confirmed_by_replication = true;
        order.order_ids.insert(id);
        order_index_[id] = key;
        config_.next_ext_id = std::max(config_.next_ext_id, row.ext_id == INT32_MAX ? INT32_MAX : row.ext_id + 1);
        order.remaining = std::max<std::int64_t>(0, remaining);
        if (unresolved_replacement)
            order.state = OrderState::Unknown; // Old-ID deletion does not prove the replacement absent.
        else if (replacing_current_id && (remaining > 0 || posted_move))
            order.state = OrderState::PendingReplace;
        else if (remaining <= 0)
            order.state = action == 2 ? OrderState::Filled : OrderState::Cancelled;
        else if (order.operator_action_required)
            order.state = OrderState::Unknown;
        else if (order.cancel_requested)
            order.state = OrderState::PendingCancel;
        else
            order.state = order.executed > 0 ? OrderState::PartFilled : OrderState::Working;
        if (terminal(order.state))
            order.operator_action_required = false;
        if (before != order_json(key, order))
            changed(key);
        else
            refresh_exposure(key);
        if (order.cancel_requested && !terminal(order.state))
            enqueue_cancel(order);
    }
    replay_deferred_trades();
    std::uint64_t sequence{};
    for (const auto& row : rows)
        if (row.from_trade_repl)
            sequence = std::max(sequence, row.trade_repl_commit_sequence);
    observe_trade_commit(sequence);
    advance_session(observed_session);
    if (!replaying_add_rows_)
        prune_terminal();
}
void OrderManager::observe_trade_commit(std::uint64_t sequence) {
    trade_commit_sequence_ = std::max(trade_commit_sequence_, sequence);
    std::vector<std::int32_t> reconciled;
    for (const auto& [isin, bulk] : bulk_cancellations_)
        if (!bulk.awaiting_reply && sequence > bulk.after_commit_sequence)
            reconciled.push_back(isin);
    for (const auto isin : reconciled) {
        const auto generation = bulk_cancellations_.at(isin).generation;
        bulk_cancellations_.erase(isin);
        const auto reconciled_command = [&](const Command& command) {
            return command.bulk_generation == generation && command.encoded.isin_id == isin;
        };
        std::erase_if(pending_, [&](const auto& entry) { return reconciled_command(entry.second); });
        std::erase_if(cancels_, reconciled_command);
        for (auto& [key, order] : orders_)
            if (order.request.isin_id == isin && order.cancel_requested && !terminal(order.state))
                enqueue_cancel(order);
    }
}
void OrderManager::replay_deferred_trades() {
    if (deferred_trades_.empty())
        return;
    std::vector<plaza2::private_state::OwnTradeSnapshot> deferred;
    for (const auto& [key, trade] : deferred_trades_)
        deferred.push_back(trade);
    observe_trades(deferred);
}
void OrderManager::observe_trades(std::span<const plaza2::private_state::OwnTradeSnapshot> rows) {
    for (const auto& row : rows) {
        if (row.multileg || row.amount <= 0)
            continue;
        bool owned_unmatched{};
        for (const bool buy : {true, false}) {
            const auto code = buy ? row.code_buy : row.code_sell;
            const auto id =
                buy ? (row.private_order_id_buy > 0 ? row.private_order_id_buy : row.public_order_id_buy)
                    : (row.private_order_id_sell > 0 ? row.private_order_id_sell : row.public_order_id_sell);
            if (code != config_.broker_code + config_.client_code || id <= 0 ||
                deals_.contains({row.sess_id, row.id_deal, buy}))
                continue;
            const auto index = order_index_.find(id);
            if (index == order_index_.end()) {
                owned_unmatched = true;
                continue;
            }
            auto& order = orders_.at(index->second);
            if (row.isin_id != order.request.isin_id || buy != (order.request.side == tr::Plaza2TradeSide::Buy))
                continue;
            if (order.executed > INT64_MAX - row.amount || filled_by_id_[id] > INT64_MAX - row.amount)
                throw std::overflow_error("own trade quantity overflow");
            order.executed += row.amount;
            filled_by_id_[id] += row.amount;
            if (!order.confirmed_by_replication)
                order.remaining = std::max<std::int64_t>(0, std::int64_t(order.request.quantity) - order.executed);
            if (order.state == OrderState::Working)
                order.state = OrderState::PartFilled;
            emit("trade", "{\"client_order_id\":" + json_string(index->second) + ",\"order_id\":" + std::to_string(id) +
                              ",\"deal_id\":" + std::to_string(row.id_deal) + ",\"quantity\":" +
                              std::to_string(row.amount) + ",\"price\":" + json_string(row.price) + "}");
            changed(index->second);
            deals_.insert({row.sess_id, row.id_deal, buy});
        }
        if (owned_unmatched)
            deferred_trades_[{row.sess_id, row.id_deal}] = row;
        else
            deferred_trades_.erase({row.sess_id, row.id_deal});
    }
}
void OrderManager::prove_absence(std::int64_t server_time, bool online) {
    if (!online)
        return;
    for (auto it = unknown_orders_.begin(); it != unknown_orders_.end();) {
        const auto key = *it++;
        auto& order = orders_.at(key);
        if (order.order_id == 0 && order.absence_reply && order.sent_utc_seconds > 0 &&
            server_time > order.sent_utc_seconds + config_.absence_margin.count()) {
            order.state = OrderState::Cancelled;
            order.remaining = 0;
            order.last_error = "NotFound after TRADE watermark and DelUserOrders num_orders=0";
            order.operator_action_required = false;
            changed(key);
        }
    }
    prune_terminal();
}
} // namespace moex::connector_host
