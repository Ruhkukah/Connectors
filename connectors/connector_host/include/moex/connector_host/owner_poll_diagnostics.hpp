#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace moex::connector_host {
// Timing diagnostics are sampled; the command and native state journal is not.
// Keep one fully attributed worst turn and the population count per second.
class OwnerPollDiagnostics {
  public:
    using Clock = std::chrono::steady_clock;
    [[nodiscard]] bool due(Clock::time_point now) const {
        return count_ != 0 && now >= deadline_;
    }
    [[nodiscard]] bool observe(Clock::time_point now, std::uint64_t sequence, std::int64_t total_ns) {
        if (count_ == 0) {
            deadline_ = now + std::chrono::seconds(1);
            first_sequence_ = sequence;
        }
        ++count_;
        last_sequence_ = sequence;
        if (count_ != 1 && total_ns <= maximum_ns_)
            return false;
        maximum_ns_ = total_ns;
        return true;
    }
    void capture(std::string fields) {
        worst_fields_ = std::move(fields);
    }
    [[nodiscard]] std::string take() {
        if (count_ == 0)
            return {};
        auto fields = std::move(worst_fields_);
        fields.pop_back();
        fields += ",\"sampled_polls\":" + std::to_string(count_) +
                  ",\"first_sequence\":" + std::to_string(first_sequence_) +
                  ",\"last_sequence\":" + std::to_string(last_sequence_) +
                  ",\"max_total_ns\":" + std::to_string(maximum_ns_) + ",\"sampling_interval_ns\":1000000000}";
        count_ = first_sequence_ = last_sequence_ = 0;
        maximum_ns_ = 0;
        return fields;
    }

  private:
    Clock::time_point deadline_{};
    std::uint64_t count_{}, first_sequence_{}, last_sequence_{};
    std::int64_t maximum_ns_{};
    std::string worst_fields_;
};
} // namespace moex::connector_host
