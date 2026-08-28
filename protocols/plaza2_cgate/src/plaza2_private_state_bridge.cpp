#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"

#include <algorithm>
#include <exception>
#include <ranges>
#include <utility>

namespace moex::plaza2::cgate {

namespace {

using fake::EngineState;
using fake::EventKind;
using fake::EventSpec;
using fake::FieldValueSpec;
using fake::RowSpec;
using generated::StreamCode;

std::string stream_name(StreamCode stream_code) {
    const auto* descriptor = generated::FindStreamByCode(stream_code);
    return descriptor == nullptr ? std::string{} : std::string(descriptor->stream_name);
}

std::size_t stream_index(const EngineState& state, StreamCode stream_code) {
    for (std::size_t index = 0; index < state.streams.size(); ++index) {
        if (state.streams[index].stream_code == stream_code) {
            return index;
        }
    }
    return state.streams.size();
}

} // namespace

Plaza2PrivateStateBridge::Plaza2PrivateStateBridge(private_state::Plaza2PrivateStateProjector& projector)
    : projector_(projector) {
    scenario_.scenario_id = "plaza2_private_state_bridge";
    scenario_.description = "shared CGate private-state bridge";
    scenario_.metadata_version = 1;
}

Plaza2Error Plaza2PrivateStateBridge::reset(std::span<const StreamCode> streams) {
    projector_.reset();
    state_ = {};
    state_.streams.clear();
    pending_row_deltas_.assign(streams.size(), 0);
    pending_clear_deleted_.assign(streams.size(), {});
    stream_lifenums_.clear();
    for (const auto stream_code : streams) {
        const auto* descriptor = generated::FindStreamByCode(stream_code);
        state_.streams.push_back({
            .stream_code = stream_code,
            .stream_name = descriptor == nullptr ? std::string_view{} : descriptor->stream_name,
        });
    }
    last_resync_reason_.clear();
    callback_error_.clear();
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::begin_run() {
    state_.open = true;
    state_.closed = false;
    projector_.on_event(scenario_, EventSpec{.kind = EventKind::kOpen}, state_);
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::end_run() {
    state_.closed = true;
    state_.online = false;
    state_.transaction_open = false;
    for (auto& stream : state_.streams) {
        stream.online = false;
        stream.snapshot_complete = false;
    }
    projector_.on_event(scenario_, EventSpec{.kind = EventKind::kClose}, state_);
    return {};
}

const EngineState& Plaza2PrivateStateBridge::state() const noexcept {
    return state_;
}

const std::string& Plaza2PrivateStateBridge::last_resync_reason() const noexcept {
    return last_resync_reason_;
}

const std::string& Plaza2PrivateStateBridge::callback_error() const noexcept {
    return callback_error_;
}

Plaza2Error Plaza2PrivateStateBridge::on_plaza2_listener_event(const Plaza2ListenerEvent& event) {
    try {
        return handle_event(event);
    } catch (const std::exception& error) {
        state_.callback_error_count += 1;
        callback_error_ = std::string("PLAZA II private-state bridge failed: ") + error.what();
        return {
            .code = Plaza2ErrorCode::CallbackFailed,
            .runtime_code = 0,
            .message = callback_error_,
        };
    } catch (...) {
        state_.callback_error_count += 1;
        callback_error_ = "PLAZA II private-state bridge failed with an unknown exception";
        return {
            .code = Plaza2ErrorCode::CallbackFailed,
            .runtime_code = 0,
            .message = callback_error_,
        };
    }
}

Plaza2Error Plaza2PrivateStateBridge::handle_event(const Plaza2ListenerEvent& event) {
    switch (event.kind) {
    case Plaza2ListenerEventKind::Open:
    case Plaza2ListenerEventKind::Timeout:
        return {};
    case Plaza2ListenerEventKind::Close:
        return handle_close(event.stream_code);
    case Plaza2ListenerEventKind::TransactionBegin:
        return handle_transaction_begin(event.stream_code);
    case Plaza2ListenerEventKind::TransactionCommit:
        return handle_transaction_commit(event.stream_code);
    case Plaza2ListenerEventKind::StreamData:
        return handle_stream_data(event);
    case Plaza2ListenerEventKind::Online:
        return handle_online(event.stream_code);
    case Plaza2ListenerEventKind::LifeNum:
        return handle_lifenum(event.stream_code, event.unsigned_value);
    case Plaza2ListenerEventKind::ClearDeleted:
        return handle_clear_deleted(event);
    case Plaza2ListenerEventKind::ReplState:
        return handle_replstate(event.text_value);
    }
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::handle_close(StreamCode stream_code) {
    const auto index = stream_index(state_, stream_code);
    if (index == state_.streams.size()) {
        return ordering_error("PLAZA II private-state bridge received CLOSE for an undeclared stream");
    }
    state_.streams[index].online = false;
    state_.streams[index].snapshot_complete = false;
    recompute_online();
    projector_.on_event(scenario_, EventSpec{.kind = EventKind::kClose, .stream_code = stream_code}, state_);
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::handle_transaction_begin(StreamCode stream_code) {
    if (state_.transaction_open) {
        return ordering_error("PLAZA II private-state bridge received nested TN_BEGIN");
    }
    const auto index = stream_index(state_, stream_code);
    if (index == state_.streams.size()) {
        return ordering_error("PLAZA II private-state bridge received TN_BEGIN for an undeclared stream");
    }
    std::fill(pending_row_deltas_.begin(), pending_row_deltas_.end(), 0);
    state_.transaction_open = true;
    projector_.on_event(scenario_, EventSpec{.kind = EventKind::kTransactionBegin, .stream_code = stream_code}, state_);
    for (const auto& pending : pending_clear_deleted_[index]) {
        projector_.on_event(scenario_,
                            EventSpec{.kind = EventKind::kClearDeleted,
                                      .stream_code = pending.stream_code,
                                      .table_code = pending.table_code,
                                      .signed_value = pending.table_rev,
                                      .clear_deleted_flags = pending.flags},
                            state_);
    }
    pending_clear_deleted_[index].clear();
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::handle_transaction_commit(StreamCode stream_code) {
    if (!state_.transaction_open) {
        return ordering_error("PLAZA II private-state bridge received TN_COMMIT without TN_BEGIN");
    }
    const auto index = stream_index(state_, stream_code);
    if (index == state_.streams.size()) {
        return ordering_error("PLAZA II private-state bridge received TN_COMMIT for an undeclared stream");
    }
    for (std::size_t delta_index = 0; delta_index < state_.streams.size(); ++delta_index) {
        state_.streams[delta_index].committed_row_count += pending_row_deltas_[delta_index];
    }
    std::fill(pending_row_deltas_.begin(), pending_row_deltas_.end(), 0);
    state_.transaction_open = false;
    state_.commit_count += 1;
    const EventSpec commit_event{.kind = EventKind::kTransactionCommit, .stream_code = stream_code};
    projector_.on_event(scenario_, commit_event, state_);
    projector_.on_transaction_commit(scenario_, commit_event, state_);
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::handle_stream_data(const Plaza2ListenerEvent& event) {
    if (!state_.transaction_open) {
        return ordering_error("PLAZA II private-state bridge received STREAM_DATA outside TN_BEGIN/TN_COMMIT");
    }
    const auto index = stream_index(state_, event.stream_code);
    if (index == state_.streams.size()) {
        return ordering_error("PLAZA II private-state bridge received STREAM_DATA for an undeclared stream");
    }

    text_storage_.clear();
    field_storage_.clear();
    text_storage_.reserve(event.fields.size());
    field_storage_.reserve(event.fields.size());
    for (const auto& field : event.fields) {
        FieldValueSpec decoded{.field_code = field.field_code};
        switch (field.kind) {
        case Plaza2DecodedValueKind::None:
            continue;
        case Plaza2DecodedValueKind::SignedInteger:
            decoded.kind = fake::ValueKind::kSignedInteger;
            decoded.signed_value = field.signed_value;
            break;
        case Plaza2DecodedValueKind::UnsignedInteger:
            decoded.kind = fake::ValueKind::kUnsignedInteger;
            decoded.unsigned_value = field.unsigned_value;
            break;
        case Plaza2DecodedValueKind::Decimal:
            decoded.kind = fake::ValueKind::kDecimal;
            text_storage_.emplace_back(field.text_value);
            decoded.text_value = text_storage_.back();
            break;
        case Plaza2DecodedValueKind::FloatingPoint:
            decoded.kind = fake::ValueKind::kFloatingPoint;
            text_storage_.emplace_back(field.text_value);
            decoded.text_value = text_storage_.back();
            break;
        case Plaza2DecodedValueKind::String:
            decoded.kind = fake::ValueKind::kString;
            text_storage_.emplace_back(field.text_value);
            decoded.text_value = text_storage_.back();
            break;
        case Plaza2DecodedValueKind::Timestamp:
            decoded.kind = fake::ValueKind::kTimestamp;
            decoded.unsigned_value = field.unsigned_value;
            break;
        }
        field_storage_.push_back(std::move(decoded));
    }

    // Keep table and signed_value (the runtime replRev) attached to the row.
    const EventSpec fake_event{
        .kind = EventKind::kStreamData,
        .stream_code = event.stream_code,
        .table_code = event.table_code,
        .signed_value = event.signed_value,
    };
    const RowSpec row{
        .stream_code = event.stream_code,
        .table_code = event.table_code,
        .field_count = static_cast<std::uint32_t>(field_storage_.size()),
    };
    projector_.on_stream_row(scenario_, fake_event, row, field_storage_, state_);
    pending_row_deltas_[index] += 1;
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::handle_online(StreamCode stream_code) {
    const auto index = stream_index(state_, stream_code);
    if (index == state_.streams.size()) {
        return ordering_error("PLAZA II private-state bridge received ONLINE for an undeclared stream");
    }
    for (const auto& pending : pending_clear_deleted_[index]) {
        projector_.on_event(scenario_,
                            EventSpec{.kind = EventKind::kClearDeleted,
                                      .stream_code = pending.stream_code,
                                      .table_code = pending.table_code,
                                      .signed_value = pending.table_rev,
                                      .clear_deleted_flags = pending.flags},
                            state_);
    }
    pending_clear_deleted_[index].clear();
    state_.streams[index].online = true;
    state_.streams[index].snapshot_complete = true;
    recompute_online();
    projector_.on_event(scenario_, EventSpec{.kind = EventKind::kOnline, .stream_code = stream_code}, state_);
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::handle_lifenum(StreamCode stream_code, std::uint64_t life_number) {
    if (state_.transaction_open) {
        return ordering_error("PLAZA II private-state bridge received P2REPL_LIFENUM inside an open transaction");
    }
    auto known_stream =
        std::ranges::find_if(stream_lifenums_, [stream_code](const auto& entry) { return entry.first == stream_code; });
    if (known_stream != stream_lifenums_.end() && known_stream->second == life_number) {
        return {};
    }
    const bool stream_lifenum_seen = known_stream != stream_lifenums_.end();
    if (known_stream == stream_lifenums_.end()) {
        stream_lifenums_.emplace_back(stream_code, life_number);
    } else {
        known_stream->second = life_number;
    }
    state_.has_lifenum = true;
    state_.last_lifenum = life_number;
    if (stream_lifenum_seen) {
        const auto index = stream_index(state_, stream_code);
        if (index < state_.streams.size()) {
            pending_clear_deleted_[index].clear();
            state_.streams[index].online = false;
            state_.streams[index].snapshot_complete = false;
            state_.streams[index].committed_row_count = 0;
        }
        recompute_online();
        last_resync_reason_ = "lifenum_change:" + stream_name(stream_code);
    }
    const EventSpec fake_event{
        .kind = EventKind::kLifeNum,
        .stream_code = stream_code,
        .numeric_value = life_number,
    };
    projector_.on_event(scenario_, fake_event, state_);
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::handle_clear_deleted(const Plaza2ListenerEvent& event) {
    const auto index = stream_index(state_, event.stream_code);
    if (index == state_.streams.size()) {
        return ordering_error("PLAZA II private-state bridge received P2REPL_CLEARDELETED for an undeclared stream");
    }
    state_.streams[index].clear_deleted_count += 1;
    // CLEARDELETED is table/revision scoped; it does not imply listener CLOSE.
    projector_.invalidate_periodic_snapshot(event.stream_code, event.table_code);
    pending_clear_deleted_[index].push_back({
        .stream_code = event.stream_code,
        .table_code = event.table_code,
        .table_rev = event.signed_value,
        .flags = event.clear_deleted_flags,
    });
    last_resync_reason_ = "clear_deleted:" + stream_name(event.stream_code);
    if (state_.transaction_open) {
        const auto pending = pending_clear_deleted_[index].back();
        projector_.on_event(scenario_,
                            EventSpec{.kind = EventKind::kClearDeleted,
                                      .stream_code = pending.stream_code,
                                      .table_code = pending.table_code,
                                      .signed_value = pending.table_rev,
                                      .clear_deleted_flags = pending.flags},
                            state_);
        pending_clear_deleted_[index].pop_back();
    }
    return {};
}

Plaza2Error Plaza2PrivateStateBridge::handle_replstate(std::string_view replstate) {
    if (state_.transaction_open) {
        return ordering_error("PLAZA II private-state bridge received P2REPL_REPLSTATE inside an open transaction");
    }
    state_.last_replstate.assign(replstate);
    projector_.on_event(scenario_, EventSpec{.kind = EventKind::kReplState, .text_value = state_.last_replstate},
                        state_);
    return {};
}

void Plaza2PrivateStateBridge::recompute_online() {
    state_.online =
        !state_.streams.empty() && std::all_of(state_.streams.begin(), state_.streams.end(),
                                               [](const fake::StreamState& stream) { return stream.online; });
}

Plaza2Error Plaza2PrivateStateBridge::ordering_error(std::string message) {
    callback_error_ = message;
    return {
        .code = Plaza2ErrorCode::AdapterState,
        .runtime_code = 0,
        .message = std::move(message),
    };
}

} // namespace moex::plaza2::cgate
