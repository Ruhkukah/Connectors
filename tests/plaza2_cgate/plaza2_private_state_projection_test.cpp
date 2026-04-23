#include "moex/plaza2/cgate/plaza2_private_state.hpp"

#include "plaza2_fake_scenarios.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {

using moex::plaza2::fake::FindScenarioById;
using moex::plaza2::fake::Plaza2FakeEngine;
using moex::plaza2::fake::ViewForScenario;
using moex::plaza2::generated::StreamCode;
using moex::plaza2::private_state::InstrumentKind;
using moex::plaza2::private_state::InstrumentSnapshot;
using moex::plaza2::private_state::LimitSnapshot;
using moex::plaza2::private_state::MatchingMapSnapshot;
using moex::plaza2::private_state::OwnOrderSnapshot;
using moex::plaza2::private_state::OwnTradeSnapshot;
using moex::plaza2::private_state::Plaza2PrivateStateProjector;
using moex::plaza2::private_state::PositionSnapshot;
using moex::plaza2::private_state::StreamHealthSnapshot;
using moex::plaza2::private_state::TradingSessionSnapshot;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const StreamHealthSnapshot* find_stream(std::span<const StreamHealthSnapshot> streams, StreamCode stream_code) {
    for (const auto& stream : streams) {
        if (stream.stream_code == stream_code) {
            return &stream;
        }
    }
    return nullptr;
}

const TradingSessionSnapshot* find_session(std::span<const TradingSessionSnapshot> sessions, std::int32_t sess_id) {
    for (const auto& session : sessions) {
        if (session.sess_id == sess_id) {
            return &session;
        }
    }
    return nullptr;
}

const InstrumentSnapshot* find_instrument(std::span<const InstrumentSnapshot> instruments, std::int32_t isin_id) {
    for (const auto& instrument : instruments) {
        if (instrument.isin_id == isin_id) {
            return &instrument;
        }
    }
    return nullptr;
}

const MatchingMapSnapshot* find_matching(std::span<const MatchingMapSnapshot> entries, std::int32_t base_contract_id) {
    for (const auto& entry : entries) {
        if (entry.base_contract_id == base_contract_id) {
            return &entry;
        }
    }
    return nullptr;
}

const LimitSnapshot* find_limit(std::span<const LimitSnapshot> limits, std::string_view account_code) {
    for (const auto& limit : limits) {
        if (limit.account_code == account_code) {
            return &limit;
        }
    }
    return nullptr;
}

const PositionSnapshot* find_position(std::span<const PositionSnapshot> positions, std::string_view account_code,
                                      std::int32_t isin_id) {
    for (const auto& position : positions) {
        if (position.account_code == account_code && position.isin_id == isin_id) {
            return &position;
        }
    }
    return nullptr;
}

const OwnOrderSnapshot* find_order(std::span<const OwnOrderSnapshot> orders, std::int64_t private_order_id) {
    for (const auto& order : orders) {
        if (order.private_order_id == private_order_id) {
            return &order;
        }
    }
    return nullptr;
}

