#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"
#include <algorithm>
#include <charconv>
#include <sstream>
#include <tuple>

namespace moex::plaza2::cgate {
namespace {
using generated::FieldCode;
std::optional<std::int64_t> parse_i64(std::string_view value) {
    std::int64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::int64_t> signed_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger) {
            return field.signed_value;
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger) {
            return static_cast<std::int64_t>(field.unsigned_value);
        }
        if (field.kind == Plaza2DecodedValueKind::String || field.kind == Plaza2DecodedValueKind::Decimal) {
            return parse_i64(field.text_value);
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> unsigned_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger || field.kind == Plaza2DecodedValueKind::Timestamp) {
            return field.unsigned_value;
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger && field.signed_value >= 0) {
            return static_cast<std::uint64_t>(field.signed_value);
        }
    }
    return std::nullopt;
}

std::string text_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::String || field.kind == Plaza2DecodedValueKind::Decimal ||
            field.kind == Plaza2DecodedValueKind::FloatingPoint) {
            return std::string(field.text_value);
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger) {
            return std::to_string(field.signed_value);
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger) {
            return std::to_string(field.unsigned_value);
        }
    }
    return {};
}

const Plaza2DecodedFieldValue* find_decoded_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code == code)
            return &field;
    }
    return nullptr;
}

} // namespace

