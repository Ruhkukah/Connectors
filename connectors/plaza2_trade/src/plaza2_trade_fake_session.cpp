#include "moex/plaza2_trade/plaza2_trade_fake_session.hpp"

#include "plaza2_generated_metadata.hpp"

#include <algorithm>
#include <charconv>
#include <type_traits>

namespace moex::plaza2_trade {

namespace {

namespace fake = moex::plaza2::fake;
namespace generated = moex::plaza2::generated;

using FieldCode = generated::FieldCode;
using StreamCode = generated::StreamCode;
using TableCode = generated::TableCode;

constexpr std::int32_t kSyntheticSessionId = 321;
constexpr std::string_view kSyntheticLogin = "fake_plaza2";

std::string_view store_text(Plaza2TradeFakeReplicationBatch& batch, std::string value) {
    batch.text_storage->push_back(std::move(value));
    return batch.text_storage->back();
}

void append_i64(Plaza2TradeFakeReplicationBatch& batch, FieldCode code, std::int64_t value) {
    batch.fields.push_back({
        .field_code = code,
        .kind = fake::ValueKind::kSignedInteger,
        .signed_value = value,
    });
}

void append_u64(Plaza2TradeFakeReplicationBatch& batch, FieldCode code, std::uint64_t value) {
    batch.fields.push_back({
        .field_code = code,
        .kind = fake::ValueKind::kUnsignedInteger,
        .unsigned_value = value,
    });
}

void append_text(Plaza2TradeFakeReplicationBatch& batch, FieldCode code, std::string value) {
    batch.fields.push_back({
        .field_code = code,
        .kind = fake::ValueKind::kString,
        .text_value = store_text(batch, std::move(value)),
    });
}

void finish_row(Plaza2TradeFakeReplicationBatch& batch, StreamCode stream, TableCode table, std::uint32_t first_field) {
    batch.rows.push_back({
        .stream_code = stream,
        .table_code = table,
        .first_field_index = first_field,
        .field_count = static_cast<std::uint32_t>(batch.fields.size() - first_field),
    });
}

void append_orders_log_row(Plaza2TradeFakeReplicationBatch& batch, const Plaza2TradeFakeOrder& order,
                           std::uint64_t repl_id, std::uint64_t moment, std::int64_t id_deal = 0) {
    const auto first = static_cast<std::uint32_t>(batch.fields.size());
    append_u64(batch, FieldCode::kFortsTradeReplOrdersLogReplId, repl_id);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogPublicOrderId, order.synthetic_order_id + 1000000);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogSessId, kSyntheticSessionId);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogIsinId, order.instrument_id);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogPublicAmount, order.quantity);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogPublicAmountRest, order.remaining_quantity);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogIdDeal, id_deal);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogXstatus, static_cast<std::int64_t>(order.status));
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogXstatus2, 0);
    append_text(batch, FieldCode::kFortsTradeReplOrdersLogPrice, order.price);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogMoment, static_cast<std::int64_t>(moment));
    append_u64(batch, FieldCode::kFortsTradeReplOrdersLogMomentNs, 0);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogDir, static_cast<std::int64_t>(order.side));
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogPublicAction, order.remaining_quantity == 0 ? 2 : 1);
    append_text(batch, FieldCode::kFortsTradeReplOrdersLogClientCode, order.client_code);
    append_text(batch, FieldCode::kFortsTradeReplOrdersLogLoginFrom, std::string(kSyntheticLogin));
    append_text(batch, FieldCode::kFortsTradeReplOrdersLogComment, "fake-session");
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogExtId, order.client_transaction_id);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogPrivateOrderId, order.synthetic_order_id);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogPrivateAmount, order.quantity);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogPrivateAmountRest, order.remaining_quantity);
    append_i64(batch, FieldCode::kFortsTradeReplOrdersLogPrivateAction, order.remaining_quantity == 0 ? 2 : 1);
    finish_row(batch, StreamCode::kFortsTradeRepl, TableCode::kFortsTradeReplOrdersLog, first);
}

