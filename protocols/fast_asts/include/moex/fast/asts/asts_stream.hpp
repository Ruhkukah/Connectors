#pragma once

// Sequencing and recovery for one MOEX ASTS FAST stream pair: an incremental
// feed (OLR orders, TLR trades, MSR statistics) and its snapshot feed (OLS,
// TLS, MSS). Feed it the raw datagrams of the incremental A/B feeds and, while
// needs_snapshot() is true, of the snapshot A/B feeds.
//
// How this differs from the Spectra stream, following the ASTS user guide and
// what the production captures show:
//  - An instrument is a Symbol on a trading board (TradingSessionID).
//  - RptSeq rises by exactly one per entry and per instrument, so it alone
//    decides what to apply: the next number is applied, lower ones are already
//    reflected, a jump means entries were missed.
//  - A snapshot states an instrument as of its RptSeq. It is usable when the
//    entries after it are all held (the first one queued is no later than
//    RptSeq + 1), or, for an instrument with nothing queued, when its cycle
//    began after the instrument fell out of sync.
//  - Packet loss makes every instrument suspect but does not discard what is
//    held: an instrument whose next entry carries the next RptSeq missed
//    nothing and resumes at once; a quiet one is confirmed by a snapshot from a
//    later cycle showing the same RptSeq. Only instruments that really lost
//    entries are rebuilt.
//  - With StreamConfig::tcp_replay the sink is told which packets were lost
//    (on_loss) so the caller can fetch them over TCP replay and hand them back
//    through on_recovered / replay_finished. Once the range is back, all
//    instruments are whole again without waiting for snapshots. Replay repairs
//    one outage at a time: if the request fails, or another loss arrives
//    before it completes, the instruments involved fall back to snapshots.
//  - Snapshot cycles restart at sequence number 1, fragments of one instrument
//    run from RouteFirst to LastFragment, and empty instruments are still sent
//    (as an Empty Book entry), so a complete cycle names every instrument.
//
// Single-threaded. The steady-state path performs no allocation; memory grows
// only when an instrument is seen for the first time.

#include "asts_arbiter.hpp"
#include "asts_messages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace moex::fast::asts {

struct StreamConfig {
    std::uint32_t reorder_window{64};            // packets held while waiting for a missing one; rounded to 2^n
    std::uint64_t reorder_timeout_ns{2'000'000}; // how long to wait before declaring the packet lost
    std::size_t recovery_queue_bytes{std::size_t{32} << 20};
    std::uint64_t replay_interval_ns{5'000'000}; // on_timer releases restored instruments at most this often
    bool tcp_replay{}; // report lost ranges through Sink::on_loss and accept them back
    std::size_t expected_instruments{4096};
};

struct StreamStats {
    std::uint64_t packets{}, malformed{}, unknown_templates{}, unkeyed_entries{};
    std::uint64_t instrument_gaps{}, losses{}, restarts{};
    std::uint64_t queued{}, queue_dropped{}, replayed{}, replay_passes{};
    std::uint64_t snapshot_packets{}, snapshot_cycles{}, snapshot_resyncs{}, snapshots_applied{}, snapshots_too_old{};
    std::uint64_t resumed_without_snapshot{}, confirmed_by_snapshot{};
    std::uint64_t recovered_packets{}, confirmed_by_replay{}, replay_ranges_failed{};
};

