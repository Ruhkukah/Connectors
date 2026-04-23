#pragma once

#include "plaza2_generated_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2::fake {

inline constexpr generated::StreamCode kNoStreamCode = static_cast<generated::StreamCode>(0);
inline constexpr generated::TableCode kNoTableCode = static_cast<generated::TableCode>(0);
inline constexpr generated::FieldCode kNoFieldCode = static_cast<generated::FieldCode>(0);

enum class EventKind : std::uint8_t {
    kOpen = 0,
    kClose = 1,
    kSnapshotBegin = 2,
    kSnapshotEnd = 3,
    kOnline = 4,
    kTransactionBegin = 5,
    kTransactionCommit = 6,
    kStreamData = 7,
    kReplState = 8,
    kLifeNum = 9,
    kClearDeleted = 10,
};

enum class InvariantKind : std::uint8_t {
    kCommitCount = 0,
    kLastLifenum = 1,
    kStreamOnline = 2,
    kLastReplstate = 3,
    kClearDeletedCount = 4,
};

enum class ValueKind : std::uint8_t {
    kNone = 0,
    kSignedInteger = 1,
    kUnsignedInteger = 2,
    kDecimal = 3,
    kFloatingPoint = 4,
    kString = 5,
    kTimestamp = 6,
};

enum class EngineErrorCode : std::uint8_t {
    kNone = 0,
    kMalformedScenario = 1,
    kAlreadyOpen = 2,
    kNotOpen = 3,
    kEventAfterClose = 4,
    kSnapshotAlreadyActive = 5,
    kSnapshotNotActive = 6,
    kOnlineBeforeSnapshotEnd = 7,
    kTransactionAlreadyOpen = 8,
    kTransactionNotOpen = 9,
    kStreamDataOutsideTransaction = 10,
    kStreamNotDeclared = 11,
    kInvariantFailed = 12,
    kScenarioEndedWithOpenTransaction = 13,
};

struct FieldValueSpec {
    generated::FieldCode field_code{kNoFieldCode};
    ValueKind kind{ValueKind::kNone};
    std::int64_t signed_value{0};
    std::uint64_t unsigned_value{0};
    std::string_view text_value{};
};

struct RowSpec {
    generated::StreamCode stream_code{kNoStreamCode};
    generated::TableCode table_code{kNoTableCode};
    std::uint32_t first_field_index{0};
    std::uint32_t field_count{0};
};

struct EventSpec {
    EventKind kind{EventKind::kOpen};
    generated::StreamCode stream_code{kNoStreamCode};
    generated::TableCode table_code{kNoTableCode};
    std::uint32_t first_row_index{0};
    std::uint32_t row_count{0};
    std::uint64_t numeric_value{0};
    std::string_view text_value{};
};

struct InvariantSpec {
    InvariantKind kind{InvariantKind::kCommitCount};
    generated::StreamCode stream_code{kNoStreamCode};
    std::uint64_t numeric_value{0};
    bool bool_value{false};
    std::string_view text_value{};
};

struct ScenarioSpec {
    std::string_view scenario_id{};
    std::string_view description{};
    std::uint32_t metadata_version{0};
    std::uint32_t deterministic_seed{0};
    std::uint32_t first_stream_index{0};
    std::uint32_t stream_count{0};
    std::uint32_t first_event_index{0};
    std::uint32_t event_count{0};
    std::uint32_t first_invariant_index{0};
    std::uint32_t invariant_count{0};
};

struct ScenarioDataView {
    const ScenarioSpec& scenario;
    std::span<const generated::StreamCode> streams;
    std::span<const EventSpec> events;
    std::span<const RowSpec> rows;
    std::span<const FieldValueSpec> fields;
    std::span<const InvariantSpec> invariants;
};

struct StreamState {
    generated::StreamCode stream_code{kNoStreamCode};
    std::string_view stream_name{};
    bool online{false};
    bool snapshot_complete{false};
    std::uint64_t clear_deleted_count{0};
    std::uint64_t committed_row_count{0};
};

struct EngineState {
    bool open{false};
    bool closed{false};
    bool snapshot_active{false};
    bool online{false};
    bool transaction_open{false};
    std::uint64_t commit_count{0};
    bool has_lifenum{false};
    std::uint64_t last_lifenum{0};
    std::string last_replstate;
    std::uint64_t callback_error_count{0};
    std::vector<StreamState> streams;
};

struct EngineError {
    EngineErrorCode code{EngineErrorCode::kNone};
    std::string message;

    explicit operator bool() const {
        return code != EngineErrorCode::kNone;
    }
};

struct RunResult {
    EngineState state;
    EngineError error;
};

class CommitListener {
  public:
    virtual ~CommitListener() = default;
    virtual void on_transaction_commit(const ScenarioSpec& scenario, const EventSpec& commit_event,
                                       const EngineState& state) = 0;
};

class Plaza2FakeEngine {
  public:
    [[nodiscard]] RunResult run(const ScenarioDataView& view, CommitListener* listener = nullptr) const;

    [[nodiscard]] static const StreamState* find_stream_state(const EngineState& state,
                                                              generated::StreamCode stream_code);
};

} // namespace moex::plaza2::fake