void append_user_order_row(Plaza2TradeFakeReplicationBatch& batch, const Plaza2TradeFakeOrder& order,
                           std::uint64_t repl_id, std::uint64_t moment) {
    const auto first = static_cast<std::uint32_t>(batch.fields.size());
    append_u64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayReplId, repl_id);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicOrderId,
               order.synthetic_order_id + 1000000);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdaySessId, kSyntheticSessionId);
    append_text(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayClientCode, order.client_code);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayMoment, static_cast<std::int64_t>(moment));
    append_u64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayMomentNs, 0);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayXstatus,
               static_cast<std::int64_t>(order.status));
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayXstatus2, 0);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicAction,
               order.remaining_quantity == 0 ? 2 : 1);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayIsinId, order.instrument_id);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayDir, static_cast<std::int64_t>(order.side));
    append_text(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrice, order.price);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicAmount, order.quantity);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicAmountRest, order.remaining_quantity);
    append_text(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayComment, "fake-session");
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayExtId, order.client_transaction_id);
    append_text(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayLoginFrom, std::string(kSyntheticLogin));
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateOrderId, order.synthetic_order_id);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateAmount, order.quantity);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateAmountRest, order.remaining_quantity);
    append_i64(batch, FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateAction,
               order.remaining_quantity == 0 ? 2 : 1);
    finish_row(batch, StreamCode::kFortsUserorderbookRepl, TableCode::kFortsUserorderbookReplOrdersCurrentday, first);
}

void append_user_deal_row(Plaza2TradeFakeReplicationBatch& batch, const Plaza2TradeFakeOrder& order,
                          std::int64_t deal_id, std::int64_t fill_quantity, std::uint64_t repl_id,
                          std::uint64_t moment) {
    const auto first = static_cast<std::uint32_t>(batch.fields.size());
    append_u64(batch, FieldCode::kFortsTradeReplUserDealReplId, repl_id);
    append_i64(batch, FieldCode::kFortsTradeReplUserDealSessId, kSyntheticSessionId);
    append_i64(batch, FieldCode::kFortsTradeReplUserDealIsinId, order.instrument_id);
    append_i64(batch, FieldCode::kFortsTradeReplUserDealIdDeal, deal_id);
    append_i64(batch, FieldCode::kFortsTradeReplUserDealXamount, fill_quantity);
    append_i64(batch, FieldCode::kFortsTradeReplUserDealPublicOrderIdBuy,
               order.side == Plaza2TradeSide::Buy ? order.synthetic_order_id + 1000000 : 7777);
    append_i64(batch, FieldCode::kFortsTradeReplUserDealPublicOrderIdSell,
               order.side == Plaza2TradeSide::Sell ? order.synthetic_order_id + 1000000 : 7777);
    append_text(batch, FieldCode::kFortsTradeReplUserDealPrice, order.price);
    append_i64(batch, FieldCode::kFortsTradeReplUserDealMoment, static_cast<std::int64_t>(moment));
    append_u64(batch, FieldCode::kFortsTradeReplUserDealMomentNs, 0);
    append_text(batch, FieldCode::kFortsTradeReplUserDealCodeBuy,
                order.side == Plaza2TradeSide::Buy ? order.client_code : "MMF");
    append_text(batch, FieldCode::kFortsTradeReplUserDealCodeSell,
                order.side == Plaza2TradeSide::Sell ? order.client_code : "MMF");
    append_text(batch, FieldCode::kFortsTradeReplUserDealCommentBuy,
                order.side == Plaza2TradeSide::Buy ? "fake-fill" : "maker");
    append_text(batch, FieldCode::kFortsTradeReplUserDealCommentSell,
                order.side == Plaza2TradeSide::Sell ? "fake-fill" : "maker");
    append_text(batch, FieldCode::kFortsTradeReplUserDealLoginBuy,
                order.side == Plaza2TradeSide::Buy ? std::string(kSyntheticLogin) : "maker");
    append_text(batch, FieldCode::kFortsTradeReplUserDealLoginSell,
                order.side == Plaza2TradeSide::Sell ? std::string(kSyntheticLogin) : "maker");
    append_i64(batch, FieldCode::kFortsTradeReplUserDealPrivateOrderIdBuy,
               order.side == Plaza2TradeSide::Buy ? order.synthetic_order_id : 0);
    append_i64(batch, FieldCode::kFortsTradeReplUserDealPrivateOrderIdSell,
               order.side == Plaza2TradeSide::Sell ? order.synthetic_order_id : 0);
    finish_row(batch, StreamCode::kFortsTradeRepl, TableCode::kFortsTradeReplUserDeal, first);
}