// Default no-op sink; derive and shadow what you need. `slot` is a dense,
// stable per-stream instrument index, so a sink can keep its state in a vector.
template <class Incremental, class Snapshot> struct StreamSink {
    using IncrementalEntry = typename Incremental::GroupMDEntries;
    using SnapshotEntry = typename Snapshot::GroupMDEntries;

    void on_instrument(std::uint32_t /*slot*/, std::string_view /*symbol*/, std::string_view /*board*/) noexcept {}
    // Discard everything known about the instrument; snapshot entries or fresh incrementals follow.
    void on_clear(std::uint32_t /*slot*/) noexcept {}
    void on_snapshot(std::uint32_t /*slot*/, const Snapshot&, const SnapshotEntry&) noexcept {}
    void on_incremental(std::uint32_t /*slot*/, const Incremental&, const IncrementalEntry&) noexcept {}
    // false: the instrument's state is no longer trustworthy until true is reported again.
    // true without a preceding on_clear means what the sink already holds was right all along.
    void on_instrument_state(std::uint32_t /*slot*/, bool /*synced*/) noexcept {}
    // Packet loss: every instrument is untrusted until its own on_instrument_state(slot, true).
    // Keep the data: most instruments are confirmed rather than rebuilt.
    void on_all_stale() noexcept {}
    // With StreamConfig::tcp_replay: packets first_seq..last_seq were lost on both feeds.
    // Fetch them (TCP replay), pass each to Stream::on_recovered, then call
    // Stream::replay_finished; call it with ok = false if the range will not be fetched.
    // A newer on_loss supersedes an unfinished one: finishing the older range is then a no-op.
    // While a range is being recovered, on_incremental can arrive for an instrument
    // still reported as not in sync; its on_instrument_state(slot, true) follows.
    void on_loss(std::uint32_t /*first_seq*/, std::uint32_t /*last_seq*/) noexcept {}
    // End of an incremental message: everything it changed is now consistent.
    void on_batch_end() noexcept {}
    void on_session_status(const TradingSessionStatus&) noexcept {}
};

