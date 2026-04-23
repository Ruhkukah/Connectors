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
using moex::plaza2::private_state::OwnOrderSnapshot;
using moex::plaza2::private_state::Plaza2PrivateStateProjector;
using moex::plaza2::private_state::StreamHealthSnapshot;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const OwnOrderSnapshot* find_order(std::span<const OwnOrderSnapshot> orders, std::int64_t private_order_id) {
    for (const auto& order : orders) {
        if (order.private_order_id == private_order_id) {
            return &order;
        }
    }
    return nullptr;
}

const StreamHealthSnapshot* find_stream(std::span<const StreamHealthSnapshot> streams, StreamCode stream_code) {
    for (const auto& stream : streams) {
        if (stream.stream_code == stream_code) {
            return &stream;
        }
    }
    return nullptr;
}

void run_or_throw(const char* scenario_id, Plaza2PrivateStateProjector* projector) {
    const auto* scenario = FindScenarioById(scenario_id);
    require(scenario != nullptr, "required Phase 3E scenario is missing");

    Plaza2FakeEngine engine;
    const auto result = engine.run(ViewForScenario(*scenario), projector);
    if (result.error) {
        throw std::runtime_error(std::string(scenario_id) + " replay failed: " + result.error.message);
    }
}

} // namespace

int main() {
    try {
        Plaza2PrivateStateProjector clear_deleted_projector;
        run_or_throw("private_state_clear_deleted", &clear_deleted_projector);

        const auto clear_deleted_orders = clear_deleted_projector.own_orders();
        require(clear_deleted_orders.size() == 2, "clear-deleted should leave only trade-owned orders committed");

        const auto* removed_order = find_order(clear_deleted_orders, 21001);
        require(removed_order == nullptr, "user-orderbook-only order should be removed by clear-deleted");

        const auto* trade_only = find_order(clear_deleted_orders, 21002);
        require(trade_only != nullptr, "trade-only order should survive clear-deleted");
        require(trade_only->from_trade_repl, "trade-only order should retain trade source");
        require(!trade_only->from_user_book && !trade_only->from_current_day,
                "trade-only order should not pick up user-orderbook sources");

        const auto* shared = find_order(clear_deleted_orders, 21003);
        require(shared != nullptr, "shared order should survive clear-deleted");
        require(shared->from_trade_repl, "shared order should retain trade source");
        require(!shared->from_user_book && !shared->from_current_day,
                "clear-deleted should clear only user-orderbook ownership");
        require(shared->price == "103500", "shared order should keep the committed trade-side delta");

        const auto* clear_deleted_stream =
            find_stream(clear_deleted_projector.stream_health(), StreamCode::kFortsUserorderbookRepl);
        require(clear_deleted_stream != nullptr, "user-orderbook stream health should exist after clear-deleted");
        require(clear_deleted_stream->clear_deleted_count == 1, "clear-deleted count should advance");
        require(clear_deleted_stream->committed_row_count == 0, "user-orderbook row watermark should be reset");

        Plaza2PrivateStateProjector lifenum_projector;
        run_or_throw("private_state_lifenum_invalidation", &lifenum_projector);

        const auto& connector = lifenum_projector.connector_health();
        require(!connector.online, "lifenum invalidation should clear connector online state");
        require(connector.commit_count == 1, "lifenum invalidation should preserve historical commit count");

        const auto& resume = lifenum_projector.resume_markers();
        require(resume.has_lifenum && resume.last_lifenum == 11,
                "latest lifenum should be retained after invalidation");

        require(lifenum_projector.sessions().empty(), "session state should be invalidated on lifenum change");
        require(lifenum_projector.instruments().empty(), "instrument state should be invalidated on lifenum change");
        require(lifenum_projector.matching_map().empty(), "matching map should be invalidated on lifenum change");
        require(lifenum_projector.limits().empty(), "limits should be invalidated on lifenum change");
        require(lifenum_projector.positions().empty(), "positions should be invalidated on lifenum change");
        require(lifenum_projector.own_orders().empty(), "own orders should be invalidated on lifenum change");
        require(lifenum_projector.own_trades().empty(), "own trades should be invalidated on lifenum change");

        const auto lifenum_streams = lifenum_projector.stream_health();
        require(lifenum_streams.size() == 5, "all declared streams should remain addressable after invalidation");
        for (const auto& stream : lifenum_streams) {
            require(!stream.online, "stream online flags should reset after lifenum invalidation");
            require(!stream.snapshot_complete, "stream snapshot flags should reset after lifenum invalidation");
            require(stream.committed_row_count == 0, "stream row watermark should reset after lifenum invalidation");
            require(stream.last_commit_sequence == 0,
                    "stream commit watermark should reset after lifenum invalidation");
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
