#include "moex/twime_trade/twime_recovery_state.hpp"

#include <algorithm>

namespace moex::twime_trade {

std::optional<TwimeRecoveryState> TwimeInMemoryRecoveryStateStore::load(std::string_view session_id) const {
    const auto found = states_.find(std::string(session_id));
    if (found == states_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void TwimeInMemoryRecoveryStateStore::save(std::string_view session_id, const TwimeRecoveryState& state) {
    states_[std::string(session_id)] = state;
}

TwimeBoundedJournal::TwimeBoundedJournal(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

void TwimeBoundedJournal::append(const TwimeJournalEntry& entry) {
    if (entries_.size() == capacity_) {
        entries_.pop_front();
    }
    entries_.push_back(entry);
}

std::vector<TwimeJournalEntry> TwimeBoundedJournal::last_n(std::size_t count) const {
    if (count >= entries_.size()) {
        return std::vector<TwimeJournalEntry>(entries_.begin(), entries_.end());
    }
    return std::vector<TwimeJournalEntry>(entries_.end() - static_cast<std::ptrdiff_t>(count), entries_.end());
}

std::span<const TwimeJournalEntry> TwimeBoundedJournal::entries() const noexcept {
    scratch_.assign(entries_.begin(), entries_.end());
    return scratch_;
}

std::size_t TwimeBoundedJournal::size() const noexcept {
    return entries_.size();
}

std::size_t TwimeBoundedJournal::capacity() const noexcept {
    return capacity_;
}

} // namespace moex::twime_trade
