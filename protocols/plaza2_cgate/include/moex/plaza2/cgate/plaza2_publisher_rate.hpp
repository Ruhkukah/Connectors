#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace moex::plaza2::cgate {
struct Plaza2PublisherRateMetrics {
    std::uint64_t admitted{}, throttled{}, clock_regressions{}, penalty_until_ms{};
    std::size_t in_window{};
};

// Single publisher owner; count attempted submissions conservatively, never refund uncertain posts.
// The host retains this object across listener/publisher/environment reopen.
class Plaza2PublisherRateGate {
  public:
    explicit Plaza2PublisherRateGate(std::uint32_t per_second)
        : times_(std::min<std::uint32_t>(per_second, 3000)), valid_(per_second > 0 && per_second <= 3000) {}
    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }
    [[nodiscard]] bool admit(std::uint64_t now_ms) noexcept {
        if (!valid_ || now_ms < last_ms_) {
            ++metrics_.throttled;
            metrics_.clock_regressions += now_ms < last_ms_;
            return false;
        }
        last_ms_ = now_ms;
        while (used_ && now_ms - times_[head_] >= 1000) {
            head_ = (head_ + 1) % times_.size();
            --used_;
        }
        metrics_.in_window = used_;
        if (now_ms < metrics_.penalty_until_ms || used_ == times_.size()) {
            ++metrics_.throttled;
            return false;
        }
        times_[(head_ + used_) % times_.size()] = now_ms;
        ++used_;
        metrics_.in_window = used_;
        ++metrics_.admitted;
        return true;
    }
    void penalize(std::uint64_t now_ms, std::uint32_t milliseconds) noexcept {
        // uint32 duration cannot overflow any steady-clock timestamp in practical use; saturate defensively.
        const auto base = std::max(last_ms_, now_ms);
        const auto until = UINT64_MAX - base < milliseconds ? UINT64_MAX : base + milliseconds;
        metrics_.penalty_until_ms = std::max(metrics_.penalty_until_ms, until);
    }
    [[nodiscard]] Plaza2PublisherRateMetrics metrics() const noexcept {
        return metrics_;
    }

  private:
    std::vector<std::uint64_t> times_;
    std::size_t head_{}, used_{};
    std::uint64_t last_ms_{};
    bool valid_{};
    Plaza2PublisherRateMetrics metrics_{};
};
} // namespace moex::plaza2::cgate