const OwnTradeSnapshot* find_trade(std::span<const OwnTradeSnapshot> trades, std::int64_t id_deal) {
    for (const auto& trade : trades) {
        if (trade.id_deal == id_deal) {
            return &trade;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    try {
        const auto* scenario = FindScenarioById("private_state_projection");
        require(scenario != nullptr, "private_state_projection scenario is missing");

        Plaza2PrivateStateProjector projector;
        Plaza2FakeEngine engine;
        const auto result = engine.run(ViewForScenario(*scenario), &projector);
        if (result.error) {
            throw std::runtime_error("private_state_projection replay failed: " + result.error.message);
        }

        const auto& connector = projector.connector_health();
        require(connector.open, "connector should be marked open");
        require(connector.online, "connector should be marked online");
        require(!connector.snapshot_active, "snapshot should be complete");
        require(!connector.transaction_open, "no transaction should remain open");
        require(connector.commit_count == 1, "commit count should match replay");

        const auto& resume = projector.resume_markers();
        require(resume.has_lifenum && resume.last_lifenum == 7, "lifenum should be retained in committed markers");
        require(resume.last_replstate == "lifenum=7;rev.orders=10;rev.position=20",
                "replstate marker should be retained");

        const auto* session = find_session(projector.sessions(), 321);
        require(session != nullptr, "session 321 should be projected");
        require(session->state == 2, "session state should be projected");
        require(session->eve_on, "session eve flag should be projected");
        require(!session->mon_on, "session mon flag should be projected");

        const auto instruments = projector.instruments();
        require(instruments.size() == 3, "projection should expose three committed instruments");

        const auto* future = find_instrument(instruments, 1001);
        require(future != nullptr, "future instrument should be projected");
        require(future->kind == InstrumentKind::kFuture, "future instrument kind should be correct");
        require(future->isin == "RTS-6.26", "future instrument code should be projected");
        require(future->base_contract_code == "RTS", "future base contract should be projected");
        require(future->settlement_price == "105000.5", "future settlement price should be projected");

        const auto* option = find_instrument(instruments, 2001);
        require(option != nullptr, "option instrument should be projected");
        require(option->kind == InstrumentKind::kOption, "option instrument kind should be correct");
        require(option->fut_isin_id == 1001, "option-to-future mapping should be projected");
        require(option->strike == "95000", "option strike should be projected");

        const auto* multileg = find_instrument(instruments, 3001);
        require(multileg != nullptr, "multileg instrument should be projected");
        require(multileg->kind == InstrumentKind::kMultileg, "multileg kind should be projected");
        require(multileg->legs.size() == 2, "multileg instrument should expose sorted leg metadata");
        require(multileg->legs[0].leg_isin_id == 1001 && multileg->legs[0].leg_order_no == 1,
                "first multileg leg should be projected");
        require(multileg->legs[1].leg_isin_id == 2001 && multileg->legs[1].leg_order_no == 2,
                "second multileg leg should be projected");

        const auto* matching = find_matching(projector.matching_map(), 500);
        require(matching != nullptr, "matching map entry should be projected");
        require(matching->matching_id == 3, "matching id should be projected");

        const auto* limit = find_limit(projector.limits(), "CL001");
        require(limit != nullptr, "client limits should be projected");
        require(limit->limits_set, "limits_set should be projected");
        require(limit->is_auto_update_limit, "auto-update flag should be projected");
        require(limit->money_free == "125000.5", "free money should be projected");
        require(limit->broker_fee == "3.15", "broker fee should be projected");

        const auto* position = find_position(projector.positions(), "CL001", 1001);
        require(position != nullptr, "client position should be projected");
        require(position->xpos == 4, "position quantity should be projected");
        require(position->xopen_qty == 5, "open quantity should be projected");
        require(position->waprice == "104950.25", "weighted average price should be projected");
        require(position->last_deal_id == 9001, "last deal id should be projected");

        const auto orders = projector.own_orders();
        require(orders.size() == 2, "two committed own orders should be projected");

        const auto* live_order = find_order(orders, 20001);
        require(live_order != nullptr, "user-orderbook-only order should be projected");
        require(live_order->from_user_book, "user-orderbook order should keep its source");
        require(!live_order->from_trade_repl, "user-orderbook-only order should not claim trade source");
        require(!live_order->from_current_day, "live order should not claim current-day source");
        require(live_order->price == "100500", "user-orderbook price should be projected");

        const auto* merged_order = find_order(orders, 20003);
        require(merged_order != nullptr, "merged current-day/trade order should be projected");
        require(merged_order->from_trade_repl, "trade source should be retained");
        require(!merged_order->from_user_book, "current-day order should not claim live-userbook source");
        require(merged_order->from_current_day, "current-day source should be retained");
        require(merged_order->price == "102500", "trade delta should override current-day price");
        require(merged_order->public_amount_rest == 5, "trade delta should override public amount rest");
        require(merged_order->private_amount_rest == 4, "trade delta should override private amount rest");
        require(merged_order->id_deal == 9001, "trade-linked order should retain deal id");

        const auto trades = projector.own_trades();
        require(trades.size() == 1, "one committed own trade should be projected");
        const auto* trade = find_trade(trades, 9001);
        require(trade != nullptr, "own trade should be projected");
        require(trade->amount == 2, "own trade amount should be projected");
        require(trade->public_order_id_sell == 10003, "own trade sell order id should be projected");
        require(trade->private_order_id_sell == 20003, "own trade private sell order id should be projected");
        require(trade->code_sell == "CL001", "own trade client code should be projected");

        const auto streams = projector.stream_health();
        require(streams.size() == 5, "all declared private streams should expose health state");

        const auto* trade_stream = find_stream(streams, StreamCode::kFortsTradeRepl);
        require(trade_stream != nullptr, "trade stream health should exist");
        require(trade_stream->online && trade_stream->snapshot_complete, "trade stream should be online after replay");
        require(trade_stream->committed_row_count == 4, "trade stream row watermark should match replay");
        require(trade_stream->last_commit_sequence == 1, "trade stream commit watermark should advance on commit");
        require(trade_stream->last_event_id == 42 && trade_stream->last_event_type == 7,
                "trade sys_event metadata should be projected");
        require(trade_stream->last_message == "trade-ready", "trade sys_event text should be projected");
        require(trade_stream->last_server_time == 1700000008, "latest trade stream server time should be projected");

        const auto* userbook_stream = find_stream(streams, StreamCode::kFortsUserorderbookRepl);
        require(userbook_stream != nullptr, "user-orderbook stream health should exist");
        require(userbook_stream->has_publication_state && userbook_stream->publication_state == 1,
                "user-orderbook publication state should be projected");
        require(userbook_stream->last_trades_rev == 1001 && userbook_stream->last_trades_lifenum == 7,
                "user-orderbook trade markers should be projected");
        require(userbook_stream->last_server_time == 1700000002, "user-orderbook server time should be projected");

        const auto* pos_stream = find_stream(streams, StreamCode::kFortsPosRepl);
        require(pos_stream != nullptr, "position stream health should exist");
        require(pos_stream->last_trades_rev == 44 && pos_stream->last_trades_lifenum == 7,
                "position trade markers should be projected");

        const auto* part_stream = find_stream(streams, StreamCode::kFortsPartRepl);
        require(part_stream != nullptr, "part stream health should exist");
        require(part_stream->last_event_id == 8 && part_stream->last_message == "limits-ready",
                "part sys_event metadata should be projected");

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
