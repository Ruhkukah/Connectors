#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace moex::twime_trade {

struct TwimeRecoveryState {
    std::uint64_t next_outbound_seq{1};
    std::uint64_t next_expected_inbound_seq{1};
    std::uint64_t last_establishment_id{0};
    std::uint64_t recovery_epoch{1};
    bool last_clean_shutdown{true};
};

class TwimeRecoveryStateStore {
  public:
    virtual ~TwimeRecoveryStateStore() = default;

    [[nodiscard]] virtual std::optional<TwimeRecoveryState> load(std::string_view session_id) const = 0;
    virtual void save(std::string_view session_id, const TwimeRecoveryState& state) = 0;
};

class TwimeInMemoryRecoveryStateStore final : public TwimeRecoveryStateStore {
  public:
    [[nodiscard]] std::optional<TwimeRecoveryState> load(std::string_view session_id) const override;
    void save(std::string_view session_id, const TwimeRecoveryState& state) override;

  private:
    std::unordered_map<std::string, TwimeRecoveryState> states_;
};

struct TwimeJournalEntry {
    std::uint64_t sequence_number{0};
    bool consumes_sequence{false};
    std::uint16_t template_id{0};
    std::string message_name;
    std::vector<std::byte> bytes;
    std::string cert_log_line;
};

class TwimeBoundedJournal {
  public:
    explicit TwimeBoundedJournal(std::size_t capacity = 64);

    void append(const TwimeJournalEntry& entry);
    [[nodiscard]] std::vector<TwimeJournalEntry> last_n(std::size_t count) const;
    [[nodiscard]] std::span<const TwimeJournalEntry> entries() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

  private:
    std::size_t capacity_{64};
    std::deque<TwimeJournalEntry> entries_;
    mutable std::vector<TwimeJournalEntry> scratch_;
};

using TwimeOutboundJournal = TwimeBoundedJournal;
using TwimeInboundJournal = TwimeBoundedJournal;

}  // namespace moex::twime_trade