void add_stream_event(Plaza2TradeFakeReplicationBatch& batch, StreamCode stream, TableCode table,
                      std::uint32_t first_row, std::uint32_t row_count) {
    batch.events.push_back({
        .kind = fake::EventKind::kStreamData,
        .stream_code = stream,
        .table_code = table,
        .first_row_index = first_row,
        .row_count = row_count,
    });
}

Plaza2TradeFakeReplicationBatch make_batch(std::string scenario_id) {
    Plaza2TradeFakeReplicationBatch batch;
    batch.streams = {StreamCode::kFortsTradeRepl, StreamCode::kFortsUserorderbookRepl};
    batch.events.push_back({.kind = fake::EventKind::kOpen});
    batch.events.push_back({.kind = fake::EventKind::kSnapshotBegin});
    batch.events.push_back({.kind = fake::EventKind::kSnapshotEnd});
    batch.events.push_back({.kind = fake::EventKind::kOnline});
    batch.events.push_back({.kind = fake::EventKind::kTransactionBegin});
    batch.scenario = {
        .scenario_id = store_text(batch, std::move(scenario_id)),
        .description = store_text(batch, "Phase 5C fake transactional confirmation"),
        .metadata_version = 1,
        .deterministic_seed = 0,
        .first_stream_index = 0,
        .stream_count = static_cast<std::uint32_t>(batch.streams.size()),
    };
    return batch;
}

void finish_batch(Plaza2TradeFakeReplicationBatch& batch) {
    batch.events.push_back({.kind = fake::EventKind::kTransactionCommit});
    batch.scenario.event_count = static_cast<std::uint32_t>(batch.events.size());
}

void append_order_confirmation(Plaza2TradeFakeReplicationBatch& batch, const Plaza2TradeFakeOrder& order,
                               std::uint64_t& repl_id, std::uint64_t& moment) {
    auto first = static_cast<std::uint32_t>(batch.rows.size());
    append_orders_log_row(batch, order, repl_id++, moment++);
    add_stream_event(batch, StreamCode::kFortsTradeRepl, TableCode::kFortsTradeReplOrdersLog, first, 1);

    first = static_cast<std::uint32_t>(batch.rows.size());
    append_user_order_row(batch, order, repl_id++, moment++);
    add_stream_event(batch, StreamCode::kFortsUserorderbookRepl, TableCode::kFortsUserorderbookReplOrdersCurrentday,
                     first, 1);
}

template <typename Request> std::int64_t ext_id_or_zero(const Request& request) {
    if constexpr (requires { request.ext_id; }) {
        return request.ext_id.value_or(0);
    }
    return 0;
}

template <typename Request> std::string client_code_or_empty(const Request& request) {
    if constexpr (requires { request.client_code; }) {
        return request.client_code.value_or("");
    } else if constexpr (requires { request.code; }) {
        return request.code.value_or("");
    }
    return "";
}

template <typename Request> std::int32_t isin_id_or_zero(const Request& request) {
    if constexpr (requires { request.isin_id; }) {
        return request.isin_id.value_or(0);
    }
    return 0;
}

template <typename Request> std::string price_or_empty(const Request& request) {
    if constexpr (requires { request.price; }) {
        return request.price.value_or("");
    }
    return "";
}

template <typename Request> std::int64_t amount_or_zero(const Request& request) {
    if constexpr (requires { request.amount; }) {
        return request.amount.value_or(0);
    } else if constexpr (requires { request.iceberg_amount; }) {
        return request.iceberg_amount.value_or(0);
    }
    return 0;
}

