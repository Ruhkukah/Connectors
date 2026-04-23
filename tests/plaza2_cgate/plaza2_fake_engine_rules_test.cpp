#include "moex/plaza2/cgate/plaza2_fake_engine.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using moex::plaza2::fake::CommitListener;
using moex::plaza2::fake::EngineErrorCode;
using moex::plaza2::fake::EventKind;
using moex::plaza2::fake::EventSpec;
using moex::plaza2::fake::FieldValueSpec;
using moex::plaza2::fake::InvariantSpec;
using moex::plaza2::fake::Plaza2FakeEngine;
using moex::plaza2::fake::RowSpec;
using moex::plaza2::fake::ScenarioDataView;
using moex::plaza2::fake::ScenarioSpec;
using moex::plaza2::fake::ValueKind;
using moex::plaza2::generated::FieldCode;
using moex::plaza2::generated::StreamCode;
using moex::plaza2::generated::TableCode;

struct ThrowingCommitListener final : CommitListener {
    void on_transaction_commit(const ScenarioSpec&, const EventSpec&, const moex::plaza2::fake::EngineState&) override {
        throw std::runtime_error("synthetic callback failure");
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ScenarioSpec make_scenario(std::uint32_t event_count) {
    return {
        .scenario_id = "manual_rule_fixture",
        .description = "Manual fake-engine rule fixture",
        .metadata_version = 1,
        .deterministic_seed = 0,
        .first_stream_index = 0,
        .stream_count = 1,
        .first_event_index = 0,
        .event_count = event_count,
        .first_invariant_index = 0,
        .invariant_count = 0,
    };
}

} // namespace

int main() {
    try {
        const StreamCode streams[] = {StreamCode::kFortsTradeRepl};
        const FieldValueSpec fields[] = {
            {
                .field_code = FieldCode::kFortsTradeReplOrdersLogReplId,
                .kind = ValueKind::kSignedInteger,
                .signed_value = 1,
                .unsigned_value = 0,
                .text_value = "1",
            },
        };
        const RowSpec rows[] = {
            {
                .stream_code = StreamCode::kFortsTradeRepl,
                .table_code = TableCode::kFortsTradeReplOrdersLog,
                .first_field_index = 0,
                .field_count = 1,
            },
        };
        const auto no_invariants = std::span<const InvariantSpec>{};

        const EventSpec outside_tx_events[] = {
            {.kind = EventKind::kOpen},
            {
                .kind = EventKind::kStreamData,
                .stream_code = StreamCode::kFortsTradeRepl,
                .table_code = TableCode::kFortsTradeReplOrdersLog,
                .first_row_index = 0,
                .row_count = 1,
            },
        };
        const EventSpec nested_tx_events[] = {
            {.kind = EventKind::kOpen},
            {.kind = EventKind::kTransactionBegin},
            {.kind = EventKind::kTransactionBegin},
        };
        const EventSpec commit_without_begin_events[] = {
            {.kind = EventKind::kOpen},
            {.kind = EventKind::kTransactionCommit},
        };
        const EventSpec callback_events[] = {
            {.kind = EventKind::kOpen},
            {.kind = EventKind::kTransactionBegin},
            {
                .kind = EventKind::kStreamData,
                .stream_code = StreamCode::kFortsTradeRepl,
                .table_code = TableCode::kFortsTradeReplOrdersLog,
                .first_row_index = 0,
                .row_count = 1,
            },
            {.kind = EventKind::kTransactionCommit},
        };

        Plaza2FakeEngine engine;

        const auto outside_tx =
            engine.run(ScenarioDataView{make_scenario(2), streams, outside_tx_events, rows, fields, no_invariants});
        require(outside_tx.error.code == EngineErrorCode::kStreamDataOutsideTransaction,
                "STREAM_DATA outside TN_BEGIN should fail");

        const auto nested_tx =
            engine.run(ScenarioDataView{make_scenario(3), streams, nested_tx_events, rows, fields, no_invariants});
        require(nested_tx.error.code == EngineErrorCode::kTransactionAlreadyOpen, "nested TN_BEGIN should fail");

        const auto commit_without_begin = engine.run(
            ScenarioDataView{make_scenario(2), streams, commit_without_begin_events, rows, fields, no_invariants});
        require(commit_without_begin.error.code == EngineErrorCode::kTransactionNotOpen,
                "TN_COMMIT without TN_BEGIN should fail");

        ThrowingCommitListener listener;
        const auto callback_result = engine.run(
            ScenarioDataView{make_scenario(4), streams, callback_events, rows, fields, no_invariants}, &listener);
        require(!callback_result.error, "callback errors must be contained");
        require(callback_result.state.commit_count == 1, "commit should still be recorded");
        require(callback_result.state.callback_error_count == 1, "callback error count should increment");

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
