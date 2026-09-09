#include "moex/plaza2/cgate/plaza2_ordlog.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace moex::plaza2::cgate {
namespace {
constexpr auto stream = generated::StreamCode::kFortsOrdlogRepl;
// Padding is not a protocol field and cannot determine equality.
bool same_record(const OrdlogRecord& a, const OrdlogRecord& b, const public_wire::Table& table) {
    for (std::size_t i = 0; i < table.fields.size(); ++i) {
        if (a.nulls[i] != b.nulls[i])
            return false;
        const auto& f = table.fields[i];
        if (!a.nulls[i] && std::memcmp(a.wire.data() + f.offset, b.wire.data() + f.offset, f.size))
            return false;
    }
    return true;
}
} // namespace

Plaza2Ordlog::Plaza2Ordlog(std::size_t capacity, std::size_t optional_capacity)
    : queue_(capacity), optional_(optional_capacity) {
    if (!capacity)
        throw std::invalid_argument("ORDLOG mandatory capacity must be positive");
}

Plaza2Error Plaza2Ordlog::create(Plaza2Connection& connection, std::string_view scheme_file) {
    if (scheme_file.empty() || scheme_file.find_first_of(";|\r\n") != std::string_view::npos) {
        return {.code = Plaza2ErrorCode::InvalidConfiguration, .message = "invalid ORDLOG scheme path"};
    }
    connection_ = &connection;
    return listener_.create(connection, stream,
                            "p2repl://FORTS_ORDLOG_REPL;scheme=|FILE|" + std::string(scheme_file) + "|CustReplScheme",
                            this);
}

Plaza2Error Plaza2Ordlog::open() {
    if (read_ != committed_) {
        return {.code = Plaza2ErrorCode::AdapterState, .message = "drain mandatory ORDLOG output before reopening"};
    }
    const auto token = std::string(checkpoint());
    if (const auto error = listener_.close(); error)
        return error;
    invalidate_uncommitted();
    failed_ = false;
    source_online_ = false;
    if (token.empty()) {
        have_commit_ = false;
        checkpoint_.clear();
        revisions_ = {};
        committed_revisions_ = {};
        ++generation_;
    }
    state_ = OrdlogState::Opening;
    enabled_ = true;
    const auto error = listener_.open(token.empty() ? "mode=snapshot+online" : "replstate=" + token);
    if (error && !failed_) {
        state_ = OrdlogState::Recovering;
        retry_after_ns_ = now_ns_ + 1000000000ULL;
    }
    return error;
}

Plaza2Error Plaza2Ordlog::close() {
    enabled_ = false;
    const auto error = listener_.close();
    source_online_ = false;
    invalidate_uncommitted();
    state_ = OrdlogState::Disabled;
    return error;
}

Plaza2Error Plaza2Ordlog::supervise(std::uint64_t monotonic_ns) {
    poll_time(monotonic_ns);
    if (!enabled_ || failed_ || !listener_.is_created())
        return {};
    std::uint32_t connection_state = 0;
    if (connection_) {
        if (const auto error = connection_->state(connection_state); error)
            return error;
        if (connection_state != 3) {
            if (state_ != OrdlogState::Recovering) {
                if (const auto error = listener_.close(); error)
                    return error;
                if (transaction_)
                    checkpoint_.clear();
                invalidate_uncommitted();
                source_online_ = false;
                state_ = OrdlogState::Recovering;
                retry_after_ns_ = now_ns_ + 1000000000ULL;
            }
            return {}; // Shared connection recovery belongs to its existing owner.
        }
    }
    std::uint32_t runtime_state = 0;
    if (const auto error = listener_.state(runtime_state); error)
        return error;
    // CG_STATE_ERROR=1; CG_STATE_CLOSED=0. ACTIVE/OPENING are driven by callbacks.
    if (runtime_state == 1 || runtime_state == 0) {
        if (state_ != OrdlogState::Recovering) {
            source_online_ = false;
            state_ = OrdlogState::Recovering;
            retry_after_ns_ = now_ns_ + 1000000000ULL;
        }
        if (now_ns_ >= retry_after_ns_ && read_ == committed_)
            return open();
    }
    return {};
}

void Plaza2Ordlog::invalidate_uncommitted() noexcept {
    metrics_.uncommitted_rolled_back += write_ - committed_;
    write_ = committed_;
    revisions_ = committed_revisions_;
    transaction_ = false;
}

