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
           ",\"price\":" + json_string(order.request.price) + ",\"error\":" + json_string(order.last_error) + "}";
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
        config_.risk.max_open_orders == 0 || config_.next_ext_id <= 0 || config_.next_user_id == 0)
        throw std::invalid_argument("invalid order manager configuration");
}
void OrderManager::emit(std::string_view kind, std::string_view fields) const {
    if (log_)
        log_(kind, fields);
}
void OrderManager::changed(const std::string& key) {
    emit("order", order_json(key, orders_.at(key)));
}
bool OrderManager::has_outstanding_command(std::string_view key, Kind kind) const {
    const auto same = [&](const Command& command) {
        return command.key == key && command.encoded.command_kind == kind;
    };
    return std::any_of(adds_.begin(), adds_.end(), same) || std::any_of(cancels_.begin(), cancels_.end(), same) ||
           std::any_of(pending_.begin(), pending_.end(), [&](const auto& item) { return same(item.second); });
}

std::string OrderManager::check_risk(const OrderRequest& request, std::size_t extra) const {
    if (config_.risk.kill_switch)
        return "kill switch enabled";
    if (!ready_(request.isin_id))
        return "order entry not ready";
    if (request.quantity <= 0 || request.quantity > config_.risk.max_quantity)
        return "quantity exceeds configured limit";
    const auto active = static_cast<std::size_t>(
        std::count_if(orders_.begin(), orders_.end(), [](const auto& value) { return !terminal(value.second.state); }));
    if (active + extra > config_.risk.max_open_orders)
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
    const auto absolute = price->units < 0 ? std::uint64_t(-(price->units + 1)) + 1 : std::uint64_t(price->units);
    if (absolute > static_cast<std::uint64_t>(config_.risk.max_notional_scaled / request.quantity))
        return "notional exceeds configured limit";
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
    if (request.client_order_id.empty() || orders_.contains(request.client_order_id))
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
        order.remaining = order.request.quantity;
        const auto key = order.request.client_order_id;
        orders_.emplace(key, std::move(order));
        ext_index_[ext] = key;
        adds_.push_back(std::move(command));
        changed(key);
        return {};
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
}
void OrderManager::enqueue_cancel(ManagedOrder& order) {
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
    order.state = OrderState::PendingCancel;
    changed(key);
}
void OrderManager::recovery_cancel(ManagedOrder& order) {
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
    request.buy_sell = 0;
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
    order.cancel_requested = true;
    const auto unsent_move = [&](const Command& cmd) {
        return cmd.key == client_id && cmd.encoded.command_kind == Kind::MoveOrder;
    };
    std::erase_if(adds_, unsent_move);
    std::erase_if(cancels_, unsent_move);
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
    auto requested = order.request;
    requested.price = price;
    requested.quantity = quantity;
    if (auto error = check_risk(requested, 0); !error.empty())
        return error;
    tr::MoveOrderRequest request;
    request.broker_code = config_.broker_code;
    request.client_code = config_.client_code;
    request.isin_id = order.request.isin_id;
    request.regime = 1;
    request.order_id1 = order.order_id;
    request.amount1 = quantity;
    request.price1 = price;
    request.ext_id1 = order.ext_id;
    request.is_check_limit = 0;
    auto command = encode(request, found->first);
    command.replacement_price = std::move(price);
    command.replacement_quantity = quantity;
    adds_.push_back(std::move(command));
    order.state = OrderState::PendingReplace;
    changed(found->first);
    return {};
}
std::string OrderManager::cancel_all(std::int32_t isin) {
    if (isin <= 0)
        return "cancel-all requires an instrument id";
    std::vector<std::string> unsent;
    for (auto& [key, order] : orders_)
        if (order.request.isin_id == isin && !terminal(order.state)) {
            order.cancel_requested = true;
            unsent.push_back(key);
        }
    for (const auto& key : unsent) {
        const auto unused = cancel(key);
        (void)unused;
    }
    tr::DelUserOrdersRequest request;
    request.broker_code = config_.broker_code;
    request.code = config_.client_code;
    request.buy_sell = 0;
    request.non_system = 0;
    request.base_contract_code = "";
    request.isin_id = isin;
    request.instrument_mask = 1;
    cancels_.push_back(encode(request, ""));
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
        command.user_id = reserve_user_id();
        command.not_before = now + std::chrono::seconds(1);
        command.acknowledged = false;
        emit("reservation", "{\"next_user_id\":" + std::to_string(config_.next_user_id) + "}");
        cancels_.push_back(std::move(command));
        return;
    }
    auto& order = found->second;
    if (terminal(order.state))
        return;
    if (command.encoded.command_kind == Kind::AddOrder || command.encoded.command_kind == Kind::MoveOrder) {
        order.state = OrderState::Unknown;
        order.cancel_requested = true;
        recovery_cancel(order);
    } else {
        // DelOrder and filtered DelUserOrders are idempotent. A new user_id
        // prevents a late reply from completing a newer attempt.
        if (order.order_id > 0)
            enqueue_cancel(order);
        else
            recovery_cancel(order);
        if (!cancels_.empty())
            cancels_.back().not_before = now + std::chrono::seconds(1);
    }
    changed(found->first);
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
        auto& queue = !cancels_.empty() ? cancels_ : adds_;
        auto& command = queue.front();
        if (now < command.not_before)
            break;
        auto found = orders_.find(command.key);
        const bool add_or_move =
            command.encoded.command_kind == Kind::AddOrder || command.encoded.command_kind == Kind::MoveOrder;
        if (found != orders_.end() && terminal(found->second.state)) {
            queue.pop_front();
            continue;
        }
        if (add_or_move && found != orders_.end()) {
            auto proposed = found->second.request;
            if (command.encoded.command_kind == Kind::MoveOrder) {
                proposed.price = command.replacement_price;
                proposed.quantity = command.replacement_quantity;
            }
            const auto error = check_risk(proposed, 0);
            if (!error.empty()) {
                if (!ready_(proposed.isin_id))
                    break; // Preserve queued work through an outage/clearing.
                found->second.last_error = error;
                found->second.state = command.encoded.command_kind == Kind::AddOrder ? OrderState::Rejected
                                                                                     : settled_state(found->second);
                changed(found->first);
                queue.pop_front();
                continue;
            }
        }
        // For cancels, exchange availability is the transport's decision;
        // session/instrument trading state must not prevent risk reduction.
        if (!rate_.admit(milliseconds(now))) {
            emit("throttle", "{\"queued\":" + std::to_string(queued()) + "}");
            break;
        }
        emit("command", "{\"user_id\":" + std::to_string(command.user_id) +
                            ",\"name\":" + json_string(command.encoded.command_name) +
                            ",\"client_order_id\":" + json_string(command.key) +
                            ",\"payload_hex\":" + json_string(tr::bytes_to_hex(command.encoded.payload)) + "}");
        const auto result = send_(command.encoded, command.user_id);
        emit("command_result", "{\"user_id\":" + std::to_string(command.user_id) + ",\"certainty\":" +
                                   std::to_string(static_cast<int>(result.certainty)) + ",\"error\":" +
                                   json_string(result.validation_error.message + result.allocation_error.message +
                                               result.post_error.message) +
                                   "}");
        if (result.certainty == cg::Plaza2SubmissionCertainty::DefinitelyNotSent) {
            command.not_before = now + std::chrono::seconds(1);
            // Definitive local validation failures cannot become an ambiguous Add.
            if (add_or_move && result.validation_error.code == cg::Plaza2ErrorCode::InvalidConfiguration) {
                if (found != orders_.end()) {
                    found->second.state = command.encoded.command_kind == Kind::AddOrder ? OrderState::Rejected
                                                                                         : settled_state(found->second);
                    found->second.last_error = result.validation_error.message;
                    changed(found->first);
                }
                queue.pop_front();
            }
            break;
        }
        auto sent = std::move(command);
        queue.pop_front();
        if (found != orders_.end())
            found->second.sent_utc_seconds = utc_seconds;
        sent.deadline = now + config_.reply_timeout;
        pending_.emplace(sent.user_id, std::move(sent));
        if (result.certainty == cg::Plaza2SubmissionCertainty::PossiblySent) {
            // Reconcile immediately, without resending the command.
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
void OrderManager::on_reply(std::uint32_t id, const tr::Plaza2TradeDecodedReply& reply, Clock::time_point now) {
    const auto pending = pending_.find(id);
    emit("reply", "{\"user_id\":" + std::to_string(id) + ",\"msgid\":" + std::to_string(reply.msgid) +
                      ",\"code\":" + std::to_string(reply.code) + ",\"message\":" + json_string(reply.message) +
                      ",\"num_orders\":" + std::to_string(reply.num_orders.value_or(-1)) + "}");
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
    if (reply.msgid == 99) {
        rate_.penalize(milliseconds(now), static_cast<std::uint32_t>(std::max(0, reply.penalty_remain.value_or(0))));
        // A flood reply rejects the Add conclusively, so callers may submit a
        // new client id. Cancels retain their place in the risk-reduction queue.
        if (command.encoded.command_kind == Kind::AddOrder) {
            if (found != orders_.end() && found->second.order_id == 0) {
                found->second.state = OrderState::Rejected;
                changed(found->first);
            }
        } else {
            command.user_id = reserve_user_id();
            command.not_before = now + std::chrono::milliseconds(std::max(1, reply.penalty_remain.value_or(0)));
            emit("reservation", "{\"next_user_id\":" + std::to_string(config_.next_user_id) + "}");
            cancels_.push_back(std::move(command));
        }
        return;
    }
    if (found == orders_.end()) {
        if (reply.code != 0 || reply.msgid == 100) {
            command.user_id = reserve_user_id();
            command.not_before = now + std::chrono::seconds(1);
            cancels_.push_back(std::move(command));
        }
        return;
    }
    auto& order = found->second;
    if (terminal(order.state))
        return; // Replication wins over a late reply.
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
        } else if (command.encoded.command_kind == Kind::MoveOrder)
            order.state = settled_state(order);
        else {
            order.state = OrderState::Working;
            enqueue_cancel(order);
            if (!cancels_.empty())
                cancels_.back().not_before = now + std::chrono::seconds(1);
        }
        changed(found->first);
        return;
    }
    if (command.encoded.command_kind == Kind::AddOrder && reply.order_id.value_or(0) > 0) {
        order.order_id = *reply.order_id;
        order.order_ids.insert(order.order_id);
        order_index_[order.order_id] = found->first;
        order.state = order.executed ? OrderState::PartFilled : OrderState::Working;
        if (order.cancel_requested)
            enqueue_cancel(order);
    } else if (command.encoded.command_kind == Kind::MoveOrder && reply.order_id1.value_or(0) > 0) {
        order.order_id = *reply.order_id1;
        order.order_ids.insert(order.order_id);
        order_index_[order.order_id] = found->first;
        order.request.price = command.replacement_price;
        order.request.quantity = static_cast<std::int32_t>(order.executed) + command.replacement_quantity;
        order.remaining = command.replacement_quantity;
        order.state = order.executed ? OrderState::PartFilled : OrderState::Working;
        if (order.cancel_requested)
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
    for (const auto& row : rows) {
        if (row.client_code != config_.broker_code + config_.client_code || row.multileg || row.identity_conflict ||
            (!row.from_trade_repl && !(rebuilding && row.from_user_book)))
            continue;
        const auto id = row.private_order_id > 0 ? row.private_order_id : row.public_order_id;
        if (id <= 0)
            continue;
        const auto remaining = row.from_trade_repl ? row.public_amount_rest : row.private_amount_rest;
        std::string key;
        if (const auto index = order_index_.find(id); index != order_index_.end())
            key = index->second;
        else if (row.ext_id != 0 && ext_index_.contains(row.ext_id))
            key = ext_index_.at(row.ext_id);
        else {
            if (remaining <= 0)
                continue;
            key = "recovered:" + std::to_string(row.sess_id) + ":" + std::to_string(id);
            ManagedOrder recovered;
            recovered.request = {.client_order_id = key,
                                 .isin_id = row.isin_id,
                                 .side = row.dir == 2 ? tr::Plaza2TradeSide::Sell : tr::Plaza2TradeSide::Buy,
                                 .price = row.price,
                                 .comment = row.comment,
                                 .quantity =
                                     static_cast<std::int32_t>(std::max(row.public_amount, row.private_amount))};
            recovered.ext_id = row.ext_id;
            orders_.emplace(key, std::move(recovered));
            if (row.ext_id != 0)
                ext_index_[row.ext_id] = key;
        }
        auto& order = orders_.at(key);
        if (row.isin_id != order.request.isin_id || row.sess_id < order.sess_id)
            continue;
        if (row.sess_id == order.sess_id && order.order_id != id && order.order_ids.contains(id))
            continue;
        if (row.from_trade_repl) {
            // A matching own TRADE row proves that Add reached the exchange,
            // even when its asynchronous reply was lost. Keeping that Add in
            // pending_ would later turn a confirmed order into timeout recovery.
            std::erase_if(pending_, [&](const auto& item) {
                return item.second.key == key && item.second.encoded.command_kind == Kind::AddOrder;
            });
        }
        const auto before = order_json(key, order);
        const auto action = row.from_trade_repl ? row.public_action : row.private_action;
        const bool replacing_current_id = order.order_id == id && has_outstanding_command(key, Kind::MoveOrder);
        const bool posted_move = std::any_of(pending_.begin(), pending_.end(), [&](const auto& item) {
            return item.second.key == key && item.second.encoded.command_kind == Kind::MoveOrder;
        });
        order.sess_id = row.sess_id;
        order.order_id = id;
        order.order_ids.insert(id);
        order_index_[id] = key;
        config_.next_ext_id = std::max(config_.next_ext_id, row.ext_id == INT32_MAX ? INT32_MAX : row.ext_id + 1);
        order.remaining = std::max<std::int64_t>(0, remaining);
        if (action == 2 || remaining > 0)
            order.executed = std::max(order.executed, std::int64_t(order.request.quantity) - order.remaining);
        // Cached old-ID rows are repeated after unrelated commits. They do not
        // complete a Move, and its old-ID cancellation can precede the reply or
        // the new-ID row. Keep that replacement unresolved until either arrives.
        if (replacing_current_id && (remaining > 0 || (posted_move && action != 2)))
            order.state = OrderState::PendingReplace;
        else if (remaining <= 0)
            order.state =
                action == 2 || order.executed >= order.request.quantity ? OrderState::Filled : OrderState::Cancelled;
        else
            order.state = order.executed > 0 ? OrderState::PartFilled : OrderState::Working;
        if (before != order_json(key, order))
            changed(key);
        if (order.cancel_requested && !terminal(order.state))
            enqueue_cancel(order);
    }
}
void OrderManager::observe_trades(std::span<const plaza2::private_state::OwnTradeSnapshot> rows) {
    for (const auto& row : rows) {
        if (row.multileg || deals_.contains({row.sess_id, row.id_deal}))
            continue;
        // Attribute by exchange order id AND participant, never ext_id alone.
        bool attributed{};
        for (const bool buy : {true, false}) {
            const auto code = buy ? row.code_buy : row.code_sell;
            const auto id =
                buy ? (row.private_order_id_buy > 0 ? row.private_order_id_buy : row.public_order_id_buy)
                    : (row.private_order_id_sell > 0 ? row.private_order_id_sell : row.public_order_id_sell);
            const auto index = order_index_.find(id);
            if (code != config_.broker_code + config_.client_code || index == order_index_.end())
                continue;
            auto& order = orders_.at(index->second);
            if (row.isin_id != order.request.isin_id)
                continue;
            // Remaining quantity comes from orders_log; deals are independently
            // logged and cannot double-count that cumulative executed quantity.
            emit("trade", "{\"client_order_id\":" + json_string(index->second) + ",\"order_id\":" + std::to_string(id) +
                              ",\"deal_id\":" + std::to_string(row.id_deal) + ",\"quantity\":" +
                              std::to_string(row.amount) + ",\"price\":" + json_string(row.price) + "}");
            attributed = true;
        }
        if (attributed)
            deals_.insert({row.sess_id, row.id_deal});
    }
}
void OrderManager::prove_absence(std::int64_t server_time, bool online) {
    if (!online)
        return;
    for (auto& [key, order] : orders_)
        if (order.state == OrderState::Unknown && order.order_id == 0 && order.absence_reply &&
            order.sent_utc_seconds > 0 && server_time > order.sent_utc_seconds + config_.absence_margin.count()) {
            order.state = OrderState::Cancelled;
            order.remaining = 0;
            order.last_error = "NotFound after TRADE watermark and DelUserOrders num_orders=0";
            changed(key);
        }
}
} // namespace moex::connector_host
