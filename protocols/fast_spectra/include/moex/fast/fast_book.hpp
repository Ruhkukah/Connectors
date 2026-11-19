#pragma once

// Aggregated order book for the FO-BOOK-N / SPOT-BOOK-N streams. The gate
// addresses levels by position (MDPriceLevel, 1-based): New inserts and shifts
// deeper levels down, Delete removes and shifts them up, Change overwrites.
// Levels pushed beyond Depth fall off without a message, as the spec requires.

#include "fast_spectra_messages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace moex::fast {

struct BookLevel {
    Decimal price{};
    std::int64_t size{};
};

template <std::size_t Depth> class AggregateBook {
  public:
    struct Side {
        std::array<BookLevel, Depth> levels{};
        std::uint32_t count{};

        [[nodiscard]] const BookLevel* best() const noexcept {
            return count ? &levels[0] : nullptr;
        }
    };

    void clear() noexcept {
        bids_.count = asks_.count = 0;
    }

    // Returns false for entries that are not book levels (trades, statistics).
    bool apply(const DefaultIncrementalRefreshMessage::MDEntries& e) noexcept {
        const char type = e.MDEntryType.front();
        if (type == 'J') {
            clear();
            return true;
        }
        if (type != '0' && type != '1')
            return false;
        Side& side = type == '0' ? bids_ : asks_;
        const std::uint32_t level = e.MDPriceLevel.value_or(0);
        if (level == 0 || level > Depth)
            return true;
        const std::uint32_t index = level - 1;
        switch (e.MDUpdateAction) {
        case 0: { // New
            const std::uint32_t at = index < side.count ? index : side.count;
            const std::uint32_t last = side.count < Depth ? side.count : Depth - 1;
            for (std::uint32_t i = last; i > at; --i)
                side.levels[i] = side.levels[i - 1];
            side.levels[at] = {e.MDEntryPx.value, e.MDEntrySize.value_or(0)};
            if (side.count < Depth)
                ++side.count;
            break;
        }
        case 1: // Change
            if (index < side.count)
                side.levels[index] = {e.MDEntryPx.value, e.MDEntrySize.value_or(0)};
            break;
        case 2: // Delete
            if (index < side.count) {
                for (std::uint32_t i = index; i + 1 < side.count; ++i)
                    side.levels[i] = side.levels[i + 1];
                --side.count;
            }
            break;
        default:
            break;
        }
        return true;
    }

    bool apply(const DefaultSnapshotMessage::MDEntries& e) noexcept {
        const char type = e.MDEntryType.front();
        if (type == 'J') {
            clear();
            return true;
        }
        if (type != '0' && type != '1')
            return false;
        Side& side = type == '0' ? bids_ : asks_;
        const std::uint32_t level = e.MDPriceLevel.value_or(side.count + 1);
        if (level == 0 || level > Depth)
            return true;
        side.levels[level - 1] = {e.MDEntryPx.value, e.MDEntrySize.value_or(0)};
        if (side.count < level)
            side.count = level;
        return true;
    }

    [[nodiscard]] const Side& bids() const noexcept {
        return bids_;
    }
    [[nodiscard]] const Side& asks() const noexcept {
        return asks_;
    }

  private:
    Side bids_;
    Side asks_;
};

} // namespace moex::fast