template <class Incremental, class Snapshot, class Sink> class Stream {
  public:
    explicit Stream(Sink& sink, StreamConfig config = {})
        : sink_(sink), arbiter_(config.reorder_window, config.reorder_timeout_ns),
          replay_interval_ns_(config.replay_interval_ns), tcp_replay_(config.tcp_replay),
          queue_(config.recovery_queue_bytes) {
        std::size_t buckets = 16;
        while (buckets < config.expected_instruments * 2)
            buckets <<= 1;
        table_.assign(buckets, 0);
        instruments_.reserve(config.expected_instruments);
        pending_slots_.reserve(kReplayBatch);
    }

    void on_incremental(const std::uint8_t* data, std::size_t size, std::uint64_t now_ns) noexcept {
        if (size <= kPreambleSize) {
            ++stats_.malformed;
            return;
        }
        arbiter_.push(data, size, now_ns, deliver_, lost_);
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
        // feed's copy of a packet from another cycle's packet with the same number.
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
            fragment_active_ = false;
            if (seq != 1) { // a cycle normally ends by wrapping to 1; anything else lost packets
                ++stats_.snapshot_resyncs;
                cycle_clean_ = false;
            }
        }
        snapshot_miss_run_ = 0;
        snapshot_expected_ = seq + 1;
        seen = {seq, fingerprint};
        if (seq == 1)
            begin_cycle();
        Reader r{data + kPreambleSize, data + size};
        std::uint64_t pmap;
        switch (read_header(r, pmap)) {
        case Snapshot::kTemplateId:
            process_snapshot(r, pmap);
            break;
        case TradingSessionStatus::kTemplateId: {
            TradingSessionStatus status;
            if (decode(r, pmap, status))
                sink_.on_session_status(std::as_const(status));
            break;
        }
        default:
            break;
        }
    }

    // One packet of a lost range, fetched by TCP replay: the FAST message as the gate
    // framed it, without the 4-byte sequence preamble. In sequence order.
    void on_recovered(const std::uint8_t* message, std::size_t size) noexcept {
        ++stats_.recovered_packets;
        recovering_ = true;
        process_message(message, message + size);
        recovering_ = false;
    }

    // The range reported by the latest on_loss has been handed back in full (ok), or will not be.
    void replay_finished(std::uint32_t first_seq, std::uint32_t last_seq, bool ok) noexcept {
        if (!open_loss_ || open_first_ != first_seq || open_last_ != last_seq)
            return; // superseded by a later loss
        open_loss_ = false;
        if (!ok) {
            ++stats_.replay_ranges_failed;
            return; // snapshots take it from here
        }
        // Nothing has been lost since, so the entries held since the loss follow on directly,
        // and an instrument with nothing held and nothing recovered was never touched.
        replay();
        for (std::uint32_t slot = 0; slot < instruments_.size(); ++slot) {
            Instrument& inst = instruments_[slot];
            if (inst.state != State::Stale || !inst.intact || inst.barred || inst.first_rpt != 0)
                continue;
            inst.state = State::Synced;
            --stale_;
            ++stats_.confirmed_by_replay;
            sink_.on_instrument_state(slot, true);
        }
    }

    // True while the range reported by the latest on_loss is still outstanding.
    [[nodiscard]] bool recovery_pending() const noexcept {
        return open_loss_;
    }

    // Call periodically so a held-back packet is released and restored instruments are not kept waiting.
    void on_timer(std::uint64_t now_ns) noexcept {
        arbiter_.on_timer(now_ns, deliver_, lost_);
        if (pending_ && now_ns - last_replay_ns_ >= replay_interval_ns_) {
            last_replay_ns_ = now_ns;
            replay();
        }
    }

    // True while the snapshot feed is needed; leave its multicast groups otherwise.
    [[nodiscard]] bool needs_snapshot() const noexcept {
        return arbiter_.started() && (!covered_ || stale_ + pending_ > 0);
    }
    [[nodiscard]] std::uint32_t next_sequence() const noexcept {
        return arbiter_.next();
    }
    [[nodiscard]] std::size_t instruments() const noexcept {
        return instruments_.size();
    }
    [[nodiscard]] std::size_t unsynced() const noexcept {
        return stale_ + pending_;
    }
    [[nodiscard]] bool synced(std::uint32_t slot) const noexcept {
        return instruments_[slot].state == State::Synced;
    }
    // RptSeq of the last entry reflected in a synced instrument.
    [[nodiscard]] std::uint32_t rpt_seq(std::uint32_t slot) const noexcept {
        return instruments_[slot].rpt_seq;
    }
    [[nodiscard]] std::string_view symbol(std::uint32_t slot) const noexcept {
        const char* text = instruments_[slot].key.data();
        return {text, ::strnlen(text, kSymbolSize)};
    }
    [[nodiscard]] std::string_view board(std::uint32_t slot) const noexcept {
        const char* text = instruments_[slot].key.data() + kSymbolSize;
        return {text, ::strnlen(text, kBoardSize)};
    }
    // Slot of a known instrument, or UINT32_MAX.
    [[nodiscard]] std::uint32_t find(std::string_view symbol, std::string_view board) const noexcept {
        Key key{};
        if (symbol.size() > kSymbolSize || board.size() > kBoardSize)
            return UINT32_MAX;
        std::memcpy(key.data(), symbol.data(), symbol.size());
        std::memcpy(key.data() + kSymbolSize, board.data(), board.size());
        const std::size_t mask = table_.size() - 1;
        for (std::size_t bucket = hash(key) & mask; table_[bucket]; bucket = (bucket + 1) & mask)
            if (instruments_[table_[bucket] - 1].key == key)
                return table_[bucket] - 1;
        return UINT32_MAX;
    }
    [[nodiscard]] const StreamStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] const SequenceArbiter& arbiter() const noexcept {
        return arbiter_;
    }

  private:
    static constexpr std::size_t kSymbolSize = 16;
    static constexpr std::size_t kBoardSize = 8;
    using Key = std::array<char, kSymbolSize + kBoardSize>; // zero-padded symbol, then zero-padded board

    // Stale: waiting for a usable snapshot. Pending: snapshot taken, queued entries still to be replayed.
    enum class State : std::uint8_t { Synced, Stale, Pending };

    struct Instrument {
        Key key{};
        std::uint32_t rpt_seq{};
        std::uint32_t first_rpt{};   // RptSeq of the first entry seen since it went stale; 0 if none
        std::uint32_t stale_cycle{}; // snapshot cycle during which it went stale
        State state{State::Stale};
        bool intact{}; // stale, but what the sink holds is exactly the state as of rpt_seq
        bool barred{}; // carries a loss that replay will not make good; only an entry or a snapshot clears it
    };
    struct Seen {
        std::uint32_t seq{};
        std::uint64_t fingerprint{};
    };
    struct Record { // followed by the packet
        std::uint32_t seq;
        std::uint32_t size;
    };
    static constexpr std::uint32_t kSnapshotMissRun = 8;
    static constexpr std::uint32_t kSnapshotSeen = 64;
    static constexpr std::size_t kReplayBatch = 64;

    void deliver(const std::uint8_t* data, std::size_t size, std::uint32_t seq) noexcept {
        ++stats_.packets;
        if (process(data, size))
            enqueue(data, size, seq);
    }

    bool process(const std::uint8_t* data, std::size_t size) noexcept {
        return process_message(data + kPreambleSize, data + size);
    }

    // Returns true when the message touches an unsynced instrument and must be kept for replay.
    bool process_message(const std::uint8_t* begin, const std::uint8_t* end) noexcept {
        Reader r{begin, end};
        std::uint64_t pmap;
        switch (read_header(r, pmap)) {
        case Incremental::kTemplateId:
            return process_incremental(r, pmap);
        case Heartbeat::kTemplateId:
            return false;
        case TradingSessionStatus::kTemplateId: {
            TradingSessionStatus status;
            if (decode(r, pmap, status))
                sink_.on_session_status(std::as_const(status));
            else
                ++stats_.malformed;
            return false;
        }
        default:
            ++stats_.unknown_templates;
            return false;
        }
    }

    bool process_incremental(Reader& r, std::uint64_t pmap) noexcept {
        Incremental message;
        bool keep = false;
        const bool ok = decode(r, pmap, message, [&](const auto& m, const auto& e) {
            if (!e.Symbol.present || !e.RptSeq.present) {
                ++stats_.unkeyed_entries;
                return;
            }
            const std::uint32_t slot = find_or_add(e.Symbol.value, optional_text(e.TradingSessionID));
            if (slot == UINT32_MAX)
                return;
            Instrument& inst = instruments_[slot];
            const auto rpt_seq = static_cast<std::uint32_t>(e.RptSeq.value);
            if (inst.state != State::Synced) {
                if (inst.state == State::Stale && inst.intact && rpt_seq <= inst.rpt_seq + 1) {
                    if (rpt_seq <= inst.rpt_seq)
                        return; // already reflected
                    if (recovering_) {
                        // A recovered entry extends what is held, but later entries may already be
                        // queued behind it, so the instrument stays out of sync until they are run.
                        inst.rpt_seq = rpt_seq;
                        sink_.on_incremental(slot, m, e);
                        return;
                    }
                    // The very next entry: nothing was missed for this instrument.
                } else if (recovering_) {
                    // Cannot be placed: the instrument was already missing something older. It is
                    // not kept, so replay cannot vouch for the instrument, which waits for a snapshot.
                    inst.barred = true;
                    return;
                } else if (rpt_seq == 1 || entry_type(e) == 'J') {
                    // The day's first entry and Empty Book both state the instrument completely.
                    inst.rpt_seq = rpt_seq - 1;
                    sink_.on_clear(slot);
                } else {
                    if (inst.first_rpt == 0)
                        inst.first_rpt = rpt_seq;
                    keep = true;
                    return;
                }
                --(inst.state == State::Stale ? stale_ : pending_);
                inst.state = State::Synced;
                ++stats_.resumed_without_snapshot;
                sink_.on_instrument_state(slot, true);
            }
            if (rpt_seq == inst.rpt_seq + 1) [[likely]] {
                inst.rpt_seq = rpt_seq;
                sink_.on_incremental(slot, m, e);
            } else if (rpt_seq > inst.rpt_seq) {
                ++stats_.instrument_gaps;
                inst.state = State::Stale;
                inst.intact = false;
                inst.barred = false;
                inst.first_rpt = rpt_seq;
                inst.stale_cycle = cycle_id_;
                if (!needs_snapshot())
                    snapshot_expected_ = 0; // rejoin the snapshot feed mid-cycle
                ++stale_;
                sink_.on_instrument_state(slot, false);
                keep = true;
            } // else: already reflected, by a snapshot or by an earlier pass
        });
        if (!ok) {
            ++stats_.malformed;
            if (!recovering_) {
                // What it carried is unknown and cannot be asked for by range.
                open_loss_ = false;
                mark_all_stale();
                for (Instrument& inst : instruments_)
                    inst.barred = true;
            }
            return false;
        }
        sink_.on_batch_end();
        return keep;
    }

    void process_snapshot(Reader& r, std::uint64_t pmap) noexcept {
        Snapshot message;
        bool begun = false, use = false;
        std::uint32_t slot = UINT32_MAX;
        const auto begin = [&](const Snapshot& m) {
            begun = true;
            slot = find_or_add(m.Symbol, optional_text(m.TradingSessionID));
            if (slot == UINT32_MAX)
                return;
            if (m.RouteFirst.value_or(1) == 0) {
                // A later fragment counts only as the continuation of a group seen from its start.
                if (fragment_active_ && fragment_slot_ == slot)
                    use = fragment_use_;
                else
                    fragment_active_ = false;
                return;
            }
            Instrument& inst = instruments_[slot];
            const auto rpt_seq = static_cast<std::uint32_t>(m.RptSeq);
            const bool fresh = cycle_id_ > inst.stale_cycle; // generated after the instrument went stale
            if (inst.state == State::Stale && inst.intact && fresh && rpt_seq == inst.rpt_seq) {
                // Nothing happened to it since: what the sink holds is current.
                inst.state = State::Synced;
                --stale_;
                ++stats_.confirmed_by_snapshot;
                sink_.on_instrument_state(slot, true);
            } else if (inst.state == State::Stale) {
                use = inst.first_rpt != 0 ? inst.first_rpt <= rpt_seq + 1 : fresh && (!inst.intact || rpt_seq > inst.rpt_seq);
                if (use) {
                    inst.intact = false;
                    sink_.on_clear(slot);
                } else {
                    ++stats_.snapshots_too_old;
                }
            }
            fragment_active_ = true;
            fragment_slot_ = slot;
            fragment_use_ = use;
        };
        const bool ok = decode(r, pmap, message, [&](const auto& m, const auto& e) {
            if (!begun)
                begin(m);
            if (use)
                sink_.on_snapshot(slot, m, e);
        });
        if (!ok) {
            ++stats_.malformed;
            fragment_active_ = false;
            cycle_clean_ = false;
            return;
        }
        if (!begun)
            begin(message);
        if (message.LastFragment.value_or(1) == 0)
            return;
        if (use && fragment_active_) {
            Instrument& inst = instruments_[slot];
            inst.rpt_seq = static_cast<std::uint32_t>(message.RptSeq);
            --stale_;
            ++stats_.snapshots_applied;
            if (inst.first_rpt == 0) {
                // Nothing arrived for it while it was stale, so there is nothing to replay.
                inst.state = State::Synced;
                sink_.on_instrument_state(slot, true);
            } else {
                inst.state = State::Pending;
                pending_slots_.push_back(slot);
                if (++pending_ >= kReplayBatch)
                    replay();
            }
        }
        fragment_active_ = false;
    }

    // Sequence number 1 on the snapshot feed opens a cycle and closes the one before it.
    void begin_cycle() noexcept {
        ++stats_.snapshot_cycles;
        // A cycle seen whole, begun after the last loss, has named every instrument. (A tail
        // lost on both feeds would go unnoticed here; such an instrument then surfaces through
        // its next entry, whose RptSeq sends it for a snapshot.)
        if (cycle_clean_ && cycle_id_ > epoch_cycle_)
            covered_ = true;
        if (pending_)
            replay();
        ++cycle_id_;
        cycle_clean_ = true;
    }

    // Releases every instrument whose snapshot has arrived by running the queued packets
    // again: RptSeq makes that idempotent. Packets still needed by others are kept.
    void replay() noexcept {
        ++stats_.replay_passes;
        for (const std::uint32_t slot : pending_slots_) {
            Instrument& inst = instruments_[slot];
            if (inst.state != State::Pending)
                continue; // resumed on its own in the meantime
            inst.state = State::Synced;
            sink_.on_instrument_state(slot, true);
        }
        pending_slots_.clear();
        pending_ = 0;
        replay_aborted_ = false;
        const std::size_t end = queue_end_;
        std::size_t read = queue_head_, write = 0;
        while (read < end && !replay_aborted_) {
            Record record;
            std::memcpy(&record, queue_.data() + read, sizeof(record));
            const std::size_t length = sizeof(record) + record.size;
            ++stats_.replayed;
            if (process(queue_.data() + read + sizeof(record), record.size)) {
                std::memmove(queue_.data() + write, queue_.data() + read, length);
                write += length;
            }
            read += length;
        }
        if (!replay_aborted_) {
            queue_head_ = 0;
            queue_end_ = write;
        }
    }

    void enqueue(const std::uint8_t* data, std::size_t size, std::uint32_t seq) noexcept {
        const std::size_t length = sizeof(Record) + size;
        if (queue_end_ + length > queue_.size() && !make_room(length)) {
            ++stats_.queue_dropped; // larger than the whole queue; its instruments will show a gap and resync
            return;
        }
        const Record record{seq, static_cast<std::uint32_t>(size)};
        std::memcpy(queue_.data() + queue_end_, &record, sizeof(record));
        std::memcpy(queue_.data() + queue_end_ + sizeof(record), data, size);
        queue_end_ += length;
        ++stats_.queued;
    }

    bool make_room(std::size_t length) noexcept {
        if (pending_)
            replay();
        // Still short: give up the oldest half. An instrument that needed it will find a
        // RptSeq gap when it is replayed and wait for a later snapshot.
        std::size_t head = queue_head_;
        while (head < queue_end_ && (queue_end_ - head > queue_.size() / 2 || queue_end_ - head + length > queue_.size())) {
            Record record;
            std::memcpy(&record, queue_.data() + head, sizeof(record));
            head += sizeof(record) + record.size;
            ++stats_.queue_dropped;
        }
        std::memmove(queue_.data(), queue_.data() + head, queue_end_ - head);
        queue_end_ -= head;
        queue_head_ = 0;
        return queue_end_ + length <= queue_.size();
    }

    void lost(std::uint32_t first_seq, std::uint32_t last_seq, bool restarted) noexcept {
        mark_all_stale(restarted);
        open_loss_ = tcp_replay_ && !restarted;
        if (!open_loss_)
            return;
        open_first_ = first_seq;
        open_last_ = last_seq;
        sink_.on_loss(first_seq, last_seq);
    }

    // Packets were lost: every instrument is suspect. After a restart of the gate's
    // numbering RptSeq starts over as well, so nothing held says anything about the new run.
    void mark_all_stale(bool restarted = false) noexcept {
        for (Instrument& inst : instruments_) {
            // Replay can make this one loss good for an instrument that was whole until now.
            // One still being restored has just lost what was queued for it as well, so only
            // its own next entry or a snapshot can clear it.
            inst.barred = inst.state != State::Synced;
            // A synced or restored instrument's data is still exact as of its RptSeq.
            inst.intact = !restarted && (inst.state != State::Stale || inst.intact);
            if (restarted)
                inst.rpt_seq = 0;
            inst.state = State::Stale;
            inst.first_rpt = 0;
            inst.stale_cycle = cycle_id_;
        }
        stale_ = instruments_.size();
        pending_ = 0;
        pending_slots_.clear();
        queue_head_ = queue_end_ = 0;
        replay_aborted_ = true;
        covered_ = false;
        epoch_cycle_ = cycle_id_;
        fragment_active_ = false;
        snapshot_expected_ = 0; // whatever cycle is in flight began before this loss
        cycle_clean_ = false;
        sink_.on_all_stale();
    }

    template <class Text> static Ascii optional_text(const Text& text) noexcept {
        return text.present ? text.value : Ascii{};
    }
    template <class Entry> static char entry_type(const Entry& e) noexcept {
        if constexpr (requires { e.MDEntryType.present; })
            return e.MDEntryType.value.front();
        else
            return e.MDEntryType.front();
    }

    static std::size_t hash(const Key& key) noexcept {
        std::uint64_t words[3];
        std::memcpy(words, key.data(), sizeof(words));
        std::uint64_t h = words[0] * 0x9E3779B97F4A7C15ull;
        h = (h ^ (h >> 29) ^ words[1]) * 0xBF58476D1CE4E5B9ull;
        h = (h ^ (h >> 32) ^ words[2]) * 0x94D049BB133111EBull;
        return static_cast<std::size_t>(h >> 24);
    }

    std::uint32_t find_or_add(Ascii symbol, Ascii board) noexcept {
        if (symbol.size() > kSymbolSize || board.size() > kBoardSize) {
            ++stats_.unkeyed_entries;
            return UINT32_MAX;
        }
        Key key{};
        symbol.copy(key.data(), kSymbolSize);
        board.copy(key.data() + kSymbolSize, kBoardSize);
        if (last_slot_ < instruments_.size() && instruments_[last_slot_].key == key) [[likely]]
            return last_slot_;
        const std::size_t mask = table_.size() - 1;
        std::size_t bucket = hash(key) & mask;
        while (const std::uint32_t entry = table_[bucket]) {
            if (instruments_[entry - 1].key == key)
                return last_slot_ = entry - 1;
            bucket = (bucket + 1) & mask;
        }
        const auto slot = static_cast<std::uint32_t>(instruments_.size());
        Instrument inst;
        inst.key = key;
        // Until a whole cycle has been seen, an unknown instrument has been out of sync
        // since the last loss; afterwards it is new as of now.
        inst.stale_cycle = covered_ ? cycle_id_ : epoch_cycle_;
        instruments_.push_back(inst);
        ++stale_;
        table_[bucket] = slot + 1;
        if (instruments_.size() * 2 > table_.size())
            rehash();
        sink_.on_instrument(slot, this->symbol(slot), this->board(slot));
        return last_slot_ = slot;
    }

    void rehash() noexcept {
        table_.assign(table_.size() * 2, 0);
        const std::size_t mask = table_.size() - 1;
        for (std::uint32_t slot = 0; slot < instruments_.size(); ++slot) {
            std::size_t bucket = hash(instruments_[slot].key) & mask;
            while (table_[bucket])
                bucket = (bucket + 1) & mask;
            table_[bucket] = slot + 1;
        }
    }

    Sink& sink_;
    StreamStats stats_;
    SequenceArbiter arbiter_;

    // Bound once so the arbiter calls straight into this object.
    struct Deliver {
        Stream* self;
        void operator()(const std::uint8_t* data, std::size_t size, std::uint32_t seq) const noexcept {
            self->deliver(data, size, seq);
        }
    } deliver_{this};
    struct Lost {
        Stream* self;
        void operator()(std::uint32_t first_missing, std::uint32_t next_seq, bool restarted) const noexcept {
            ++(restarted ? self->stats_.restarts : self->stats_.losses);
            self->lost(first_missing, next_seq - 1, restarted);
        }
    } lost_{this};

    // Instruments.
    std::vector<Instrument> instruments_;
    std::vector<std::uint32_t> table_;
    std::uint32_t last_slot_{};
    std::size_t stale_{};
    std::size_t pending_{};
    std::vector<std::uint32_t> pending_slots_;
    bool covered_{}; // a complete snapshot cycle has been seen since the last loss

    std::uint64_t replay_interval_ns_{};
    std::uint64_t last_replay_ns_{};

    // TCP replay: the one loss currently being made good.
    std::uint32_t open_first_{}, open_last_{};
    bool open_loss_{};
    bool tcp_replay_{};
    bool recovering_{}; // inside on_recovered

    // Recovery queue: Record-prefixed packets in arrival order, live in [queue_head_, queue_end_).
    std::vector<std::uint8_t> queue_;
    std::size_t queue_head_{};
    std::size_t queue_end_{};
    bool replay_aborted_{};

    // Snapshot feed.
    std::uint32_t snapshot_expected_{};
    std::uint32_t snapshot_miss_run_{};
    Seen snapshot_seen_[kSnapshotSeen]{}; // recently processed snapshot packets
    std::uint32_t cycle_id_{1};
    std::uint32_t epoch_cycle_{1}; // cycle in flight at the last loss (or at start)
    bool cycle_clean_{};
    bool fragment_active_{};
    bool fragment_use_{};
    std::uint32_t fragment_slot_{};
};

// The four order and trade streams.
template <class Sink> using CurrencyOrders = Stream<X_OLR_CURR, W_OLS_CURR, Sink>;
template <class Sink> using CurrencyTrades = Stream<X_TLR_CURR, W_TLS_CURR, Sink>;
template <class Sink> using EquityOrders = Stream<X_OLR_FOND, W_OLS_FOND, Sink>;
template <class Sink> using EquityTrades = Stream<X_TLR_FOND, W_TLS_FOND, Sink>;

} // namespace moex::fast::asts
