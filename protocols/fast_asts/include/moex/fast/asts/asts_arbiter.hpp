#pragma once

// A/B feed arbitration by the 4-byte sequence preamble: delivers each packet
// once and in order, holds packets that arrive ahead of a missing one for a
// short while, and reports a loss when the missing packet does not turn up.
// On the ASTS gate heartbeats and session-status messages consume sequence
// numbers like any other message, so there are no special cases here.

#include "moex/fast/fast_wire.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace moex::fast::asts {

class SequenceArbiter {
  public:
    static constexpr std::size_t kMaxPacket = 2048;

    SequenceArbiter(std::uint32_t window, std::uint64_t timeout_ns) : timeout_ns_(timeout_ns) {
        window_ = 8;
        while (window_ < window)
            window_ <<= 1;
        slots_.resize(window_);
        bytes_.resize(std::size_t{window_} * kMaxPacket);
    }

    // deliver(data, size, seq) is called for every packet, in sequence.
    // lost(next_seq, restarted) is called first whenever packets before next_seq
    // are gone for good; restarted means the gate began numbering again.
    template <class Deliver, class Lost>
    void push(const std::uint8_t* data, std::size_t size, std::uint64_t now_ns, Deliver&& deliver, Lost&& lost) {
        const std::uint32_t seq = read_preamble(data);
        if (!started_) {
            started_ = true;
            expected_ = seq;
        }
        if (seq == expected_) [[likely]] {
            behind_run_ = 0;
            expected_ = seq + 1;
            deliver(data, size, seq);
            if (buffered_)
                drain(now_ns, deliver);
        } else if (seq < expected_) {
            ++duplicates;
            // A long run far behind means the gate restarted its numbering unannounced.
            if (expected_ - seq <= window_ || ++behind_run_ <= 2 * window_)
                return;
            jump(data, size, seq, true, deliver, lost);
        } else if (seq - expected_ >= window_ || size > kMaxPacket) {
            jump(data, size, seq, false, deliver, lost);
        } else {
            Slot& slot = slots_[seq & (window_ - 1)];
            if (slot.size != 0 && slot.seq == seq) {
                ++duplicates;
            } else {
                if (slot.size == 0 && buffered_++ == 0)
                    waiting_since_ns_ = now_ns;
                slot = {seq, static_cast<std::uint32_t>(size)};
                std::memcpy(slot_data(seq), data, size);
                ++reordered;
            }
        }
        on_timer(now_ns, deliver, lost);
    }

    // Call when the feed is quiet so a held-back packet still gets released.
    template <class Deliver, class Lost> void on_timer(std::uint64_t now_ns, Deliver&& deliver, Lost&& lost) {
        if (!buffered_ || now_ns - waiting_since_ns_ < timeout_ns_)
            return;
        std::uint32_t next = expected_ + 1;
        while (slots_[next & (window_ - 1)].size == 0 || slots_[next & (window_ - 1)].seq != next) {
            if (++next - expected_ > window_) { // nothing usable is held after all
                clear();
                return;
            }
        }
        ++gaps;
        expected_ = next;
        lost(next, false);
        drain(now_ns, deliver);
    }

    [[nodiscard]] bool started() const noexcept {
        return started_;
    }
    [[nodiscard]] std::uint32_t next() const noexcept {
        return expected_;
    }

    std::uint64_t duplicates{}, reordered{}, gaps{};

  private:
    struct Slot {
        std::uint32_t seq{};
        std::uint32_t size{};
    };

    std::uint8_t* slot_data(std::uint32_t seq) noexcept {
        return bytes_.data() + std::size_t{seq & (window_ - 1)} * kMaxPacket;
    }
    void clear() noexcept {
        for (Slot& slot : slots_)
            slot.size = 0;
        buffered_ = 0;
    }
    template <class Deliver, class Lost>
    void jump(const std::uint8_t* data, std::size_t size, std::uint32_t seq, bool restarted, Deliver&& deliver,
              Lost&& lost) {
        ++gaps;
        behind_run_ = 0;
        clear();
        expected_ = seq + 1;
        lost(seq, restarted);
        deliver(data, size, seq);
    }
    template <class Deliver> void drain(std::uint64_t now_ns, Deliver&& deliver) {
        while (buffered_) {
            Slot& slot = slots_[expected_ & (window_ - 1)];
            if (slot.size == 0 || slot.seq != expected_)
                break;
            const std::uint32_t size = slot.size;
            slot.size = 0;
            --buffered_;
            expected_ = slot.seq + 1;
            deliver(slot_data(slot.seq), size, slot.seq);
        }
        waiting_since_ns_ = now_ns;
    }

    std::uint32_t expected_{};
    std::uint32_t window_{};
    std::uint32_t buffered_{};
    std::uint32_t behind_run_{};
    std::uint64_t waiting_since_ns_{};
    std::uint64_t timeout_ns_{};
    bool started_{};
    std::vector<Slot> slots_;
    std::vector<std::uint8_t> bytes_;
};

} // namespace moex::fast::asts
