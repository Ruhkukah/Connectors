#pragma once

// Sequencing and recovery for one Spectra FAST market-data stream
// (FO-BOOK-1/5/20/50, FO-TRADES, SPOT-BOOK-*, SPOT-TRADES): templates 31/32.
//
// Feed it raw datagrams from the incremental A/B pair and, while
// needs_snapshot() is true, from the snapshot A/B pair. It arbitrates A/B by
// the preamble sequence number, rides out short reordering, and after a real
// loss rebuilds each instrument from the snapshot feed while queueing the
// incremental entries that arrived meanwhile.
//
// Production snapshot cycles last from half a minute (trades) to several
// minutes (deep books), and every snapshot in a cycle carries the cycle's
// starting LastMsgSeqNumProcessed. So the queue holds single entries of
// still-unsynced instruments only, instruments are released in batches as
// their snapshots arrive, and when memory runs short the oldest entries go
// first: the affected instruments simply wait for a later cycle.
//
// Idempotence is by MsgSeqNum: a snapshot covers everything up to its
// LastMsgSeqNumProcessed, and queued packets beyond it are replayed. RptSeq is
// used only to detect per-instrument gaps, and both "+1 per entry" and
// "repeated within one event" numbering are accepted.
//
// MarketStream<Sink> handles the aggregated streams; OrdersLogStream<Sink> is the
// same machinery over the order-log templates (29/30).
//
// Single-threaded. The steady-state path performs no allocation; memory grows
// only when an instrument is seen for the first time.

#include "fast_spectra_messages.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace moex::fast {

struct StreamConfig {
    std::uint32_t reorder_window{64};             // packets held while waiting for a missing one; rounded to 2^n
    std::uint64_t reorder_timeout_ns{2'000'000};  // how long to wait before declaring the packet lost
    std::size_t recovery_queue_bytes{std::size_t{64} << 20}; // entries of unsynced instruments
    std::size_t expected_instruments{2048};
};

struct StreamStats {
    std::uint64_t packets{}, duplicates{}, reordered{}, gaps{}, malformed{}, unknown_templates{};
    std::uint64_t instrument_gaps{}, unkeyed_entries{}, market_clears{}, sequence_resets{};
    std::uint64_t queued{}, queue_dropped{}, replayed{}, replay_passes{};
    std::uint64_t snapshot_packets{}, snapshot_resyncs{}, snapshots_applied{}, snapshots_too_old{}, empty_adopted{};
};

// Which templates a stream carries. The sequencing and recovery rules are the
// same for the aggregated streams and the order log; only the messages differ.
struct DefaultSchema { // FO-BOOK-*, FO-TRADES, SPOT-BOOK-*, SPOT-TRADES
    using Incremental = DefaultIncrementalRefreshMessage;
    using Snapshot = DefaultSnapshotMessage;
    // A snapshot message known to begin an instrument's snapshot.
    static bool starts_snapshot(const Snapshot& m) noexcept {
        return !m.LastFragment.present;
    }
};
struct OrdersLogSchema { // ORDERS-LOG: individual orders and trades
    using Incremental = OrdersLogMessage;
    using Snapshot = BookMessage;
    static bool starts_snapshot(const Snapshot& m) noexcept {
        return m.RouteFirst != 0;
    }
};

namespace detail {
// Fields that are mandatory in one schema and optional in the other.
inline bool present(std::uint32_t) noexcept {
    return true;
}
inline std::uint32_t value(std::uint32_t field) noexcept {
    return field;
}
inline bool present(const Opt<std::uint32_t>& field) noexcept {
    return field.present;
}
inline std::uint32_t value(const Opt<std::uint32_t>& field) noexcept {
    return field.value;
}
template <class Message> bool last_fragment(const Message& m) noexcept {
    return !present(m.LastFragment) || value(m.LastFragment) != 0;
}
} // namespace detail

