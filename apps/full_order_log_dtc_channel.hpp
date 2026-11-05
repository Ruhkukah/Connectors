#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace moex::connector_host {

// Single producer and single consumer. One slot stays empty to distinguish
// a full ring from an empty ring.
template <class T, std::size_t Slots> class FolDtcChannel {
  public:
    static_assert(Slots > 1);

    bool push(T value) {
        return try_move(value);
    }
    // A rejected write leaves ownership with the producer for a fallback.
    bool try_move(T& value) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) % Slots;
        if (next == read_.load(std::memory_order_acquire))
            return false;
        slots_[write] = std::move(value);
        write_.store(next, std::memory_order_release);
        return true;
    }
    std::optional<T> pop() {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire))
            return std::nullopt;
        T value = std::move(slots_[read]);
        read_.store((read + 1) % Slots, std::memory_order_release);
        return value;
    }

  private:
    std::array<T, Slots> slots_{};
    std::atomic_size_t read_{}, write_{};
};

// A latest-value handoff. Only the exchanger that obtains the prior pointer
// owns it, even when publication and consumption happen at the same instant.
template <class T> class FolDtcMailbox {
  public:
    ~FolDtcMailbox() {
        delete value_.exchange(nullptr, std::memory_order_relaxed);
    }
    void publish(std::unique_ptr<T> value) noexcept {
        delete value_.exchange(value.release(), std::memory_order_acq_rel);
    }
    [[nodiscard]] std::unique_ptr<T> take() noexcept {
        return std::unique_ptr<T>(value_.exchange(nullptr, std::memory_order_acq_rel));
    }

  private:
    std::atomic<T*> value_{};
};

} // namespace moex::connector_host
