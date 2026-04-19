#include "moex/twime_trade/twime_rate_limit_model.hpp"

namespace moex::twime_trade {

TwimeRateLimitModel::TwimeRateLimitModel(std::uint32_t max_messages_per_window, std::uint64_t window_ms) noexcept
    : max_messages_per_window_(max_messages_per_window == 0 ? 1 : max_messages_per_window),
      window_ms_(window_ms == 0 ? 1 : window_ms) {}

TwimeRateLimitDecision TwimeRateLimitModel::observe_send(std::uint64_t now_ms) noexcept {
    if (used_in_window_ == 0 || now_ms >= window_start_ms_ + window_ms_) {
        window_start_ms_ = now_ms;
        used_in_window_ = 0;
    }

    TwimeRateLimitDecision decision;
    if (used_in_window_ >= max_messages_per_window_) {
        decision.allowed = false;
        decision.used_in_window = used_in_window_;
        decision.retry_after_ms = (window_start_ms_ + window_ms_) - now_ms;
        return decision;
    }

    ++used_in_window_;
    decision.allowed = true;
    decision.used_in_window = used_in_window_;
    return decision;
}

void TwimeRateLimitModel::reset() noexcept {
    window_start_ms_ = 0;
    used_in_window_ = 0;
}

}  // namespace moex::twime_trade