// Default no-op sink; derive and shadow what you need. `slot` is a dense,
// stable per-stream instrument index, so a sink can keep its state in a vector.
template <class Schema> struct BasicStreamSink {
    using Incremental = typename Schema::Incremental;
    using Snapshot = typename Schema::Snapshot;

    void on_instrument(std::uint32_t /*slot*/, std::uint64_t /*security_id*/) noexcept {}
    // Discard everything known about the instrument; snapshot entries or fresh incrementals follow.
    void on_book_clear(std::uint32_t /*slot*/) noexcept {}
    void on_snapshot(std::uint32_t /*slot*/, const Snapshot&, const typename Snapshot::MDEntries&) noexcept {}
    void on_incremental(std::uint32_t /*slot*/, const Incremental&, const typename Incremental::MDEntries&) noexcept {}
    // An Empty Book entry that names no instrument, delivered once per instrument it
    // applies to: live for every synced instrument, later for each one as it resyncs.
    // With ExchangeTradingSessionID set it removes that session's orders, otherwise all.
    void on_market_clear(std::uint32_t /*slot*/, const Incremental&, const typename Incremental::MDEntries&) noexcept {}
    // false: the instrument's state is no longer trustworthy until true is reported again.
    void on_instrument_state(std::uint32_t /*slot*/, bool /*synced*/) noexcept {}
    // Packet loss: every instrument is untrusted until its own on_instrument_state(slot, true).
    void on_all_stale() noexcept {}
    // Daily sequence reset: clear every book; all instruments restart in sync.
    void on_sequence_reset() noexcept {}
    // End of an unfragmented message or of the last fragment: books are consistent.
    void on_batch_end() noexcept {}
    void on_session_status(const TradingSessionStatus&) noexcept {}
};
using StreamSink = BasicStreamSink<DefaultSchema>;
using OrdersLogSink = BasicStreamSink<OrdersLogSchema>;

