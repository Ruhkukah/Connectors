#include "adapters/alorengine_capi/moex_c_api.h"
#include "moex_c_api_internal.hpp"
#include "plaza2_twime_reconciler_test_support.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using moex::capi_internal::install_private_state_projector;
using moex::capi_internal::install_reconciler_snapshot;
using moex::plaza2::generated::StreamCode;
using moex::plaza2_twime_reconciler::make_plaza_committed_snapshot;
using moex::plaza2_twime_reconciler::Plaza2TwimeReconciler;
using moex::plaza2_twime_reconciler::PlazaOrderInput;
using moex::plaza2_twime_reconciler::PlazaTradeInput;
using moex::plaza2_twime_reconciler::Side;
using moex::plaza2_twime_reconciler::TwimeOrderInput;
using moex::plaza2_twime_reconciler::TwimeOrderInputKind;
using moex::plaza2_twime_reconciler::TwimeTradeInput;
using moex::plaza2_twime_reconciler::TwimeTradeInputKind;
using moex::plaza2_twime_reconciler::test_support::make_ready_plaza_snapshot;
using moex::plaza2_twime_reconciler::test_support::make_twime_health_input;
using moex::plaza2_twime_reconciler::test_support::price_to_mantissa;
using moex::plaza2_twime_reconciler::test_support::project_scenario;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct HandleGuard {
    MoexConnectorHandle handle{nullptr};

    HandleGuard() = default;

    ~HandleGuard() {
        if (handle != nullptr) {
            (void)moex_destroy_connector(handle);
        }
    }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;

    HandleGuard(HandleGuard&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }

    HandleGuard& operator=(HandleGuard&& other) noexcept {
        if (this != &other) {
            if (handle != nullptr) {
                (void)moex_destroy_connector(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
};

HandleGuard make_handle() {
    HandleGuard guard;
    MoexConnectorCreateParams params{};
    params.struct_size = sizeof(params);
    params.abi_version = MOEX_C_ABI_VERSION;
    params.connector_name = "phase4b_read_side_test";
    params.instance_id = "offline";
    const auto result = moex_create_connector(&params, &guard.handle);
    require(result == MOEX_RESULT_OK && guard.handle != nullptr, "connector creation must succeed");
    return guard;
}

TwimeOrderInput make_manual_order(std::uint64_t logical_sequence, std::uint64_t cl_ord_id, std::int64_t order_id,
                                  TwimeOrderInputKind kind, std::string account, std::string_view price_text,
                                  std::uint32_t order_qty, Side side) {
    TwimeOrderInput input;
    input.kind = kind;
    input.logical_sequence = logical_sequence;
    input.cl_ord_id = cl_ord_id;
    input.order_id = order_id;
    input.trading_session_id = 321;
    input.security_id = 1001;
    input.account = std::move(account);
    input.side = side;
    input.has_price = true;
    input.price_mantissa = price_to_mantissa(price_text);
    input.has_order_qty = true;
    input.order_qty = order_qty;
    return input;
}

TwimeTradeInput make_manual_trade(std::uint64_t logical_sequence, std::int64_t order_id, std::int64_t trade_id,
                                  std::string_view price_text, std::uint32_t last_qty, std::uint32_t order_qty,
                                  Side side) {
    TwimeTradeInput input;
    input.kind = TwimeTradeInputKind::Execution;
    input.logical_sequence = logical_sequence;
    input.order_id = order_id;
    input.trade_id = trade_id;
    input.trading_session_id = 321;
    input.security_id = 1001;
    input.side = side;
    input.has_price = true;
    input.price_mantissa = price_to_mantissa(price_text);
    input.has_last_qty = true;
    input.last_qty = last_qty;
    input.has_order_qty = true;
    input.order_qty = order_qty;
    return input;
}

Plaza2TwimeReconciler make_confirmed_reconciler() {
    Plaza2TwimeReconciler reconciler;
    reconciler.update_twime_source_health(make_twime_health_input());
    reconciler.apply_twime_order_input(
        make_manual_order(1, 501, 20003, TwimeOrderInputKind::NewAccepted, "CL001", "102500", 7, Side::Sell));
    reconciler.apply_twime_trade_input(make_manual_trade(2, 20003, 9001, "102500", 2, 7, Side::Sell));
    auto projector = project_scenario("private_state_projection");
    reconciler.apply_plaza_snapshot(make_plaza_committed_snapshot(projector, 50));
    return reconciler;
}

Plaza2TwimeReconciler make_diverged_reconciler() {
    Plaza2TwimeReconciler reconciler;
    reconciler.update_twime_source_health(make_twime_health_input());
    reconciler.apply_twime_order_input(
        make_manual_order(10, 701, 20003, TwimeOrderInputKind::NewAccepted, "CL001", "102600", 7, Side::Sell));
    reconciler.apply_twime_trade_input(make_manual_trade(11, 32001, 91001, "102500", 2, 7, Side::Sell));

    auto snapshot = make_ready_plaza_snapshot(81);
    snapshot.orders = {
        PlazaOrderInput{
            .public_order_id = 10003,
            .private_order_id = 20003,
            .sess_id = 321,
            .isin_id = 1001,
            .client_code = "CL001",
            .login_from = "TRADER",
            .comment = "diverged-order",
            .price_text = "102500",
            .has_price = true,
            .price_mantissa = price_to_mantissa("102500"),
            .public_amount = 7,
            .public_amount_rest = 5,
            .private_amount = 7,
            .private_amount_rest = 4,
            .id_deal = 9001,
            .side = Side::Sell,
            .moment = 1700000009,
            .moment_ns = 1700000009000000000ULL,
        },
    };
    snapshot.trades = {
        PlazaTradeInput{
            .trade_id = 91001,
            .sess_id = 321,
            .isin_id = 1001,
            .price_text = "102500",
            .has_price = true,
            .price_mantissa = price_to_mantissa("102500"),
            .amount = 1,
            .private_order_id_sell = 32001,
            .code_sell = "CL001",
            .comment_sell = "diverged-trade",
            .login_sell = "TRADER",
            .moment = 1700000010,
            .moment_ns = 1700000010000000000ULL,
        },
    };
    reconciler.apply_plaza_snapshot(snapshot);
    return reconciler;
}

void test_unavailable_and_invalid_argument_guards() {
    auto guard = make_handle();

    MoexPlaza2PrivateConnectorHealth private_health{};
    require(moex_get_plaza2_private_connector_health(nullptr, &private_health) == MOEX_RESULT_NULL_POINTER,
            "null handle must be rejected for private health");
    require(moex_get_plaza2_private_connector_health(guard.handle, nullptr) == MOEX_RESULT_NULL_POINTER,
            "null summary pointer must be rejected");
    require(moex_get_plaza2_private_connector_health(guard.handle, &private_health) == MOEX_RESULT_SNAPSHOT_UNAVAILABLE,
            "missing private snapshot must report snapshot unavailable");

    uint32_t count = 0;
    require(moex_get_plaza2_stream_health_count(nullptr, &count) == MOEX_RESULT_NULL_POINTER,
            "null handle must be rejected for count");
    require(moex_get_plaza2_stream_health_count(guard.handle, nullptr) == MOEX_RESULT_NULL_POINTER,
            "null count pointer must be rejected");

    auto projector = project_scenario("private_state_projection");
    require(install_private_state_projector(guard.handle, std::move(projector)) == MOEX_RESULT_OK,
            "private snapshot install must succeed");

    MoexPlaza2PrivateConnectorHealth small{};
    small.struct_size = sizeof(MoexPlaza2PrivateConnectorHealth) - 1U;
    require(moex_get_plaza2_private_connector_health(guard.handle, &small) == MOEX_RESULT_INVALID_ARGUMENT,
            "short struct_size must be rejected");
    require(moex_get_plaza2_stream_health_count(guard.handle, nullptr) == MOEX_RESULT_NULL_POINTER,
            "null count pointer must still be rejected once snapshot is available");
}

void test_private_snapshot_export_and_no_mutation_on_read() {
    auto guard = make_handle();
    auto projector = project_scenario("private_state_projection");
    require(install_private_state_projector(guard.handle, std::move(projector)) == MOEX_RESULT_OK,
            "private projector install must succeed");

    const auto native_stream_count_before = guard.handle->plaza2_private_state->stream_health().size();
    const auto native_order_count_before = guard.handle->plaza2_private_state->own_orders().size();

    MoexPlaza2PrivateConnectorHealth health{};
    require(moex_get_plaza2_private_connector_health(guard.handle, &health) == MOEX_RESULT_OK,
            "private health export must succeed");
    require(health.open == 1U && health.online == 1U, "private health flags must reflect committed snapshot");
    require(health.commit_count == 1U, "private health commit count must be exported");

    MoexPlaza2ResumeMarkers markers{};
    require(moex_get_plaza2_resume_markers(guard.handle, &markers) == MOEX_RESULT_OK,
            "resume markers export must succeed");
    require(markers.has_lifenum == 1U && markers.last_lifenum == 7U, "resume lifenum must be exported");
    require(std::string(markers.last_replstate) == "lifenum=7;rev.orders=10;rev.position=20",
            "resume replstate must be exported");

    uint32_t stream_count = 0;
    require(moex_get_plaza2_stream_health_count(guard.handle, &stream_count) == MOEX_RESULT_OK,
            "stream count must succeed");
    require(stream_count == 5U, "expected five committed private streams");

    std::array<MoexPlaza2StreamHealthItem, 1> too_small_streams{};
    too_small_streams[0].stream_code = 0xDEADBEEF;
    uint32_t required = 0;
    require(moex_copy_plaza2_stream_health_items(guard.handle, too_small_streams.data(),
                                                 static_cast<uint32_t>(too_small_streams.size()),
                                                 &required) == MOEX_RESULT_BUFFER_TOO_SMALL,
            "stream copy must report insufficient capacity");
    require(required == stream_count, "buffer-too-small path must report required count");
    require(too_small_streams[0].stream_code == 0xDEADBEEF, "buffer-too-small path must not mutate caller buffer");

    std::vector<MoexPlaza2StreamHealthItem> streams(stream_count);
    uint32_t written = 0;
    require(moex_copy_plaza2_stream_health_items(guard.handle, streams.data(), stream_count, &written) ==
                MOEX_RESULT_OK,
            "stream copy must succeed with full capacity");
    require(written == stream_count, "stream copy must write all committed items");

    bool found_trade_stream = false;
    for (const auto& item : streams) {
        if (item.stream_code == static_cast<std::uint32_t>(StreamCode::kFortsTradeRepl)) {
            found_trade_stream = true;
            require(item.online == 1U && item.snapshot_complete == 1U, "trade stream flags must be exported");
            require(std::string(item.last_message) == "trade-ready", "trade stream message must be exported");
        }
    }
    require(found_trade_stream, "trade stream must be present in exported stream list");

    uint32_t instrument_count = 0;
    require(moex_get_plaza2_instrument_count(guard.handle, &instrument_count) == MOEX_RESULT_OK,
            "instrument count must succeed");
    require(instrument_count == 3U, "expected three committed instruments");
    std::vector<MoexPlaza2InstrumentItem> instruments(instrument_count);
    require(moex_copy_plaza2_instrument_items(guard.handle, instruments.data(), instrument_count, &written) ==
                MOEX_RESULT_OK,
            "instrument copy must succeed");

    bool found_multileg = false;
    for (const auto& item : instruments) {
        if (item.isin_id == 3001) {
            found_multileg = true;
            require(item.kind == MOEX_PLAZA2_INSTRUMENT_KIND_MULTILEG, "multileg kind must be exported");
            require(item.leg_count == 2U && item.leg1_isin_id == 1001 && item.leg2_isin_id == 2001,
                    "multileg legs must be exported deterministically");
        }
    }
    require(found_multileg, "multileg instrument must be present");

    uint32_t limit_count = 0;
    require(moex_get_plaza2_limit_count(guard.handle, &limit_count) == MOEX_RESULT_OK && limit_count == 1U,
            "limit count must reflect committed snapshot");
    std::vector<MoexPlaza2LimitItem> limits(limit_count);
    require(moex_copy_plaza2_limit_items(guard.handle, limits.data(), limit_count, &written) == MOEX_RESULT_OK,
            "limit copy must succeed");
    require(std::string(limits.front().account_code) == "CL001", "limit account must be exported");
    require(std::string(limits.front().money_free) == "125000.5", "limit money_free must be exported");

    uint32_t order_count = 0;
    require(moex_get_plaza2_own_order_count(guard.handle, &order_count) == MOEX_RESULT_OK && order_count == 2U,
            "own order count must reflect committed snapshot");
    std::vector<MoexPlaza2OwnOrderItem> orders(order_count);
    require(moex_copy_plaza2_own_order_items(guard.handle, orders.data(), order_count, &written) == MOEX_RESULT_OK,
            "own order copy must succeed");
    bool found_merged_order = false;
    for (const auto& item : orders) {
        if (item.private_order_id == 20003) {
            found_merged_order = true;
            require(std::string(item.price_text) == "102500", "merged order price must be exported");
        }
    }
    require(found_merged_order, "merged own order must be exported");

    require(guard.handle->plaza2_private_state->stream_health().size() == native_stream_count_before,
            "read calls must not mutate native private stream state");
    require(guard.handle->plaza2_private_state->own_orders().size() == native_order_count_before,
            "read calls must not mutate native private order state");

    std::vector<MoexPlaza2OwnOrderItem> orders_again(order_count);
    require(moex_copy_plaza2_own_order_items(guard.handle, orders_again.data(), order_count, &written) ==
                MOEX_RESULT_OK,
            "repeat own order copy must succeed");
    bool found_repeat_merged_order = false;
    for (const auto& item : orders_again) {
        if (item.private_order_id == 20003) {
            found_repeat_merged_order = true;
            require(std::string(item.price_text) == "102500", "repeat reads must remain deterministic");
        }
    }
    require(found_repeat_merged_order, "repeat read must still include merged order");
}

void test_reconciler_export_and_divergence_status() {
    auto guard = make_handle();

    auto confirmed = make_confirmed_reconciler();
    require(install_reconciler_snapshot(guard.handle, std::move(confirmed)) == MOEX_RESULT_OK,
            "reconciler install must succeed");

    MoexPlaza2TwimeReconcilerHealth health{};
    require(moex_get_plaza2_twime_reconciler_health(guard.handle, &health) == MOEX_RESULT_OK,
            "reconciler health export must succeed");
    require(health.twime_present == 1U && health.plaza_present == 1U,
            "reconciler health must surface both source summaries");
    require(health.total_confirmed_orders == 1U, "confirmed order count must be exported");
    require(health.total_matched_trades == 1U, "matched trade count must be exported");

    const auto native_order_count_before = guard.handle->plaza2_twime_reconciler->orders().size();
    const auto native_trade_count_before = guard.handle->plaza2_twime_reconciler->trades().size();

    uint32_t order_count = 0;
    require(moex_get_plaza2_reconciled_order_count(guard.handle, &order_count) == MOEX_RESULT_OK &&
                order_count == static_cast<uint32_t>(native_order_count_before),
            "confirmed reconciled order count must match native snapshot size");
    std::vector<MoexPlaza2ReconciledOrderItem> orders(order_count);
    uint32_t written = 0;
    require(moex_copy_plaza2_reconciled_order_items(guard.handle, orders.data(), order_count, &written) ==
                MOEX_RESULT_OK,
            "reconciled order copy must succeed");
    bool found_confirmed_order = false;
    for (const auto& item : orders) {
        if (item.twime_order_id == 20003 || item.plaza_private_order_id == 20003) {
            found_confirmed_order = true;
            require(item.status == MOEX_PLAZA2_ORDER_STATUS_CONFIRMED,
                    "confirmed reconciled order status must be exported");
            require(item.match_mode == MOEX_PLAZA2_MATCH_MODE_DIRECT_IDENTIFIER,
                    "confirmed order match mode must be exported");
        }
    }
    require(found_confirmed_order, "confirmed reconciled order must be present");

    uint32_t trade_count = 0;
    require(moex_get_plaza2_reconciled_trade_count(guard.handle, &trade_count) == MOEX_RESULT_OK &&
                trade_count == static_cast<uint32_t>(native_trade_count_before),
            "matched reconciled trade count must match native snapshot size");
    std::vector<MoexPlaza2ReconciledTradeItem> trades(trade_count);
    require(moex_copy_plaza2_reconciled_trade_items(guard.handle, trades.data(), trade_count, &written) ==
                MOEX_RESULT_OK,
            "reconciled trade copy must succeed");
    bool found_matched_trade = false;
    for (const auto& item : trades) {
        if (item.twime_trade_id == 9001 || item.plaza_trade_id == 9001) {
            found_matched_trade = true;
            require(item.status == MOEX_PLAZA2_TRADE_STATUS_MATCHED,
                    "matched reconciled trade status must be exported");
        }
    }
    require(found_matched_trade, "matched reconciled trade must be present");

    require(guard.handle->plaza2_twime_reconciler->orders().size() == native_order_count_before,
            "read calls must not mutate native reconciled orders");
    require(guard.handle->plaza2_twime_reconciler->trades().size() == native_trade_count_before,
            "read calls must not mutate native reconciled trades");

    auto diverged = make_diverged_reconciler();
    require(install_reconciler_snapshot(guard.handle, std::move(diverged)) == MOEX_RESULT_OK,
            "diverged reconciler install must succeed");

    require(moex_get_plaza2_reconciled_order_count(guard.handle, &order_count) == MOEX_RESULT_OK &&
                order_count == static_cast<uint32_t>(guard.handle->plaza2_twime_reconciler->orders().size()),
            "diverged order count must remain queryable");
    orders.assign(order_count, {});
    require(moex_copy_plaza2_reconciled_order_items(guard.handle, orders.data(), order_count, &written) ==
                MOEX_RESULT_OK,
            "diverged order copy must succeed");
    bool found_diverged_order = false;
    for (const auto& item : orders) {
        if (item.twime_order_id == 20003 || item.plaza_private_order_id == 20003) {
            found_diverged_order = true;
            require(item.status == MOEX_PLAZA2_ORDER_STATUS_DIVERGED, "diverged order status must be preserved");
            require(std::string(item.fault_reason) == "price_mismatch",
                    "diverged order fault reason must be preserved");
        }
    }
    require(found_diverged_order, "diverged order must be present");

    require(moex_get_plaza2_reconciled_trade_count(guard.handle, &trade_count) == MOEX_RESULT_OK && trade_count == 1U,
            "diverged trade count must remain queryable");
    trades.assign(trade_count, {});
    require(moex_copy_plaza2_reconciled_trade_items(guard.handle, trades.data(), trade_count, &written) ==
                MOEX_RESULT_OK,
            "diverged trade copy must succeed");
    bool found_diverged_trade = false;
    for (const auto& item : trades) {
        if (item.twime_trade_id == 91001 || item.plaza_trade_id == 91001) {
            found_diverged_trade = true;
            require(item.status == MOEX_PLAZA2_TRADE_STATUS_DIVERGED, "diverged trade status must be preserved");
            require(std::string(item.fault_reason) == "quantity_mismatch",
                    "diverged trade fault reason must be preserved");
        }
    }
    require(found_diverged_trade, "diverged trade must be present");
}

} // namespace

int main() {
    try {
        test_unavailable_and_invalid_argument_guards();
        test_private_snapshot_export_and_no_mutation_on_read();
        test_reconciler_export_and_divergence_status();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
