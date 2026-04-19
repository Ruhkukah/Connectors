#include "moex/twime_trade/twime_sequence_state.hpp"

namespace moex::twime_trade {

void TwimeSequenceState::reset(std::uint64_t next_outbound_seq, std::uint64_t next_expected_inbound_seq) noexcept {
    next_outbound_seq_ = next_outbound_seq == 0 ? 1 : next_outbound_seq;
    next_expected_inbound_seq_ = next_expected_inbound_seq == 0 ? 1 : next_expected_inbound_seq;
    last_peer_next_seq_no_ = 0;
}

std::uint64_t TwimeSequenceState::next_outbound_seq() const noexcept {
    return next_outbound_seq_;
}

std::uint64_t TwimeSequenceState::next_expected_inbound_seq() const noexcept {
    return next_expected_inbound_seq_;
}

std::uint64_t TwimeSequenceState::last_peer_next_seq_no() const noexcept {
    return last_peer_next_seq_no_;
}

std::uint64_t TwimeSequenceState::reserve_outbound_sequence(bool consumes_sequence) noexcept {
    const auto current = next_outbound_seq_;
    if (consumes_sequence) {
        ++next_outbound_seq_;
    }
    return current;
}

TwimeSequenceObservation TwimeSequenceState::observe_inbound(std::uint64_t sequence_number,
                                                             bool consumes_sequence) noexcept {
    TwimeSequenceObservation observation;
    observation.consumes_sequence = consumes_sequence;
    if (!consumes_sequence) {
        return observation;
    }

    if (sequence_number < next_expected_inbound_seq_) {
        observation.accepted = false;
        observation.duplicate_or_stale = true;
        return observation;
    }

    if (sequence_number > next_expected_inbound_seq_) {
        observation.accepted = false;
        observation.gap = TwimeSequenceGap{next_expected_inbound_seq_, sequence_number - 1};
        return observation;
    }

    ++next_expected_inbound_seq_;
    return observation;
}

std::optional<TwimeSequenceGap> TwimeSequenceState::observe_peer_next_seq(std::uint64_t peer_next_seq_no) noexcept {
    last_peer_next_seq_no_ = peer_next_seq_no;
    if (peer_next_seq_no == 0 || peer_next_seq_no <= next_expected_inbound_seq_) {
        return std::nullopt;
    }
    return TwimeSequenceGap{next_expected_inbound_seq_, peer_next_seq_no - 1};
}

void TwimeSequenceState::restore_outbound_next(std::uint64_t value) noexcept {
    next_outbound_seq_ = value == 0 ? 1 : value;
}

void TwimeSequenceState::restore_expected_inbound_next(std::uint64_t value) noexcept {
    next_expected_inbound_seq_ = value == 0 ? 1 : value;
}

} // namespace moex::twime_trade