template <typename Request> Plaza2TradeSide side_or_buy(const Request& request) {
    if constexpr (requires { request.dir; }) {
        return request.dir.value_or(Plaza2TradeSide::Buy);
    }
    return Plaza2TradeSide::Buy;
}

std::int64_t correlation_for(const Plaza2TradeCommandRequest& request) {
    return std::visit(
        [](const auto& value) -> std::int64_t {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, MoveOrderRequest>) {
                return value.ext_id1.value_or(0);
            } else if constexpr (std::is_same_v<T, CODHeartbeatRequest>) {
                return value.seq_number.value_or(0);
            } else {
                return ext_id_or_zero(value);
            }
        },
        request);
}

std::int32_t primary_reply_msgid(Plaza2TradeCommandKind kind) {
    switch (kind) {
    case Plaza2TradeCommandKind::AddOrder:
        return 179;
    case Plaza2TradeCommandKind::IcebergAddOrder:
        return 180;
    case Plaza2TradeCommandKind::DelOrder:
        return 177;
    case Plaza2TradeCommandKind::IcebergDelOrder:
        return 182;
    case Plaza2TradeCommandKind::MoveOrder:
        return 176;
    case Plaza2TradeCommandKind::IcebergMoveOrder:
        return 181;
    case Plaza2TradeCommandKind::DelUserOrders:
        return 186;
    case Plaza2TradeCommandKind::DelOrdersByBFLimit:
        return 172;
    case Plaza2TradeCommandKind::CODHeartbeat:
        return 10000;
    }
    return 0;
}

Plaza2TradeDecodedReply make_reply(std::int32_t msgid, Plaza2TradeFakeOutcomeStatus status, std::string message,
                                   std::optional<std::int64_t> order_id = std::nullopt,
                                   std::optional<std::int32_t> count = std::nullopt) {
    Plaza2TradeDecodedReply reply;
    reply.msgid = msgid;
    reply.message_name = msgid == 10000 ? "CODHeartbeat" : "FORTS_MSG" + std::to_string(msgid);
    reply.status = status == Plaza2TradeFakeOutcomeStatus::Accepted ? Plaza2TradeReplyStatusCategory::Accepted
                                                                    : Plaza2TradeReplyStatusCategory::Rejected;
    reply.code = status == Plaza2TradeFakeOutcomeStatus::Accepted ? 0 : -1;
    reply.message = std::move(message);
    if (order_id) {
        if (msgid == 180) {
            reply.iceberg_order_id = *order_id;
        } else {
            reply.order_id = *order_id;
            reply.order_id1 = *order_id;
        }
    }
    if (count) {
        reply.amount = *count;
        reply.num_orders = *count;
    }
    return reply;
}

bool is_terminal(Plaza2TradeFakeOrderStatus status) {
    return status == Plaza2TradeFakeOrderStatus::Canceled || status == Plaza2TradeFakeOrderStatus::Filled ||
           status == Plaza2TradeFakeOrderStatus::Rejected;
}

Plaza2TradeFakeSubmitResult make_result(Plaza2TradeCommandKind kind, std::int64_t correlation,
                                        Plaza2TradeFakeOutcomeStatus status, std::string diagnostic) {
    Plaza2TradeFakeSubmitResult result;
    result.command_family = kind;
    result.client_transaction_id = correlation;
    result.reply_msgid = primary_reply_msgid(kind);
    result.status = status;
    result.diagnostic = std::move(diagnostic);
    result.decoded_reply = make_reply(result.reply_msgid, status, result.diagnostic);
    return result;
}

} // namespace

bool Plaza2TradeFakeReplicationBatch::empty() const noexcept {
    return rows.empty();
}