void Plaza2Ordlog::on_plaza2_listener_error(const Plaza2Error&) noexcept {
    if (!failed_)
        ++metrics_.decode_failures;
    failed_ = true;
    source_online_ = false;
    checkpoint_.clear();
    state_ = OrdlogState::Failed;
    invalidate_uncommitted();
}

Plaza2Error Plaza2Ordlog::fail(std::string_view message, bool decode, bool revision) {
    metrics_.decode_failures += decode;
    metrics_.revision_failures += revision;
    failed_ = true;
    source_online_ = false;
    checkpoint_.clear();
    state_ = OrdlogState::Failed;
    invalidate_uncommitted();
    return {.code = Plaza2ErrorCode::DecodeFailed, .message = std::string(message)};
}

Plaza2Error Plaza2Ordlog::append(const OrdlogRecord& record) {
    if (write_ - read_ == queue_.size()) {
        ++metrics_.mandatory_overflows;
        ++metrics_.dropped;
        return fail("mandatory ORDLOG queue overflow; fresh history required");
    }
    queue_[write_++ % queue_.size()] = record;
    if (source_online_)
        state_ = OrdlogState::CatchingUp;
    metrics_.high_water = std::max(metrics_.high_water, static_cast<std::size_t>(write_ - read_));
    return {};
}

void Plaza2Ordlog::publish() noexcept {
    if (!optional_.empty()) {
        for (auto i = committed_; i != write_; ++i) {
            const auto& record = queue_[i % queue_.size()];
            if (optional_failed_ || optional_write_ - optional_read_ == optional_.size()) {
                if (!optional_failed_) {
                    optional_failed_ = true;
                    ++metrics_.optional_overflows;
                    metrics_.optional_first_lost_sequence = record.sequence;
                }
                ++metrics_.optional_dropped;
            } else {
                optional_[optional_write_++ % optional_.size()] = record;
            }
        }
    }
    committed_ = write_;
    committed_revisions_ = revisions_;
}

