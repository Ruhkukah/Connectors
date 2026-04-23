#pragma once

#include "moex/plaza2/cgate/plaza2_fake_engine.hpp"
#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2_twime_reconciler/plaza2_twime_reconciler.hpp"

#include "plaza2_fake_scenarios.hpp"
#include "twime_trade_test_support.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2_twime_reconciler::test_support {

inline void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline void set_field(twime_sbe::TwimeEncodeRequest& request, std::string_view name, twime_sbe::TwimeFieldValue value) {
    for (auto& field : request.fields) {
        if (field.name == name) {
            field.value = value;
            return;
        }
    }
    throw std::runtime_error("missing TWIME field: " + std::string(name));
}

inline twime_trade::TwimeJournalEntry make_inbound_entry(const twime_sbe::TwimeEncodeRequest& request,
                                                         std::uint64_t sequence_number) {
    return {
        .sequence_number = sequence_number,
        .consumes_sequence = true,
        .recoverable = true,
        .template_id = request.template_id,
        .message_name = request.message_name,
        .bytes = twime_trade::test::encode_bytes(request),
    };
}

inline TwimeSourceHealthInput make_twime_health_input() {
    twime_trade::TwimeSessionHealthSnapshot session_health;
    session_health.state = twime_trade::TwimeSessionState::Active;
    session_health.transport_open = true;
    session_health.session_active = true;
    session_health.next_expected_inbound_seq = 21;
    session_health.next_outbound_seq = 34;
    session_health.last_transition_time_ms = 123456;

    twime_trade::TwimeSessionMetrics metrics;
    metrics.reconnect_attempts = 1;
    metrics.state_changes = 3;
    metrics.ack_received = 1;
    metrics.last_transition_time_ms = 123456;
    return make_twime_source_health_input(session_health, metrics);
}

inline std::int64_t price_to_mantissa(std::string_view value) {
    if (value.empty()) {
        return 0;
    }

    std::size_t position = 0;
    bool negative = false;
    if (value[position] == '-') {
        negative = true;
        ++position;
    }

    std::int64_t whole = 0;
    while (position < value.size() && value[position] != '.') {
        require(std::isdigit(static_cast<unsigned char>(value[position])) != 0,
                "price_to_mantissa received invalid whole digits");
        whole = whole * 10 + static_cast<std::int64_t>(value[position] - '0');
        ++position;
    }

    std::int64_t fraction = 0;
    std::int32_t fraction_digits = 0;
    if (position < value.size() && value[position] == '.') {
        ++position;
        while (position < value.size()) {
            require(std::isdigit(static_cast<unsigned char>(value[position])) != 0,
                    "price_to_mantissa received invalid fractional digits");
            require(fraction_digits < 5, "price_to_mantissa supports up to five fractional digits");
            fraction = fraction * 10 + static_cast<std::int64_t>(value[position] - '0');
            ++fraction_digits;
            ++position;
        }
    }
    while (fraction_digits < 5) {
        fraction *= 10;
        ++fraction_digits;
    }

    const auto mantissa = whole * 100000 + fraction;
    return negative ? -mantissa : mantissa;
}

inline void append_required_private_streams(PlazaSourceHealthInput& source_health, bool online = true,
                                            bool snapshot_complete = true) {
    using moex::plaza2::generated::StreamCode;
    using moex::plaza2::private_state::StreamHealthSnapshot;

    source_health.stream_health = {
        StreamHealthSnapshot{
            .stream_code = StreamCode::kFortsTradeRepl,
            .stream_name = "FORTS_TRADE_REPL",
            .online = online,
            .snapshot_complete = snapshot_complete,
        },
        StreamHealthSnapshot{
            .stream_code = StreamCode::kFortsUserorderbookRepl,
            .stream_name = "FORTS_USERORDERBOOK_REPL",
            .online = online,
            .snapshot_complete = snapshot_complete,
        },
        StreamHealthSnapshot{
            .stream_code = StreamCode::kFortsPosRepl,
            .stream_name = "FORTS_POS_REPL",
            .online = online,
            .snapshot_complete = snapshot_complete,
        },
        StreamHealthSnapshot{
            .stream_code = StreamCode::kFortsPartRepl,
            .stream_name = "FORTS_PART_REPL",
            .online = online,
            .snapshot_complete = snapshot_complete,
        },
        StreamHealthSnapshot{
            .stream_code = StreamCode::kFortsRefdataRepl,
            .stream_name = "FORTS_REFDATA_REPL",
            .online = online,
            .snapshot_complete = snapshot_complete,
        },
    };
}