void Plaza2Aggr20ListenerBridge::refresh_ready() noexcept {
    session_data_ready_ = ready_event_ && ready_event_->source_repl_act == 0 &&
                          accepts_session(ready_event_->sess_id) && !clearing_started_;
    valid_ = transport_active_ && online_ && snapshot_complete_ && !projector_.transaction_open() && !recovering() &&
             session_data_ready_;
}
void Plaza2Aggr20ListenerBridge::set_session_id(std::int32_t id) noexcept {
    expected_session_id_ = id;
    refresh_ready();
}
bool Plaza2Aggr20ListenerBridge::accepts_session(std::int32_t id) const noexcept {
    return id > 0 && (expected_session_id_ == 0 || expected_session_id_ == id);
}
void Plaza2Aggr20ListenerBridge::invalidate(bool reopen, bool active) noexcept {
    projector_.reset();
    ++stream_epoch_;
    transport_active_ = active;
    online_ = snapshot_complete_ = session_data_ready_ = valid_ = false;
    reopen_required_ = reopen;
    ready_event_.reset();
    last_sys_event_.reset();
    pending_sys_events_.clear();
    clearing_started_ = false;
}
void Plaza2Aggr20ListenerBridge::reset() noexcept {
    invalidate(false, false);
    has_lifenum_ = false;
    last_lifenum_ = 0;
}
Plaza2Aggr20Status Plaza2Aggr20ListenerBridge::status() const {
    return {.transport_active = transport_active_,
            .snapshot_complete = snapshot_complete_,
            .session_data_ready = session_data_ready_,
            .aggr_online = online_,
            .valid = valid_ && !projector_.transaction_open(),
            .stream_epoch = stream_epoch_,
            .ready_event = ready_event_,
            .last_sys_event = last_sys_event_};
}
Plaza2Error Plaza2Aggr20ListenerBridge::on_plaza2_listener_event(const Plaza2ListenerEvent& event) {
    switch (event.kind) {
    case Plaza2ListenerEventKind::Open:
        invalidate(false, true);
        break;
    case Plaza2ListenerEventKind::LifeNum:
        invalidate(false, transport_active_);
        has_lifenum_ = true;
        last_lifenum_ = event.unsigned_value;
        break;
    case Plaza2ListenerEventKind::Close:
        invalidate(true, false);
        break;
    case Plaza2ListenerEventKind::ClearDeleted:
        if (event.table_code == generated::TableCode::kFortsAggrReplOrdersAggr) {
            projector_.clear_deleted(event.signed_value);
        }
        break;
    case Plaza2ListenerEventKind::TransactionBegin:
        if (!reopen_required_) {
            pending_sys_events_.clear();
            projector_.begin_transaction();
        }
        break;
    case Plaza2ListenerEventKind::TransactionCommit: {
        if (reopen_required_)
            return {};
        if (const auto error = projector_.commit(); error)
            return error;
        const auto chronology = [](const auto& value) {
            return std::tuple{value.source_repl_rev, value.event_id, value.source_repl_id};
        };
        std::stable_sort(pending_sys_events_.begin(), pending_sys_events_.end(),
                         [&](const auto& lhs, const auto& rhs) { return chronology(lhs) < chronology(rhs); });
        for (const auto& value : pending_sys_events_) {
            if (last_sys_event_ && chronology(value) < chronology(*last_sys_event_))
                continue;
            last_sys_event_ = value;
            if (ready_event_ && (value.source_repl_id == ready_event_->source_repl_id ||
                                 (value.source_repl_act > 0 &&
                                  static_cast<std::uint64_t>(value.source_repl_act) == ready_event_->source_repl_id)))
                ready_event_.reset();
            if (value.source_repl_act != 0)
                continue;
            if (value.event_type == 1) {
                // Snapshot and online rows have the same committed meaning.
                ready_event_ = value;
                clearing_started_ = false;
            } else if (value.event_type == 5 && ready_event_ && value.sess_id == ready_event_->sess_id) {
                clearing_started_ = true;
            }
        }
        pending_sys_events_.clear();
        refresh_ready();
        break;
    }
    case Plaza2ListenerEventKind::StreamData:
        if (reopen_required_)
            return {};
        if (event.table_code == generated::TableCode::kFortsAggrReplOrdersAggr) {
            const auto error = projector_.on_row(event.fields);
            return error;
        }
        if (event.table_code == generated::TableCode::kFortsAggrReplSysEvents) {
            pending_sys_events_.push_back(
                {.source_repl_id = unsigned_field(event.fields, FieldCode::kFortsAggrReplSysEventsReplId).value_or(0),
                 .source_repl_rev = signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsReplRev).value_or(0),
                 .source_repl_act = signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsReplAct).value_or(0),
                 .event_type = static_cast<std::int32_t>(
                     signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsEventType).value_or(0)),
                 .event_id = signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsEventId).value_or(0),
                 .sess_id = static_cast<std::int32_t>(
                     signed_field(event.fields, FieldCode::kFortsAggrReplSysEventsSessId).value_or(0)),
                 .message = text_field(event.fields, FieldCode::kFortsAggrReplSysEventsMessage),
                 .server_time = unsigned_field(event.fields, FieldCode::kFortsAggrReplSysEventsServerTime).value_or(0),
                 .seen_during_snapshot = !snapshot_complete_});
        }
        break;
    case Plaza2ListenerEventKind::Online:
        if (!reopen_required_ && !projector_.transaction_open()) {
            transport_active_ = online_ = snapshot_complete_ = true;
            refresh_ready();
        }
        break;
    default:
        break;
    }
    return {};
}
Plaza2Aggr20BookProjector::Plaza2Aggr20BookProjector(NowFn now)
    : now_(now ? std::move(now) : [] { return Clock::now(); }) {}