Plaza2Error Plaza2Ordlog::on_plaza2_listener_event(const Plaza2ListenerEvent& event) {
    using Kind = Plaza2ListenerEventKind;
    if (event.stream_code != stream)
        return fail("unexpected ORDLOG stream", true);
    if (failed_ && event.kind != Kind::Close && event.kind != Kind::ReplState) {
        return {.code = Plaza2ErrorCode::AdapterState, .message = "ORDLOG requires fresh recovery after failure"};
    }
    switch (event.kind) {
    case Kind::Open:
        source_online_ = false;
        state_ = OrdlogState::History;
        break;
    case Kind::TransactionBegin:
        if (transaction_)
            return fail("nested ORDLOG transaction", false, true);
        checkpoint_.clear();
        transaction_ = true;
        break;
    case Kind::TransactionCommit:
        if (!transaction_)
            return fail("ORDLOG commit without begin", false, true);
        transaction_ = false;
        have_commit_ = true;
        publish();
        break;
    case Kind::Online:
        if (transaction_)
            return fail("ORDLOG online before transaction commit", false, true);
        source_online_ = true;
        state_ = read_ == committed_ ? OrdlogState::Online : OrdlogState::CatchingUp;
        break;
    case Kind::Close:
        if (transaction_)
            checkpoint_.clear();
        invalidate_uncommitted();
        source_online_ = false;
        if (!failed_)
            state_ = OrdlogState::Stale;
        break;
    case Kind::ReplState:
        if (!failed_ && have_commit_ && !transaction_ && event.text_value.size() <= 65536)
            checkpoint_ = event.text_value;
        break;
    case Kind::LifeNum: {
        // Even an initial life marker is exposed. A changed life invalidates the complete generation.
        if (life_ != event.unsigned_value) {
            if (transaction_)
                return fail("LifeNum changed inside ORDLOG transaction", false, true);
            have_commit_ = false;
            life_ = event.unsigned_value;
            ++generation_;
            ++metrics_.generation_changes;
            revisions_ = {};
            committed_revisions_ = {};
            source_online_ = false;
            state_ = OrdlogState::History;
            checkpoint_.clear();
        }
        OrdlogRecord record{.kind = event.kind,
                            .revision = OrdlogRevision::GenerationChange,
                            .sequence = ++sequence_,
                            .life = life_,
                            .generation = generation_,
                            .received_ns = now_ns_};
        if (auto error = append(record); error)
            return error;
        if (!transaction_)
            publish();
        break;
    }
    case Kind::ClearDeleted: {
        const auto* table = public_wire::table(event.table_code);
        if (!table || table->stream != stream)
            return fail("unknown ORDLOG clear-deleted table", true);
        checkpoint_.clear();
        if (event.signed_value == std::numeric_limits<std::int64_t>::max())
            revisions_[table->index] = {};
        OrdlogRecord record{.kind = event.kind,
                            .table = event.table_code,
                            .table_index = table->index,
                            .sequence = ++sequence_,
                            .life = life_,
                            .generation = generation_,
                            .received_ns = now_ns_,
                            .repl_rev = event.signed_value,
                            .flags = event.clear_deleted_flags};
        if (auto error = append(record); error)
            return error;
        if (!transaction_)
            publish();
        break;
    }
    case Kind::StreamData: {
        ++metrics_.records;
        checkpoint_.clear();
        const auto* table = public_wire::table(event.table_code);
        if (!table || table->stream != stream || table->index != event.table_index ||
            public_wire::validate(*table, event.raw_payload, event.raw_nulls) != public_wire::DecodeResult::Ok) {
            return fail("malformed or unsupported ORDLOG record", true);
        }
        if (!transaction_)
            return fail("ORDLOG record outside transaction", false, true);
        OrdlogRecord record{.kind = event.kind,
                            .table = event.table_code,
                            .table_index = table->index,
                            .size = table->size,
                            .null_count = static_cast<std::uint16_t>(event.raw_nulls.size()),
                            .sequence = ++sequence_,
                            .life = life_,
                            .generation = generation_,
                            .received_ns = now_ns_,
                            .repl_id = public_wire::load<std::int64_t>(event.raw_payload),
                            .repl_rev = public_wire::load<std::int64_t>(event.raw_payload, 8),
                            .repl_act = public_wire::load<std::int64_t>(event.raw_payload, 16)};
        std::memcpy(record.wire.data(), event.raw_payload.data(), table->size);
        std::copy(event.raw_nulls.begin(), event.raw_nulls.end(), record.nulls.begin());
        auto& tracker = revisions_[table->index];
        if (record.repl_rev <= 0 || event.signed_value != record.repl_rev) {
            return fail("invalid ORDLOG replication revision", false, true);
        }
        if (tracker.present && record.repl_rev == tracker.last.repl_rev && same_record(record, tracker.last, *table)) {
            record.revision = OrdlogRevision::Duplicate;
            ++metrics_.duplicates;
        } else if (tracker.present && record.repl_rev <= tracker.maximum) {
            if (source_online_)
                return fail("conflicting online ORDLOG revision", false, true);
            record.revision = OrdlogRevision::Replay;
            ++metrics_.replays;
        } else if (tracker.present && record.repl_rev - tracker.maximum > 1) {
            record.revision = OrdlogRevision::Discontinuity;
            ++metrics_.discontinuities; // A numeric jump is observable, not evidence of dropped transport data.
        }
        tracker.maximum = std::max(tracker.maximum, record.repl_rev);
        tracker.last = record;
        tracker.present = true;
        if (auto error = append(record); error)
            return error;
        break;
    }
    default:
        return fail("unsupported ORDLOG callback type", true);
    }
    return {};
}

const OrdlogRecord* Plaza2Ordlog::front() const noexcept {
    return read_ == committed_ ? nullptr : &queue_[read_ % queue_.size()];
}
bool Plaza2Ordlog::acknowledge() noexcept {
    if (!front())
        return false;
    ++read_;
    ++metrics_.acknowledged;
    if (source_online_ && read_ == committed_ && !transaction_)
        state_ = OrdlogState::Online;
    return true;
}
const OrdlogRecord* Plaza2Ordlog::optional_front() const noexcept {
    return optional_read_ == optional_write_ ? nullptr : &optional_[optional_read_ % optional_.size()];
}
bool Plaza2Ordlog::optional_acknowledge() noexcept {
    if (!optional_front())
        return false;
    ++optional_read_;
    return true;
}
OrdlogMetrics Plaza2Ordlog::metrics() const noexcept {
    auto result = metrics_;
    result.queued = write_ - read_;
    result.optional_queued = optional_write_ - optional_read_;
    if (write_ != read_) {
        const auto received = queue_[read_ % queue_.size()].received_ns;
        result.oldest_age_ns = now_ns_ >= received ? now_ns_ - received : 0;
    }
    return result;
}
std::string_view Plaza2Ordlog::checkpoint() const noexcept {
    return !failed_ && !transaction_ && read_ == committed_ ? std::string_view(checkpoint_) : std::string_view{};
}
} // namespace moex::plaza2::cgate