fake::ScenarioDataView Plaza2TradeFakeReplicationBatch::view() const {
    return {
        .scenario = scenario,
        .streams = std::span<const StreamCode>(streams.data(), streams.size()),
        .events = std::span<const fake::EventSpec>(events.data(), events.size()),
        .rows = std::span<const fake::RowSpec>(rows.data(), rows.size()),
        .fields = std::span<const fake::FieldValueSpec>(fields.data(), fields.size()),
        .invariants = std::span<const fake::InvariantSpec>(invariants.data(), invariants.size()),
    };
}

Plaza2TradeFakeSession::Plaza2TradeFakeSession() = default;

void Plaza2TradeFakeSession::establish() {
    state_ = Plaza2TradeFakeSessionState::Established;
}

void Plaza2TradeFakeSession::disconnect() {
    state_ = Plaza2TradeFakeSessionState::Disconnected;
}

void Plaza2TradeFakeSession::recover() {
    state_ = Plaza2TradeFakeSessionState::Recovering;
}

void Plaza2TradeFakeSession::terminate() {
    state_ = Plaza2TradeFakeSessionState::Terminated;
}

Plaza2TradeFakeSessionState Plaza2TradeFakeSession::state() const noexcept {
    return state_;
}

std::span<const Plaza2TradeFakeOrder> Plaza2TradeFakeSession::orders() const noexcept {
    return orders_;
}