void Plaza2Aggr20BookProjector::reset() {
    staged_.clear();
    rows_.clear();
    books_.clear();
    committed_ = {};
    dirty_ = true;
    transaction_open_ = false;
}
void Plaza2Aggr20BookProjector::begin_transaction() {
    staged_.clear();
    transaction_open_ = true;
}
Plaza2Error Plaza2Aggr20BookProjector::on_row(std::span<const Plaza2DecodedFieldValue> fields) {
    if (!transaction_open_)
        return {.code = Plaza2ErrorCode::AdapterState, .message = "AGGR20 row outside transaction"};
    Plaza2Aggr20Level row;
    row.repl_id = unsigned_field(fields, FieldCode::kFortsAggrReplOrdersAggrReplId).value_or(0);
    row.repl_rev = signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrReplRev).value_or(0);
    row.isin_id = signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrIsinId).value_or(0);
    row.volume = signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrVolume).value_or(0);
    if (signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrReplAct).value_or(0) != 0 || row.volume == 0) {
        row.volume = 0;
        // Deleted rows need only replID; their other columns may be NULL.
        staged_.push_back({.row = std::move(row)});
        return {};
    }
    const auto* price = find_decoded_field(fields, FieldCode::kFortsAggrReplOrdersAggrPrice);
    std::optional<std::int64_t> scaled;
    if (price && (price->kind == Plaza2DecodedValueKind::Decimal || price->kind == Plaza2DecodedValueKind::String)) {
        if (price->decimal_exact && price->decimal_scale == 5)
            scaled = price->decimal_mantissa;
        else if (price->raw_value.empty() && price->type_token.empty())
            scaled = parse_fixed_point(price->text_value, 5, true, kPlaza2D16_5DecimalPrecision);
    }
    if (!scaled)
        return {.code = Plaza2ErrorCode::DecodeFailed, .message = "AGGR20 orders_aggr.price is malformed or missing"};
    row.price_scaled = *scaled;
    row.price = std::string(price->text_value);
    row.dir = static_cast<std::int32_t>(signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrDir).value_or(0));
    row.moment = unsigned_field(fields, FieldCode::kFortsAggrReplOrdersAggrMoment).value_or(0);
    row.moment_ns = unsigned_field(fields, FieldCode::kFortsAggrReplOrdersAggrMomentNs).value_or(0);
    row.synth_volume = text_field(fields, FieldCode::kFortsAggrReplOrdersAggrSynthVolume);
    if (row.repl_id == 0 || row.isin_id <= 0 || (row.dir != 1 && row.dir != 2) || row.volume < 0)
        return {.code = Plaza2ErrorCode::DecodeFailed, .message = "AGGR20 invalid row identity, side or volume"};
    staged_.push_back({.row = std::move(row)});
    return {};
}
void Plaza2Aggr20BookProjector::clear_deleted(std::int64_t revision) {
    const bool implicit = !transaction_open_;
    if (implicit)
        begin_transaction();
    staged_.push_back({.clear = true, .revision = revision});
    if (implicit)
        (void)commit();
}
void Plaza2Aggr20BookProjector::erase_row(std::uint64_t id, std::unordered_set<std::int64_t>& affected) {
    const auto found = rows_.find(id);
    if (found == rows_.end())
        return;
    const auto& row = found->second;
    affected.insert(row.isin_id);
    auto& book = books_[row.isin_id];
    (row.dir == 1 ? book.bids : book.asks).erase({row.price_scaled, row.repl_id});
    rows_.erase(found);
}
Plaza2Error Plaza2Aggr20BookProjector::commit() {
    if (!transaction_open_)
        return {.code = Plaza2ErrorCode::AdapterState, .message = "AGGR20 commit without transaction"};
    std::unordered_set<std::int64_t> affected;
    const auto at = now_();
    for (auto& op : staged_) {
        if (op.clear) {
            // CG_MAX_REVISON is the signed int64 maximum.
            std::vector<std::uint64_t> deleted;
            for (const auto& [id, row] : rows_)
                if (row.repl_rev < op.revision || op.revision == std::numeric_limits<std::int64_t>::max())
                    deleted.push_back(id);
            for (const auto id : deleted)
                erase_row(id, affected);
            if (op.revision == std::numeric_limits<std::int64_t>::max()) {
                committed_.last_repl_rev = 0;
                committed_.last_repl_id = 0;
                for (auto& [id, book] : books_)
                    book.last_repl_rev = book.last_repl_id = 0;
            }
            continue;
        }
        const auto id = op.row.repl_id;
        auto existing = rows_.find(id);
        if (existing != rows_.end() && op.row.repl_rev < existing->second.repl_rev)
            continue;
        const auto old_isin = existing == rows_.end() ? op.row.isin_id : existing->second.isin_id;
        erase_row(id, affected);
        if (old_isin > 0) {
            affected.insert(old_isin);
            auto& old_book = books_[old_isin];
            old_book.last_repl_rev = std::max(old_book.last_repl_rev, op.row.repl_rev);
            old_book.last_repl_id = std::max(old_book.last_repl_id, id);
        }
        if (op.row.volume > 0) {
            const auto isin = op.row.isin_id;
            auto& book = books_[isin];
            affected.insert(isin);
            (op.row.dir == 1 ? book.bids : book.asks)[{op.row.price_scaled, id}] = id;
            book.last_repl_id = std::max(book.last_repl_id, id);
            book.last_repl_rev = std::max(book.last_repl_rev, op.row.repl_rev);
            book.moment = std::max(book.moment, op.row.moment);
            book.moment_ns = std::max(book.moment_ns, op.row.moment_ns);
            rows_[id] = std::move(op.row);
        }
        committed_.last_repl_id = std::max(committed_.last_repl_id, id);
        committed_.last_repl_rev = std::max(committed_.last_repl_rev, op.row.repl_rev);
    }
    for (const auto isin : affected) {
        books_[isin].version = ++version_;
        books_[isin].committed_at = at;
    }
    committed_.committed_at = at;
    staged_.clear();
    transaction_open_ = false;
    dirty_ = true;
    if (qualification_observer_)
        qualification_observer_->committed(snapshot());
    return {};
}
void Plaza2Aggr20BookProjector::rollback() {
    staged_.clear();
    transaction_open_ = false;
}
Plaza2Aggr20InstrumentSnapshot Plaza2Aggr20BookProjector::make_snapshot(std::int64_t id,
                                                                        const InstrumentBook& book) const {
    Plaza2Aggr20InstrumentSnapshot out{.isin_id = id};
    out.row_count = book.bids.size() + book.asks.size();
    out.bid_depth_levels = book.bids.size();
    out.ask_depth_levels = book.asks.size();
    out.last_repl_id = book.last_repl_id;
    out.last_repl_rev = book.last_repl_rev;
    out.source_snapshot_version = book.version;
    out.committed_at = book.committed_at;
    out.exchange_moment = book.moment;
    out.exchange_moment_ns = book.moment_ns;
    out.levels.reserve(out.row_count);
    for (auto it = book.bids.rbegin(); it != book.bids.rend(); ++it)
        out.levels.push_back(rows_.at(it->second));
    if (!out.levels.empty())
        out.top_bid = out.levels.front();
    const auto bid_count = out.levels.size();
    for (const auto& [key, row] : book.asks)
        out.levels.push_back(rows_.at(row));
    if (out.levels.size() > bid_count)
        out.top_ask = out.levels[bid_count];
    return out;
}
std::optional<Plaza2Aggr20InstrumentSnapshot> Plaza2Aggr20BookProjector::snapshot_for_isin(std::int64_t id) const {
    const auto found = books_.find(id);
    return found == books_.end() ? std::nullopt : std::optional{make_snapshot(id, found->second)};
}
const Plaza2Aggr20Snapshot& Plaza2Aggr20BookProjector::snapshot() const {
    if (!dirty_)
        return committed_;
    committed_.levels.clear();
    committed_.top_bid.reset();
    committed_.top_ask.reset();
    committed_.bid_depth_levels = committed_.ask_depth_levels = committed_.instrument_count = 0;
    committed_.exchange_moment = committed_.exchange_moment_ns = 0;
    committed_.row_count = rows_.size();
    committed_.levels.reserve(rows_.size());
    for (const auto& [id, book] : books_) {
        if (!book.bids.empty() || !book.asks.empty())
            ++committed_.instrument_count;
        auto scoped = make_snapshot(id, book);
        committed_.bid_depth_levels += scoped.bid_depth_levels;
        committed_.ask_depth_levels += scoped.ask_depth_levels;
        committed_.exchange_moment = std::max(committed_.exchange_moment, scoped.exchange_moment);
        committed_.exchange_moment_ns = std::max(committed_.exchange_moment_ns, scoped.exchange_moment_ns);
        if (scoped.top_bid && (!committed_.top_bid || scoped.top_bid->price_scaled > committed_.top_bid->price_scaled))
            committed_.top_bid = scoped.top_bid;
        if (scoped.top_ask && (!committed_.top_ask || scoped.top_ask->price_scaled < committed_.top_ask->price_scaled))
            committed_.top_ask = scoped.top_ask;
        committed_.levels.insert(committed_.levels.end(), scoped.levels.begin(), scoped.levels.end());
    }
    std::sort(committed_.levels.begin(), committed_.levels.end(),
              [](const auto& a, const auto& b) { return a.repl_id < b.repl_id; });
    dirty_ = false;
    return committed_;
}
bool Plaza2Aggr20BookProjector::transaction_open() const noexcept {
    return transaction_open_;
}
} // namespace moex::plaza2::cgate
