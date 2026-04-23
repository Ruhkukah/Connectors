#include "moex/plaza2/cgate/plaza2_fake_engine.hpp"

#include <exception>
#include <vector>

namespace moex::plaza2::fake {

namespace {

EngineError make_error(EngineErrorCode code, std::string message) {
    return {
        .code = code,
        .message = std::move(message),
    };
}

std::vector<StreamState> initialize_stream_states(std::span<const generated::StreamCode> streams) {
    std::vector<StreamState> out;
    out.reserve(streams.size());
    for (const auto stream_code : streams) {
        const auto* descriptor = generated::FindStreamByCode(stream_code);
        out.push_back(
            {
                .stream_code = stream_code,
                .stream_name = descriptor != nullptr ? descriptor->stream_name : std::string_view{},
            }
        );
    }
    return out;
}

std::size_t require_stream_index(const EngineState& state, generated::StreamCode stream_code, EngineError* error) {
    for (std::size_t index = 0; index < state.streams.size(); ++index) {
        if (state.streams[index].stream_code == stream_code) {
            return index;
        }
    }
    if (error != nullptr) {
        *error = make_error(EngineErrorCode::kStreamNotDeclared, "scenario event references a stream that was not declared");
    }
    return state.streams.size();
}

bool rows_for_event(const ScenarioDataView& view, const EventSpec& event, std::span<const RowSpec>* out_rows,
                    EngineError* error) {
    if (event.row_count == 0) {
        *out_rows = {};
        return true;
    }
    const auto first = static_cast<std::size_t>(event.first_row_index);
    const auto count = static_cast<std::size_t>(event.row_count);
    if (first > view.rows.size() || count > view.rows.size() - first) {
        if (error != nullptr) {
            *error = make_error(EngineErrorCode::kMalformedScenario, "event row span is outside the scenario row table");
        }
        return false;
    }
    *out_rows = view.rows.subspan(first, count);
    return true;
}

bool fields_for_row(const ScenarioDataView& view, const RowSpec& row, std::span<const FieldValueSpec>* out_fields,
                    EngineError* error) {
    if (row.field_count == 0) {
        if (error != nullptr) {
            *error = make_error(EngineErrorCode::kMalformedScenario, "stream row does not carry any field values");
        }
        return false;
    }
    const auto first = static_cast<std::size_t>(row.first_field_index);
    const auto count = static_cast<std::size_t>(row.field_count);
    if (first > view.fields.size() || count > view.fields.size() - first) {
        if (error != nullptr) {
            *error = make_error(EngineErrorCode::kMalformedScenario, "row field span is outside the scenario field table");
        }
        return false;
    }
    *out_fields = view.fields.subspan(first, count);
    return true;
}

void invalidate_stream_state_for_resync(EngineState& state) {
    state.online = false;
    for (auto& stream : state.streams) {
        stream.online = false;
        stream.snapshot_complete = false;
        stream.committed_row_count = 0;
    }
}

bool check_invariants(const ScenarioDataView& view, const EngineState& state, EngineError* error) {
    for (const auto& invariant : view.invariants) {
        switch (invariant.kind) {
        case InvariantKind::kCommitCount:
            if (state.commit_count != invariant.numeric_value) {
                *error = make_error(
                    EngineErrorCode::kInvariantFailed,
                    "commit_count invariant failed for scenario " + std::string(view.scenario.scenario_id)
                );
                return false;
            }
            break;
        case InvariantKind::kLastLifenum:
            if (!state.has_lifenum || state.last_lifenum != invariant.numeric_value) {
                *error = make_error(
                    EngineErrorCode::kInvariantFailed,
                    "last_lifenum invariant failed for scenario " + std::string(view.scenario.scenario_id)
                );
                return false;
            }
            break;
        case InvariantKind::kStreamOnline: {
            const auto* stream = Plaza2FakeEngine::find_stream_state(state, invariant.stream_code);
            if (stream == nullptr || stream->online != invariant.bool_value) {
                *error = make_error(
                    EngineErrorCode::kInvariantFailed,
                    "stream_online invariant failed for scenario " + std::string(view.scenario.scenario_id)
                );
                return false;
            }
            break;
        }
        case InvariantKind::kLastReplstate:
            if (state.last_replstate != invariant.text_value) {
                *error = make_error(
                    EngineErrorCode::kInvariantFailed,
                    "last_replstate invariant failed for scenario " + std::string(view.scenario.scenario_id)
                );
                return false;
            }
            break;
        case InvariantKind::kClearDeletedCount: {
            const auto* stream = Plaza2FakeEngine::find_stream_state(state, invariant.stream_code);
            if (stream == nullptr || stream->clear_deleted_count != invariant.numeric_value) {
                *error = make_error(
                    EngineErrorCode::kInvariantFailed,
                    "clear_deleted_count invariant failed for scenario " + std::string(view.scenario.scenario_id)
                );
                return false;
            }
            break;
        }
        }
    }
    return true;
}

} // namespace

RunResult Plaza2FakeEngine::run(const ScenarioDataView& view, CommitListener* listener) const {
    RunResult result;
    result.state.streams = initialize_stream_states(view.streams);
    std::vector<std::uint64_t> pending_row_deltas(result.state.streams.size(), 0);

    for (const auto& event : view.events) {
        switch (event.kind) {
        case EventKind::kOpen:
            if (result.state.open) {
                result.error = make_error(EngineErrorCode::kAlreadyOpen, "OPEN may appear only once");
                return result;
            }
            result.state.open = true;
            break;

        case EventKind::kClose:
            if (!result.state.open) {
                result.error = make_error(EngineErrorCode::kNotOpen, "CLOSE requires OPEN");
                return result;
            }
            if (result.state.closed) {
                result.error = make_error(EngineErrorCode::kEventAfterClose, "events may not appear after CLOSE");
                return result;
            }
            if (result.state.transaction_open) {
                result.error = make_error(EngineErrorCode::kTransactionAlreadyOpen, "CLOSE is invalid while a transaction is open");
                return result;
            }
            result.state.closed = true;
            result.state.online = false;
            for (auto& stream : result.state.streams) {
                stream.online = false;
            }
            break;

        case EventKind::kSnapshotBegin:
            if (!result.state.open) {
                result.error = make_error(EngineErrorCode::kNotOpen, "SNAPSHOT_BEGIN requires OPEN");
                return result;
            }
            if (result.state.closed) {
                result.error = make_error(EngineErrorCode::kEventAfterClose, "events may not appear after CLOSE");
                return result;
            }
            if (result.state.snapshot_active) {
                result.error = make_error(EngineErrorCode::kSnapshotAlreadyActive, "SNAPSHOT_BEGIN may not nest");
                return result;
            }
            if (result.state.transaction_open) {
                result.error = make_error(
                    EngineErrorCode::kTransactionAlreadyOpen,
                    "SNAPSHOT_BEGIN is invalid while a transaction is open"
                );
                return result;
            }
            result.state.snapshot_active = true;
            break;

        case EventKind::kSnapshotEnd:
            if (!result.state.snapshot_active) {
                result.error = make_error(EngineErrorCode::kSnapshotNotActive, "SNAPSHOT_END requires SNAPSHOT_BEGIN");
                return result;
            }
            if (result.state.transaction_open) {
                result.error = make_error(
                    EngineErrorCode::kTransactionAlreadyOpen,
                    "SNAPSHOT_END is invalid while a transaction is open"
                );
                return result;
            }
            result.state.snapshot_active = false;
            for (auto& stream : result.state.streams) {
                stream.snapshot_complete = true;
            }
            break;

        case EventKind::kOnline:
            if (!result.state.open) {
                result.error = make_error(EngineErrorCode::kNotOpen, "ONLINE requires OPEN");
                return result;
            }
            if (result.state.closed) {
                result.error = make_error(EngineErrorCode::kEventAfterClose, "events may not appear after CLOSE");
                return result;
            }
            if (result.state.snapshot_active) {
                result.error = make_error(EngineErrorCode::kOnlineBeforeSnapshotEnd, "ONLINE requires SNAPSHOT_END first");
                return result;
            }
            result.state.online = true;
            for (auto& stream : result.state.streams) {
                stream.online = true;
            }
            break;

        case EventKind::kTransactionBegin:
            if (!result.state.open) {
                result.error = make_error(EngineErrorCode::kNotOpen, "TN_BEGIN requires OPEN");
                return result;
            }
            if (result.state.closed) {
                result.error = make_error(EngineErrorCode::kEventAfterClose, "events may not appear after CLOSE");
                return result;
            }
            if (result.state.transaction_open) {
                result.error = make_error(EngineErrorCode::kTransactionAlreadyOpen, "nested TN_BEGIN is invalid");
                return result;
            }
            std::fill(pending_row_deltas.begin(), pending_row_deltas.end(), 0);
            result.state.transaction_open = true;
            break;

        case EventKind::kTransactionCommit:
            if (!result.state.transaction_open) {
                result.error = make_error(EngineErrorCode::kTransactionNotOpen, "TN_COMMIT requires TN_BEGIN");
                return result;
            }
            for (std::size_t index = 0; index < result.state.streams.size(); ++index) {
                result.state.streams[index].committed_row_count += pending_row_deltas[index];
            }
            std::fill(pending_row_deltas.begin(), pending_row_deltas.end(), 0);
            result.state.transaction_open = false;
            result.state.commit_count += 1;
            if (listener != nullptr) {
                try {
                    listener->on_transaction_commit(view.scenario, event, result.state);
                } catch (const std::exception&) {
                    result.state.callback_error_count += 1;
                } catch (...) {
                    result.state.callback_error_count += 1;
                }
            }
            break;

        case EventKind::kStreamData: {
            if (!result.state.transaction_open) {
                result.error = make_error(
                    EngineErrorCode::kStreamDataOutsideTransaction,
                    "STREAM_DATA requires TN_BEGIN before it"
                );
                return result;
            }
            std::span<const RowSpec> row_span;
            if (!rows_for_event(view, event, &row_span, &result.error)) {
                return result;
            }
            EngineError stream_error;
            const auto stream_index = require_stream_index(result.state, event.stream_code, &stream_error);
            if (stream_index == result.state.streams.size()) {
                result.error = stream_error;
                return result;
            }
            for (const auto& row : row_span) {
                if (row.stream_code != event.stream_code || row.table_code != event.table_code) {
                    result.error = make_error(
                        EngineErrorCode::kMalformedScenario,
                        "STREAM_DATA row metadata does not match its parent event"
                    );
                    return result;
                }
                std::span<const FieldValueSpec> field_span;
                if (!fields_for_row(view, row, &field_span, &result.error)) {
                    return result;
                }
                if (field_span.empty()) {
                    result.error = make_error(
                        EngineErrorCode::kMalformedScenario,
                        "STREAM_DATA row resolved to an empty field span"
                    );
                    return result;
                }
                pending_row_deltas[stream_index] += 1;
            }
            break;
        }

        case EventKind::kReplState:
            if (!result.state.open) {
                result.error = make_error(EngineErrorCode::kNotOpen, "P2REPL_REPLSTATE requires OPEN");
                return result;
            }
            if (result.state.transaction_open) {
                result.error = make_error(
                    EngineErrorCode::kTransactionAlreadyOpen,
                    "P2REPL_REPLSTATE is invalid while a transaction is open"
                );
                return result;
            }
            result.state.last_replstate.assign(event.text_value);
            break;

        case EventKind::kLifeNum:
            if (!result.state.open) {
                result.error = make_error(EngineErrorCode::kNotOpen, "P2REPL_LIFENUM requires OPEN");
                return result;
            }
            if (result.state.transaction_open) {
                result.error = make_error(
                    EngineErrorCode::kTransactionAlreadyOpen,
                    "P2REPL_LIFENUM is invalid while a transaction is open"
                );
                return result;
            }
            if (result.state.has_lifenum && result.state.last_lifenum != event.numeric_value) {
                invalidate_stream_state_for_resync(result.state);
            }
            result.state.has_lifenum = true;
            result.state.last_lifenum = event.numeric_value;
            break;

        case EventKind::kClearDeleted: {
            if (!result.state.open) {
                result.error = make_error(EngineErrorCode::kNotOpen, "P2REPL_CLEARDELETED requires OPEN");
                return result;
            }
            if (result.state.transaction_open) {
                result.error = make_error(
                    EngineErrorCode::kTransactionAlreadyOpen,
                    "P2REPL_CLEARDELETED is invalid while a transaction is open"
                );
                return result;
            }
            EngineError stream_error;
            const auto stream_index = require_stream_index(result.state, event.stream_code, &stream_error);
            if (stream_index == result.state.streams.size()) {
                result.error = stream_error;
                return result;
            }
            result.state.streams[stream_index].clear_deleted_count += 1;
            break;
        }
        }
    }

    if (result.state.transaction_open) {
        result.error = make_error(
            EngineErrorCode::kScenarioEndedWithOpenTransaction,
            "scenario ended with an open transaction"
        );
        return result;
    }

    if (!check_invariants(view, result.state, &result.error)) {
        return result;
    }

    return result;
}

const StreamState* Plaza2FakeEngine::find_stream_state(const EngineState& state, generated::StreamCode stream_code) {
    for (const auto& stream : state.streams) {
        if (stream.stream_code == stream_code) {
            return &stream;
        }
    }
    return nullptr;
}

} // namespace moex::plaza2::fake