Plaza2TradeFakeSubmitResult Plaza2TradeFakeSession::submit(const Plaza2TradeCommandRequest& request) {
    const auto kind = command_kind(request);
    const auto correlation = correlation_for(request);
    Plaza2TradeFakeSubmitResult result;
    result.command_family = kind;
    result.client_transaction_id = correlation;
    result.reply_msgid = primary_reply_msgid(kind);
    result.encoded_command = codec_.encode(request);

    if (state_ != Plaza2TradeFakeSessionState::Established) {
        result.status = Plaza2TradeFakeOutcomeStatus::InvalidState;
        result.diagnostic = "fake session is not established";
        result.decoded_reply = make_reply(result.reply_msgid, result.status, result.diagnostic);
        return result;
    }
    if (!result.encoded_command.validation.ok()) {
        result.status = Plaza2TradeFakeOutcomeStatus::ValidationFailed;
        result.diagnostic =
            result.encoded_command.validation.field_name + ": " + result.encoded_command.validation.message;
        result.decoded_reply = make_reply(result.reply_msgid, result.status, result.diagnostic);
        return result;
    }
    if (correlation != 0 && std::find(seen_client_transaction_ids_.begin(), seen_client_transaction_ids_.end(),
                                      correlation) != seen_client_transaction_ids_.end()) {
        result.status = Plaza2TradeFakeOutcomeStatus::DuplicateClientTransactionId;
        result.diagnostic = "duplicate client transaction id";
        result.decoded_reply = make_reply(result.reply_msgid, result.status, result.diagnostic);
        return result;
    }

    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            auto accepted = make_result(kind, correlation, Plaza2TradeFakeOutcomeStatus::Accepted, "accepted");
            if constexpr (std::is_same_v<T, AddOrderRequest> || std::is_same_v<T, IcebergAddOrderRequest>) {
                if (correlation != 0) {
                    seen_client_transaction_ids_.push_back(correlation);
                }
                Plaza2TradeFakeOrder order;
                order.synthetic_order_id = next_order_id_++;
                order.client_transaction_id = correlation;
                order.instrument_id = isin_id_or_zero(value);
                order.side = side_or_buy(value);
                order.price = price_or_empty(value);
                order.quantity = amount_or_zero(value);
                order.remaining_quantity = order.quantity;
                order.client_code = client_code_or_empty(value);
                order.original_command_family = kind;
                order.last_command_family = kind;
                orders_.push_back(order);
                accepted.generated_order_id = order.synthetic_order_id;
                accepted.decoded_reply =
                    make_reply(result.reply_msgid, accepted.status, accepted.diagnostic, order.synthetic_order_id);
                accepted.replication = make_batch("phase5c_add_order_accept");
                append_order_confirmation(accepted.replication, orders_.back(), next_repl_id_, next_moment_);
                finish_batch(accepted.replication);
                return accepted;
            } else if constexpr (std::is_same_v<T, DelOrderRequest> || std::is_same_v<T, IcebergDelOrderRequest>) {
                auto it = std::find_if(orders_.begin(), orders_.end(),
                                       [&](const auto& order) { return order.synthetic_order_id == *value.order_id; });
                if (it == orders_.end()) {
                    return make_result(kind, correlation, Plaza2TradeFakeOutcomeStatus::UnknownOrder, "unknown order");
                }
                if (is_terminal(it->status)) {
                    return make_result(kind, correlation, Plaza2TradeFakeOutcomeStatus::Rejected,
                                       "order is already terminal");
                }
                it->status = Plaza2TradeFakeOrderStatus::Canceled;
                it->remaining_quantity = 0;
                it->last_command_family = kind;
                accepted.generated_order_id = it->synthetic_order_id;
                accepted.decoded_reply =
                    make_reply(result.reply_msgid, accepted.status, accepted.diagnostic, it->synthetic_order_id, 1);
                accepted.replication = make_batch("phase5c_del_order_accept");
                append_order_confirmation(accepted.replication, *it, next_repl_id_, next_moment_);
                finish_batch(accepted.replication);
                return accepted;
            } else if constexpr (std::is_same_v<T, MoveOrderRequest>) {
                auto it = std::find_if(orders_.begin(), orders_.end(),
                                       [&](const auto& order) { return order.synthetic_order_id == *value.order_id1; });
                if (it == orders_.end()) {
                    return make_result(kind, correlation, Plaza2TradeFakeOutcomeStatus::UnknownOrder, "unknown order");
                }
                if (is_terminal(it->status)) {
                    return make_result(kind, correlation, Plaza2TradeFakeOutcomeStatus::Rejected,
                                       "order is already terminal");
                }
                it->status = Plaza2TradeFakeOrderStatus::Moved;
                it->price = value.price1.value_or(it->price);
                it->quantity = value.amount1.value_or(static_cast<std::int32_t>(it->quantity));
                it->remaining_quantity = it->quantity;
                it->client_transaction_id = value.ext_id1.value_or(it->client_transaction_id);
                it->last_command_family = kind;
                accepted.generated_order_id = it->synthetic_order_id;
                accepted.decoded_reply =
                    make_reply(result.reply_msgid, accepted.status, accepted.diagnostic, it->synthetic_order_id);
                accepted.replication = make_batch("phase5c_move_order_accept");
                append_order_confirmation(accepted.replication, *it, next_repl_id_, next_moment_);
                finish_batch(accepted.replication);
                return accepted;
            } else if constexpr (std::is_same_v<T, IcebergMoveOrderRequest>) {
                auto it = std::find_if(orders_.begin(), orders_.end(),
                                       [&](const auto& order) { return order.synthetic_order_id == *value.order_id; });
                if (it == orders_.end()) {
                    return make_result(kind, correlation, Plaza2TradeFakeOutcomeStatus::UnknownOrder, "unknown order");
                }
                if (is_terminal(it->status)) {
                    return make_result(kind, correlation, Plaza2TradeFakeOutcomeStatus::Rejected,
                                       "order is already terminal");
                }
                it->status = Plaza2TradeFakeOrderStatus::Moved;
                it->price = value.price.value_or(it->price);
                it->client_transaction_id = value.ext_id.value_or(it->client_transaction_id);
                it->last_command_family = kind;
                accepted.generated_order_id = it->synthetic_order_id;
                accepted.decoded_reply =
                    make_reply(result.reply_msgid, accepted.status, accepted.diagnostic, it->synthetic_order_id);
                accepted.replication = make_batch("phase5c_iceberg_move_order_accept");
                append_order_confirmation(accepted.replication, *it, next_repl_id_, next_moment_);
                finish_batch(accepted.replication);
                return accepted;
            } else if constexpr (std::is_same_v<T, DelUserOrdersRequest>) {
                std::int32_t affected = 0;
                accepted.replication = make_batch("phase5c_del_user_orders_accept");
                for (auto& order : orders_) {
                    if (is_terminal(order.status)) {
                        continue;
                    }
                    if (value.code && order.client_code != *value.code) {
                        continue;
                    }
                    if (value.isin_id && order.instrument_id != *value.isin_id) {
                        continue;
                    }
                    order.status = Plaza2TradeFakeOrderStatus::Canceled;
                    order.remaining_quantity = 0;
                    order.last_command_family = kind;
                    append_order_confirmation(accepted.replication, order, next_repl_id_, next_moment_);
                    ++affected;
                }
                accepted.decoded_reply =
                    make_reply(result.reply_msgid, accepted.status, accepted.diagnostic, std::nullopt, affected);
                finish_batch(accepted.replication);
                return accepted;
            } else if constexpr (std::is_same_v<T, DelOrdersByBFLimitRequest>) {
                return make_result(kind, correlation, Plaza2TradeFakeOutcomeStatus::UnsupportedCommand,
                                   "Phase 5A metadata has only broker_code for this command");
            } else {
                return make_result(kind, correlation, Plaza2TradeFakeOutcomeStatus::Accepted, "heartbeat accepted");
            }
        },
        request);
}

