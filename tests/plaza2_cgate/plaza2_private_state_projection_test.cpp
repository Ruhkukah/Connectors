#include "moex/plaza2/cgate/plaza2_fake_engine.hpp"
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

const PositionSnapshot* find_position(std::span<const PositionSnapshot> positions, std::string_view account_code,
                                      std::int32_t isin_id) {
    for (const auto& position : positions) {
        if (position.account_code == account_code && position.isin_id == isin_id) {
            return &position;
        }
    }
    return nullptr;
}

enum class OrderSource {
    Any,
    Trade,
    UserBook,
    CurrentDay,
};

const OwnOrderSnapshot* find_order(std::span<const OwnOrderSnapshot> orders, std::int64_t private_order_id,
                                   OrderSource source = OrderSource::Any) {
    for (const auto& order : orders) {
        const bool source_matches = source == OrderSource::Any ||
                                    (source == OrderSource::Trade && order.from_trade_repl) ||
                                    (source == OrderSource::UserBook && order.from_user_book) ||
                                    (source == OrderSource::CurrentDay && order.from_current_day);
        if (order.private_order_id == private_order_id && source_matches) {
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
        require(session->begin == 1700000000 && session->end == 1700003600 && session->clr_sess_begin == 1700002400 &&
                    session->settl_sess_begin == 1700003000 && session->settl_price_calc_time == 1700003200 &&
                    session->settl_sess_t1_begin == 1700003300 && session->margin_call_fix_schedule == 1700003400,
                "session without removed compatibility fields lost retained schedule timestamps");

        const auto instruments = projector.instruments();
        require(instruments.size() == 1, "declared futures projection should omit option and multileg instruments");

        const auto* future = find_instrument(instruments, 1001);
        require(future != nullptr, "future instrument should be projected");
        require(future->kind == InstrumentKind::kFuture, "future instrument kind should be correct");
        require(future->isin == "RTS-6.26", "future instrument code should be projected");
        require(future->base_contract_code == "RTS", "future base contract should be projected");
        require(future->settlement_price == "105000.5", "future settlement price should be projected");

        require(find_instrument(instruments, 2001) == nullptr && find_instrument(instruments, 3001) == nullptr &&
                    projector.matching_map().empty() && projector.limits().empty(),
                "undeclared options, multileg, matching and PART rows reached product state");

        const auto* position = find_position(projector.positions(), "CL001", 1001);
        require(position != nullptr, "client position should be projected");
        require(position->xpos == 4, "position quantity should be projected");
        require(position->xopen_qty == 5, "open quantity should be projected");
        require(position->waprice == "104950.25", "weighted average price should be projected");
        require(position->last_deal_id == 9001, "last deal id should be projected");

        const auto orders = projector.own_orders();
        require(orders.size() == 2, "regular TRADE and USERORDERBOOK surfaces should remain independently projected");

        const auto* live_order = find_order(orders, 20001);
        require(live_order != nullptr, "user-orderbook-only order should be projected");
        require(live_order->from_user_book, "user-orderbook order should keep its source");
        require(!live_order->from_trade_repl, "user-orderbook-only order should not claim trade source");
        require(!live_order->from_current_day, "live order should not claim current-day source");
        require(live_order->price == "100500", "user-orderbook price should be projected");

        require(find_order(orders, 20003, OrderSource::CurrentDay) == nullptr,
                "unconsumed current-day book row reached the product view");

        const auto* trade_order = find_order(orders, 20003, OrderSource::Trade);
        require(trade_order != nullptr, "TRADE order should remain independently projected");
        require(!trade_order->from_user_book && !trade_order->from_current_day,
                "TRADE order must not claim USERORDERBOOK sources");
        require(trade_order->price == "102500", "TRADE order fields must remain unchanged");
        require(trade_order->public_amount_rest == 5 && trade_order->private_amount_rest == 4,
                "TRADE order amount/rest must not be overwritten by USERORDERBOOK");
        require(trade_order->id_deal == 9001, "TRADE order must retain its deal identity");

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
        require(!userbook_stream->has_publication_state && !userbook_stream->periodic_snapshot_consistent,
                "current-day publication state must not certify the regular user-orderbook snapshot");
        require(userbook_stream->last_trades_rev == 0 && userbook_stream->last_trades_lifenum == 0,
                "current-day trade markers must not replace regular user-orderbook markers");
        require(userbook_stream->last_server_time == 0, "current-day server time must not replace regular metadata");

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