template <class Sink, class Schema = DefaultSchema> class MarketStream {
    using Incremental = typename Schema::Incremental;
    using Snapshot = typename Schema::Snapshot;
    using Entry = typename Incremental::MDEntries;

  public:
    static constexpr std::size_t kMaxPacket = 2048;

    explicit MarketStream(Sink& sink, StreamConfig config = {})
        : sink_(sink), timeout_ns_(config.reorder_timeout_ns), queue_(config.recovery_queue_bytes) {
        window_ = 8;
        while (window_ < config.reorder_window)
            window_ <<= 1;
        slots_.resize(window_);
        slot_bytes_.resize(std::size_t{window_} * kMaxPacket);
        std::size_t buckets = 16;
        while (buckets < config.expected_instruments * 2)
            buckets <<= 1;
        table_.assign(buckets, 0);
        instruments_.reserve(config.expected_instruments);
    }

    void on_incremental(const std::uint8_t* data, std::size_t size, std::uint64_t now_ns) noexcept {
        if (size <= kPreambleSize) {
            ++stats_.malformed;
            return;
        }
        const std::uint32_t seq = read_preamble(data);
        if (!started_) {
            started_ = true;
            expected_ = epoch_from_ = seq;
        }
        if (heartbeat_seq_ == expected_ && seq == expected_ + 1)
            ++expected_; // the heartbeat consumed a sequence number
        if (seq == expected_) [[likely]] {
            behind_run_ = 0;
            deliver(data, size, seq);
            if (buffered_)
                drain(now_ns);
        } else if (seq < expected_) {
            ++stats_.duplicates;
            // A long run far behind means the gate restarted its numbering unannounced.
            if (expected_ - seq > window_ && ++behind_run_ > 2 * window_)
                restart_at(data, size, seq);
            return;
        } else if (seq - expected_ >= window_ || size > kMaxPacket) {
            restart_at(data, size, seq);
        } else {
            Slot& slot = slots_[seq & (window_ - 1)];
            if (slot.size != 0 && slot.seq == seq) {
                ++stats_.duplicates;
            } else {
                if (slot.size == 0 && buffered_++ == 0)
                    gap_since_ns_ = now_ns;
                slot = {seq, static_cast<std::uint32_t>(size)};
                std::memcpy(slot_data(seq), data, size);
                ++stats_.reordered;
            }
        }
        if (buffered_ && now_ns - gap_since_ns_ >= timeout_ns_)
            resolve_gap(now_ns);
    }

    void on_snapshot(const std::uint8_t* data, std::size_t size) noexcept {
        if (size <= kPreambleSize) {
            ++stats_.malformed;
            return;
        }
        if (!needs_snapshot()) {
            snapshot_expected_ = 0;
            return;
        }
        ++stats_.snapshot_packets;
        const std::uint32_t seq = read_preamble(data);
        // Sequence numbers restart every cycle, so they alone cannot tell the other
        // feed's copy of a packet from a different cycle's packet with the same number.
        std::uint64_t fingerprint = 0xCBF29CE484222325ull ^ size;
        for (std::size_t i = kPreambleSize, end = std::min<std::size_t>(size, 40); i < end; ++i)
            fingerprint = (fingerprint ^ data[i]) * 0x100000001B3ull;
        Seen& seen = snapshot_seen_[seq % kSnapshotSeen];
        if (seq != snapshot_expected_) {
            if (snapshot_expected_ != 0 && seen.seq == seq && seen.fingerprint == fingerprint)
                return; // the other feed's copy
            // Ahead of us: the missing packet may still come from the other feed.
            if (seq > snapshot_expected_ && snapshot_expected_ != 0 && ++snapshot_miss_run_ < kSnapshotMissRun)
                return;
            ++stats_.snapshot_resyncs;
            cycle_clean_ = false;
            fragment_active_ = false;
            fragment_resync_ = true;
        }
        snapshot_miss_run_ = 0;
        snapshot_expected_ = seq + 1;
        seen = {seq, fingerprint};
        Reader r{data + kPreambleSize, data + size};
        switch (read_template_id(r)) {
        case Snapshot::kTemplateId:
            process_snapshot(r);
            break;
        case SequenceReset::kTemplateId: {
            SequenceReset reset;
            if (decode(r, reset)) {
                end_cycle();
                snapshot_expected_ = reset.NewSeqNo ? reset.NewSeqNo : 1;
            }
            break;
        }
        case TradingSessionStatus::kTemplateId: {
            TradingSessionStatus status;
            if (decode(r, status))
                sink_.on_session_status(std::as_const(status));
            break;
        }
        default:
            break;
        }
    }

    // Call periodically when the incremental feed is quiet so a pending gap still times out.
    void on_timer(std::uint64_t now_ns) noexcept {
        if (buffered_ && now_ns - gap_since_ns_ >= timeout_ns_)
            resolve_gap(now_ns);
    }

    // True while the snapshot feed is needed; leave its multicast groups otherwise.
    [[nodiscard]] bool needs_snapshot() const noexcept {
        return started_ && (!covered_ || stale_ + pending_ > 0);
    }
    // Next incremental MsgSeqNum; every synced instrument reflects all packets before it.
    [[nodiscard]] std::uint32_t next_sequence() const noexcept {
        return expected_;
    }
    [[nodiscard]] std::size_t instruments() const noexcept {
        return instruments_.size();
    }
    [[nodiscard]] std::size_t unsynced() const noexcept {
        return stale_ + pending_;
    }
    [[nodiscard]] std::uint64_t security_id(std::uint32_t slot) const noexcept {
        return instruments_[slot].id;
    }
    // Last incremental MsgSeqNum reflected in a synced instrument: normally the
    // packet before next_sequence(), later when its snapshot ran ahead of the feed.
    [[nodiscard]] std::uint32_t applied_through(std::uint32_t slot) const noexcept {
        return std::max(expected_ - 1, instruments_[slot].applied_seq);
    }
    [[nodiscard]] bool synced(std::uint32_t slot) const noexcept {
        return instruments_[slot].state <= State::Adopt;
    }
    [[nodiscard]] const StreamStats& stats() const noexcept {
        return stats_;
    }

  private:
    // Synced: applying live. Adopt: known empty, RptSeq taken from the next entry.
    // Stale: waiting for a snapshot covering from_seq. Pending*: snapshot taken, queue replay due.
    enum class State : std::uint8_t { Synced, Adopt, Stale, Pending, PendingEmpty };

    struct Instrument {
        std::uint64_t id{};
        std::uint32_t rpt_seq{};
        std::uint32_t applied_seq{}; // replay applies only packets beyond this MsgSeqNum
        std::uint32_t from_seq{};    // first MsgSeqNum we hold for it while stale
        std::uint32_t seen_cycle{};
        std::uint32_t replay_id{};
        State state{State::Stale};
    };
    struct Slot {
        std::uint32_t seq{};
        std::uint32_t size{};
    };
    struct Record { // followed by the entry's wire bytes
        std::uint32_t seq;
        std::uint32_t slot;
        std::uint64_t sending_time;
        std::uint32_t size;
    };
    struct Seen {
        std::uint32_t seq{};
        std::uint64_t fingerprint{};
    };
    static constexpr std::uint32_t kSnapshotMissRun = 8;
    static constexpr std::uint32_t kSnapshotSeen = 64;
    static constexpr std::size_t kReplayBatch = 64;
    static constexpr std::uint32_t kAllSlots = ~std::uint32_t{0};

    std::uint8_t* slot_data(std::uint32_t seq) noexcept {
        return slot_bytes_.data() + std::size_t{seq & (window_ - 1)} * kMaxPacket;
    }

    void deliver(const std::uint8_t* data, std::size_t size, std::uint32_t seq) noexcept {
        ++stats_.packets;
        expected_ = seq + 1;
        process(data, size, seq);
    }

    void drain(std::uint64_t now_ns) noexcept {
        while (buffered_) {
            if (heartbeat_seq_ == expected_ && slots_[(expected_ + 1) & (window_ - 1)].seq == expected_ + 1 &&
                slots_[(expected_ + 1) & (window_ - 1)].size)
                ++expected_;
            Slot& slot = slots_[expected_ & (window_ - 1)];
            if (slot.size == 0 || slot.seq != expected_)
                break;
            const std::uint32_t size = slot.size;
            slot.size = 0;
            --buffered_;
            deliver(slot_data(slot.seq), size, slot.seq);
        }
        gap_since_ns_ = now_ns;
    }

    void clear_buffer() noexcept {
        for (Slot& slot : slots_)
            slot.size = 0;
        buffered_ = 0;
    }

    // The missing packet is not coming: skip to the oldest one we hold.
    void resolve_gap(std::uint64_t now_ns) noexcept {
        std::uint32_t next = expected_ + 1;
        while (slots_[next & (window_ - 1)].size == 0 || slots_[next & (window_ - 1)].seq != next) {
            if (++next - expected_ > window_) { // nothing usable is held after all
                clear_buffer();
                return;
            }
        }
        ++stats_.gaps;
        mark_all_stale(next);
        expected_ = next;
        drain(now_ns);
    }

    void restart_at(const std::uint8_t* data, std::size_t size, std::uint32_t seq) noexcept {
        ++stats_.gaps;
        behind_run_ = 0;
        clear_buffer();
        mark_all_stale(seq);
        deliver(data, size, seq);
    }

    void process(const std::uint8_t* data, std::size_t size, std::uint32_t seq) noexcept {
        Reader r{data + kPreambleSize, data + size};
        switch (read_template_id(r)) {
        case Incremental::kTemplateId:
            process_incremental(r, seq);
            return;
        case Heartbeat::kTemplateId:
            // Whether a heartbeat consumes a sequence number is not specified;
            // hold position and let the next data packet decide.
            heartbeat_seq_ = seq;
            expected_ = seq;
            return;
        case SequenceReset::kTemplateId: {
            SequenceReset reset;
            if (decode(r, reset))
                sequence_reset(reset.NewSeqNo);
            else
                ++stats_.malformed;
            return;
        }
        case TradingSessionStatus::kTemplateId: {
            TradingSessionStatus status;
            if (decode(r, status))
                sink_.on_session_status(std::as_const(status));
            else
                ++stats_.malformed;
            return;
        }
        default:
            ++stats_.unknown_templates;
        }
    }

    void process_incremental(Reader& r, std::uint32_t seq) noexcept {
        Incremental message;
        const bool ok = decode(r, message, [&](const auto& m, const auto& e) {
            if (!e.SecurityID.present) {
                if (e.MDEntryType.front() == 'J')
                    market_clear(seq, m, e, static_cast<std::size_t>(r.p - r.mark), r.mark);
                else
                    ++stats_.unkeyed_entries;
                return;
            }
            const std::uint32_t slot = find_or_add(e.SecurityID.value);
            const auto size = static_cast<std::size_t>(r.p - r.mark);
            // Making room can release instruments, so it happens before their state is looked at.
            if (queue_end_ + sizeof(Record) + size > queue_.size() && !make_room(sizeof(Record) + size)) {
                mark_all_stale(seq + 1); // the queue cannot hold even one entry
                return;
            }
            // Empty Book states the whole book, so an unsynced instrument needs no snapshot to resume.
            if (instruments_[slot].state >= State::Stale && e.MDEntryType.front() == 'J')
                resume_empty(slot, seq, detail::value(e.RptSeq));
            // Unsynced, or just gone stale on this entry: keep it for after the snapshot.
            if (instruments_[slot].state >= State::Stale || !apply(slot, seq, m, e))
                enqueue(seq, slot, m.SendingTime, r.mark, size);
        });
        if (!ok) {
            ++stats_.malformed;
            mark_all_stale(seq + 1);
            return;
        }
        if (detail::last_fragment(message))
            sink_.on_batch_end();
    }

    // Empty Book for the whole market or one trading session. Synced instruments take it
    // now; the rest take it in order when their queued entries are replayed.
    void market_clear(std::uint32_t seq, const Incremental& m, const Entry& e, std::size_t size,
                      const std::uint8_t* data) noexcept {
        ++stats_.market_clears;
        for (std::uint32_t slot = 0; slot < instruments_.size(); ++slot)
            if (instruments_[slot].state < State::Stale && seq > instruments_[slot].applied_seq)
                sink_.on_market_clear(slot, m, e);
        if (stale_ + pending_ == 0)
            return;
        if (queue_end_ + sizeof(Record) + size > queue_.size() && !make_room(sizeof(Record) + size)) {
            mark_all_stale(seq + 1);
            return;
        }
        enqueue(seq, kAllSlots, m.SendingTime, data, size);
    }

    void resume_empty(std::uint32_t slot, std::uint32_t seq, std::uint32_t rpt_seq) noexcept {
        Instrument& inst = instruments_[slot];
        --(inst.state == State::Stale ? stale_ : pending_);
        inst.state = State::Synced;
        inst.rpt_seq = rpt_seq;
        inst.applied_seq = seq - 1; // its queued entries all predate this packet and are discarded by the next pass
        inst.replay_id = 0;
        ++stats_.empty_adopted;
        sink_.on_book_clear(slot);
        sink_.on_instrument_state(slot, true);
    }

    // Applies one entry to a synced instrument. Returns false if its RptSeq shows a gap.
    bool apply(std::uint32_t slot, std::uint32_t seq, const Incremental& m, const Entry& e) noexcept {
        Instrument& inst = instruments_[slot];
        // A snapshot can be ahead of our position in the incremental feed.
        if (seq <= inst.applied_seq)
            return true;
        const std::uint32_t rpt_seq = detail::present(e.RptSeq) ? detail::value(e.RptSeq) : inst.rpt_seq;
        if (inst.state == State::Adopt) {
            inst.state = State::Synced;
        } else if (rpt_seq - inst.rpt_seq > 1) {
            ++stats_.instrument_gaps;
            if (!needs_snapshot())
                snapshot_expected_ = 0; // rejoin the snapshot feed mid-cycle
            inst.state = State::Stale;
            inst.from_seq = replaying_ ? expected_ : seq;
            ++stale_;
            sink_.on_instrument_state(slot, false);
            return false;
        }
        inst.rpt_seq = rpt_seq;
        sink_.on_incremental(slot, m, e);
        return true;
    }

    void process_snapshot(Reader& r) noexcept {
        Snapshot message;
        bool begun = false, skip = true, use = false;
        std::uint32_t slot = 0;
        const auto begin = [&](const Snapshot& m) {
            begun = true;
            if (fragment_resync_) {
                // After a discontinuity wait for a message known to begin an instrument's snapshot.
                if (!Schema::starts_snapshot(m))
                    return;
                fragment_resync_ = false;
            }
            if (!m.SecurityID.present)
                return;
            skip = false;
            slot = find_or_add(m.SecurityID.value);
            Instrument& inst = instruments_[slot];
            inst.seen_cycle = cycle_id_;
            cycle_any_ = true;
            cycle_min_processed_ = std::min(cycle_min_processed_, m.LastMsgSeqNumProcessed);
            if (fragment_active_ && fragment_slot_ == slot) {
                use = fragment_use_;
                return;
            }
            // Later snapshots never reach further back, so queued entries this one already covers are dead weight.
            if (pending_ == 0)
                trim_queue(m.LastMsgSeqNumProcessed);
            if (inst.state != State::Stale)
                return;
            use = std::uint64_t{m.LastMsgSeqNumProcessed} + 1 >= std::max(inst.from_seq, queue_floor_);
            if (use)
                sink_.on_book_clear(slot);
            else
                ++stats_.snapshots_too_old;
        };
        const bool ok = decode(r, message, [&](const auto& m, const auto& e) {
            if (!begun)
                begin(m);
            if (use)
                sink_.on_snapshot(slot, m, e);
        });
        if (!ok) {
            ++stats_.malformed;
            cycle_clean_ = false;
            fragment_active_ = false;
            fragment_resync_ = true;
            return;
        }
        if (!begun)
            begin(message);
        const bool last = detail::last_fragment(message);
        if (skip) {
            if (last)
                fragment_resync_ = false; // the next message starts another instrument
            return;
        }
        fragment_active_ = !last;
        fragment_slot_ = slot;
        fragment_use_ = use;
        if (last && use) {
            Instrument& inst = instruments_[slot];
            inst.rpt_seq = detail::value(message.RptSeq);
            inst.applied_seq = message.LastMsgSeqNumProcessed;
            inst.state = State::Pending;
            --stale_;
            ++pending_;
            ++stats_.snapshots_applied;
            if (pending_ >= kReplayBatch)
                replay(); // release instruments as the cycle goes rather than all at its end
        }
    }

    // SequenceReset on the snapshot feed closes a cycle.
    void end_cycle() noexcept {
        if (cycle_clean_ && cycle_any_) {
            // A complete cycle omits only instruments with nothing to report, so
            // those are empty as of the cycle's oldest LastMsgSeqNumProcessed.
            const std::uint64_t held_from = std::uint64_t{cycle_min_processed_} + 1;
            for (std::uint32_t slot = 0; stale_ && slot < instruments_.size(); ++slot) {
                Instrument& inst = instruments_[slot];
                if (inst.state != State::Stale || inst.seen_cycle == cycle_id_ || inst.from_seq > held_from)
                    continue;
                sink_.on_book_clear(slot);
                inst.applied_seq = cycle_min_processed_;
                inst.state = State::PendingEmpty;
                --stale_;
                ++pending_;
                ++stats_.empty_adopted;
            }
            if (epoch_from_ <= held_from)
                covered_ = true;
        }
        if (pending_)
            replay();
        ++cycle_id_;
        cycle_clean_ = true;
        cycle_any_ = false;
        cycle_min_processed_ = UINT32_MAX;
        fragment_active_ = false;
        fragment_resync_ = false;
    }

    // Releases every instrument whose snapshot has arrived: applies its queued
    // entries beyond the snapshot and drops them, keeping those of instruments still waiting.
    void replay() noexcept {
        ++replay_id_;
        ++stats_.replay_passes;
        for (std::uint32_t slot = 0; slot < instruments_.size(); ++slot) {
            Instrument& inst = instruments_[slot];
            if (inst.state < State::Pending)
                continue;
            inst.state = inst.state == State::Pending ? State::Synced : State::Adopt;
            inst.replay_id = replay_id_;
            sink_.on_instrument_state(slot, true);
        }
        pending_ = 0;
        replaying_ = true;
        replay_aborted_ = false;
        const std::size_t end = queue_end_;
        std::size_t read = queue_head_, write = 0;
        Incremental message{};
        message.NoMDEntries = 1;
        bool applied = false;
        while (read < end && !replay_aborted_) {
            Record record;
            std::memcpy(&record, queue_.data() + read, sizeof(record));
            const std::size_t length = sizeof(record) + record.size;
            if (record.slot == kAllSlots) {
                Reader r{queue_.data() + read + sizeof(record), queue_.data() + read + length};
                Entry entry;
                if (decode(r, entry)) {
                    message.MsgSeqNum = record.seq;
                    message.SendingTime = record.sending_time;
                    for (std::uint32_t slot = 0; slot < instruments_.size(); ++slot) {
                        const Instrument& inst = instruments_[slot];
                        if (inst.state < State::Stale && inst.replay_id == replay_id_ && record.seq > inst.applied_seq) {
                            sink_.on_market_clear(slot, message, entry);
                            applied = true;
                        }
                    }
                }
                if (stale_) { // instruments still waiting will need it too
                    std::memmove(queue_.data() + write, queue_.data() + read, length);
                    write += length;
                }
                read += length;
                continue;
            }
            const Instrument& inst = instruments_[record.slot];
            if (inst.state >= State::Stale) {
                std::memmove(queue_.data() + write, queue_.data() + read, length);
                write += length;
            } else if (inst.replay_id == replay_id_) {
                Reader r{queue_.data() + read + sizeof(record), queue_.data() + read + length};
                Entry entry;
                if (decode(r, entry)) {
                    message.MsgSeqNum = record.seq;
                    message.SendingTime = record.sending_time;
                    apply(record.slot, record.seq, message, entry);
                    applied = true;
                    ++stats_.replayed;
                }
            }
            read += length;
        }
        replaying_ = false;
        if (!replay_aborted_) {
            queue_head_ = 0;
            queue_end_ = write;
        }
        if (applied)
            sink_.on_batch_end();
    }

    void enqueue(std::uint32_t seq, std::uint32_t slot, std::uint64_t sending_time, const std::uint8_t* data,
                 std::size_t size) noexcept {
        const Record record{seq, slot, sending_time, static_cast<std::uint32_t>(size)};
        std::memcpy(queue_.data() + queue_end_, &record, sizeof(record));
        std::memcpy(queue_.data() + queue_end_ + sizeof(record), data, size);
        queue_end_ += sizeof(record) + size;
        ++stats_.queued;
    }

    bool make_room(std::size_t length) noexcept {
        if (pending_)
            replay();
        // Still short: give up the oldest half. Instruments that needed it wait for a later cycle.
        std::size_t head = queue_head_;
        while (head < queue_end_ && (queue_end_ - head > queue_.size() / 2 || queue_end_ - head + length > queue_.size())) {
            Record record;
            std::memcpy(&record, queue_.data() + head, sizeof(record));
            head += sizeof(record) + record.size;
            queue_floor_ = record.seq + 1;
            ++stats_.queue_dropped;
        }
        std::memmove(queue_.data(), queue_.data() + head, queue_end_ - head);
        queue_end_ -= head;
        queue_head_ = 0;
        return queue_end_ + length <= queue_.size();
    }

    // Drops queued entries from packets up to and including seq.
    void trim_queue(std::uint32_t seq) noexcept {
        while (queue_head_ < queue_end_) {
            Record record;
            std::memcpy(&record, queue_.data() + queue_head_, sizeof(record));
            if (record.seq > seq)
                break;
            queue_head_ += sizeof(record) + record.size;
        }
        if (queue_head_ == queue_end_)
            queue_head_ = queue_end_ = 0;
    }

    // Nothing before from_seq can be trusted: every instrument needs a snapshot that reaches it.
    void mark_all_stale(std::uint32_t from_seq) noexcept {
        for (Instrument& inst : instruments_) {
            inst.state = State::Stale;
            inst.from_seq = from_seq;
        }
        stale_ = instruments_.size();
        pending_ = 0;
        queue_head_ = queue_end_ = 0;
        queue_floor_ = 0;
        replay_aborted_ = true;
        epoch_from_ = from_seq;
        covered_ = false;
        fragment_active_ = false;
        snapshot_expected_ = 0; // whatever cycle is in flight predates this loss
        sink_.on_all_stale();
    }

    void sequence_reset(std::uint32_t new_seq) noexcept {
        ++stats_.sequence_resets;
        clear_buffer();
        expected_ = epoch_from_ = new_seq;
        heartbeat_seq_ = kNoHeartbeat;
        for (Instrument& inst : instruments_) {
            inst.state = State::Synced;
            inst.rpt_seq = inst.applied_seq = 0;
        }
        stale_ = pending_ = 0;
        queue_head_ = queue_end_ = 0;
        queue_floor_ = 0;
        covered_ = true; // numbering restarts, so everything from here on is observed
        fragment_active_ = false;
        sink_.on_sequence_reset();
    }

    std::uint32_t find_or_add(std::uint64_t id) noexcept {
        if (last_slot_ < instruments_.size() && instruments_[last_slot_].id == id) [[likely]]
            return last_slot_;
        const std::size_t mask = table_.size() - 1;
        std::size_t bucket = static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> 32) & mask;
        while (const std::uint32_t entry = table_[bucket]) {
            if (instruments_[entry - 1].id == id)
                return last_slot_ = entry - 1;
            bucket = (bucket + 1) & mask;
        }
        const auto slot = static_cast<std::uint32_t>(instruments_.size());
        Instrument inst;
        inst.id = id;
        // Once a complete snapshot cycle has been reconciled, an instrument we have
        // never heard of had nothing to report, so its first entry starts its book.
        if (covered_) {
            inst.state = State::Adopt;
        } else {
            inst.from_seq = epoch_from_;
            ++stale_;
        }
        instruments_.push_back(inst);
        table_[bucket] = slot + 1;
        if (instruments_.size() * 2 > table_.size())
            rehash();
        sink_.on_instrument(slot, id);
        return last_slot_ = slot;
    }

    void rehash() noexcept {
        table_.assign(table_.size() * 2, 0);
        const std::size_t mask = table_.size() - 1;
        for (std::uint32_t slot = 0; slot < instruments_.size(); ++slot) {
            std::size_t bucket =
                static_cast<std::size_t>((instruments_[slot].id * 0x9E3779B97F4A7C15ull) >> 32) & mask;
            while (table_[bucket])
                bucket = (bucket + 1) & mask;
            table_[bucket] = slot + 1;
        }
    }

    static constexpr std::uint64_t kNoHeartbeat = ~std::uint64_t{0};

    Sink& sink_;
    StreamStats stats_;

    // Incremental arbitration.
    std::uint32_t expected_{};
    std::uint32_t window_{};
    std::uint32_t buffered_{};
    std::uint32_t behind_run_{};
    std::uint64_t heartbeat_seq_{kNoHeartbeat};
    std::uint64_t gap_since_ns_{};
    std::uint64_t timeout_ns_{};
    bool started_{};
    std::vector<Slot> slots_;
    std::vector<std::uint8_t> slot_bytes_;

    // Instruments.
    std::vector<Instrument> instruments_;
    std::vector<std::uint32_t> table_;
    std::uint32_t last_slot_{};
    std::size_t stale_{};
    std::size_t pending_{};
    std::uint32_t epoch_from_{}; // first MsgSeqNum held since the last loss
    bool covered_{};             // a complete snapshot cycle has been reconciled since then

    // Recovery queue: Record-prefixed entries in MsgSeqNum order, live in [queue_head_, queue_end_).
    std::vector<std::uint8_t> queue_;
    std::size_t queue_head_{};
    std::size_t queue_end_{};
    std::uint32_t queue_floor_{}; // entries of packets before this were dropped under memory pressure
    std::uint32_t replay_id_{};
    bool replaying_{};
    bool replay_aborted_{};

    // Snapshot feed.
    std::uint32_t snapshot_expected_{};
    std::uint32_t snapshot_miss_run_{};
    Seen snapshot_seen_[kSnapshotSeen]{}; // recently processed snapshot packets
    std::uint32_t cycle_id_{1};
    std::uint32_t cycle_min_processed_{UINT32_MAX};
    bool cycle_clean_{};
    bool cycle_any_{};
    bool fragment_active_{};
    bool fragment_resync_{true};
    bool fragment_use_{};
    std::uint32_t fragment_slot_{};
};

template <class Sink> using OrdersLogStream = MarketStream<Sink, OrdersLogSchema>;

} // namespace moex::fast