inline PlazaCommittedSnapshotInput make_ready_plaza_snapshot(std::uint64_t logical_sequence = 1) {
    PlazaCommittedSnapshotInput snapshot;
    snapshot.logical_sequence = logical_sequence;
    snapshot.source_health.connector_health.open = true;
    snapshot.source_health.connector_health.online = true;
    snapshot.source_health.required_private_streams_ready = true;
    append_required_private_streams(snapshot.source_health, true, true);
    snapshot.source_health.resume_markers.has_lifenum = true;
    snapshot.source_health.resume_markers.last_lifenum = 7;
    snapshot.source_health.resume_markers.last_replstate = "lifenum=7;rev.orders=10";
    return snapshot;
}

inline PlazaCommittedSnapshotInput make_invalidated_plaza_snapshot(std::string reason,
                                                                   std::uint64_t last_lifenum = 11) {
    PlazaCommittedSnapshotInput snapshot;
    snapshot.logical_sequence = 99;
    snapshot.source_health.connector_health.open = true;
    snapshot.source_health.connector_health.online = false;
    snapshot.source_health.invalidated = true;
    snapshot.source_health.required_private_streams_ready = false;
    snapshot.source_health.invalidation_reason = std::move(reason);
    snapshot.source_health.resume_markers.has_lifenum = true;
    snapshot.source_health.resume_markers.last_lifenum = last_lifenum;
    append_required_private_streams(snapshot.source_health, false, false);
    return snapshot;
}

inline plaza2::private_state::Plaza2PrivateStateProjector project_scenario(std::string_view scenario_id) {
    const auto* scenario = plaza2::fake::FindScenarioById(scenario_id);
    require(scenario != nullptr, "required PLAZA scenario is missing");

    plaza2::private_state::Plaza2PrivateStateProjector projector;
    plaza2::fake::Plaza2FakeEngine engine;
    const auto result = engine.run(plaza2::fake::ViewForScenario(*scenario), &projector);
    if (result.error) {
        throw std::runtime_error("scenario replay failed: " + result.error.message);
    }
    return projector;
}

inline const ReconciledOrderSnapshot* find_order_by_cl_ord_id(std::span<const ReconciledOrderSnapshot> orders,
                                                              std::uint64_t cl_ord_id) {
    for (const auto& order : orders) {
        if (order.twime.cl_ord_id == cl_ord_id) {
            return &order;
        }
    }
    return nullptr;
}

inline const ReconciledOrderSnapshot* find_order_by_order_id(std::span<const ReconciledOrderSnapshot> orders,
                                                             std::int64_t order_id) {
    for (const auto& order : orders) {
        if (order.twime.order_id == order_id || order.plaza.private_order_id == order_id) {
            return &order;
        }
    }
    return nullptr;
}

inline const ReconciledTradeSnapshot* find_trade_by_trade_id(std::span<const ReconciledTradeSnapshot> trades,
                                                             std::int64_t trade_id) {
    for (const auto& trade : trades) {
        if (trade.twime.trade_id == trade_id || trade.plaza.trade_id == trade_id) {
            return &trade;
        }
    }
    return nullptr;
}

} // namespace moex::plaza2_twime_reconciler::test_support
