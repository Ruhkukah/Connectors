#pragma once

#include <cstdint>
#include <optional>

namespace moex::twime_trade {

struct TwimeSequenceGap {
    std::uint64_t from_seq_no{0};
    std::uint64_t to_seq_no{0};
};

struct TwimeSequenceObservation {
    bool accepted{true};
    bool consumes_sequence{false};
    bool duplicate_or_stale{false};
    std::optional<TwimeSequenceGap> gap;
};

class TwimeSequenceState {
  public:
    void reset(std::uint64_t next_outbound_seq = 1, std::uint64_t next_expected_inbound_seq = 1) noexcept;

    [[nodiscard]] std::uint64_t next_outbound_seq() const noexcept;
    [[nodiscard]] std::uint64_t next_expected_inbound_seq() const noexcept;
    [[nodiscard]] std::uint64_t last_peer_next_seq_no() const noexcept;

    [[nodiscard]] std::uint64_t reserve_outbound_sequence(bool consumes_sequence) noexcept;
    [[nodiscard]] TwimeSequenceObservation observe_inbound(std::uint64_t sequence_number,
                                                           bool consumes_sequence) noexcept;
    [[nodiscard]] std::optional<TwimeSequenceGap> observe_peer_next_seq(std::uint64_t peer_next_seq_no) noexcept;

    void restore_outbound_next(std::uint64_t value) noexcept;
    void restore_expected_inbound_next(std::uint64_t value) noexcept;

  private:
    std::uint64_t next_outbound_seq_{1};
    std::uint64_t next_expected_inbound_seq_{1};
    std::uint64_t last_peer_next_seq_no_{0};
};

} // namespace moex::twime_trade