Plaza2TradeFakeSubmitResult Plaza2TradeFakeSession::simulate_fill(std::int64_t order_id, std::int64_t deal_id,
                                                                  std::int64_t fill_quantity) {
    auto it = std::find_if(orders_.begin(), orders_.end(),
                           [&](const auto& order) { return order.synthetic_order_id == order_id; });
    if (it == orders_.end()) {
        return make_result(Plaza2TradeCommandKind::AddOrder, 0, Plaza2TradeFakeOutcomeStatus::UnknownOrder,
                           "unknown order");
    }
    if (is_terminal(it->status)) {
        return make_result(it->last_command_family, it->client_transaction_id, Plaza2TradeFakeOutcomeStatus::Rejected,
                           "order is already terminal");
    }
    const auto applied = std::min<std::int64_t>(fill_quantity, it->remaining_quantity);
    it->remaining_quantity -= applied;
    if (it->remaining_quantity == 0) {
        it->status = Plaza2TradeFakeOrderStatus::Filled;
    }

    auto result = make_result(it->last_command_family, it->client_transaction_id,
                              Plaza2TradeFakeOutcomeStatus::Accepted, "fake fill accepted");
    result.generated_order_id = it->synthetic_order_id;
    result.generated_deal_id = deal_id;
    result.replication = make_batch("phase5c_fake_fill");
    auto first = static_cast<std::uint32_t>(result.replication.rows.size());
    append_orders_log_row(result.replication, *it, next_repl_id_++, next_moment_, deal_id);
    add_stream_event(result.replication, StreamCode::kFortsTradeRepl, TableCode::kFortsTradeReplOrdersLog, first, 1);
    first = static_cast<std::uint32_t>(result.replication.rows.size());
    append_user_deal_row(result.replication, *it, deal_id, applied, next_repl_id_++, next_moment_++);
    add_stream_event(result.replication, StreamCode::kFortsTradeRepl, TableCode::kFortsTradeReplUserDeal, first, 1);
    finish_batch(result.replication);
    return result;
}

const char* fake_outcome_name(Plaza2TradeFakeOutcomeStatus status) noexcept {
    switch (status) {
    case Plaza2TradeFakeOutcomeStatus::Accepted:
        return "Accepted";
    case Plaza2TradeFakeOutcomeStatus::Rejected:
        return "Rejected";
    case Plaza2TradeFakeOutcomeStatus::DuplicateClientTransactionId:
        return "DuplicateClientTransactionId";
    case Plaza2TradeFakeOutcomeStatus::UnknownOrder:
        return "UnknownOrder";
    case Plaza2TradeFakeOutcomeStatus::InvalidState:
        return "InvalidState";
    case Plaza2TradeFakeOutcomeStatus::UnsupportedCommand:
        return "UnsupportedCommand";
    case Plaza2TradeFakeOutcomeStatus::ValidationFailed:
        return "ValidationFailed";
    }
    return "Unknown";
}

} // namespace moex::plaza2_trade
