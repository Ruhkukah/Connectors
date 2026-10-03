#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace moex::connector_host {
[[nodiscard]] std::string json_string(std::string_view text);
// Throws on disabled logging or non-default severity overrides.
void validate_cgate_logging(std::string_view env_open_settings, const std::filesystem::path& config_dir = {});
struct JournalReservation {
    std::int32_t next_ext_id{1};
    std::uint32_t next_user_id{1};
};
class EventJournal {
  public:
    explicit EventJournal(const std::filesystem::path& path, const std::filesystem::path& state_path = {});
    ~EventJournal();
    EventJournal(const EventJournal&) = delete;
    EventJournal& operator=(const EventJournal&) = delete;
    void append(std::string_view kind, std::string_view fields = "{}");
    void flush();
    void flush_if_due();
    [[nodiscard]] JournalReservation reservations() const noexcept {
        return reservations_;
    }
    [[nodiscard]] std::uint64_t recovery_read_bytes() const noexcept {
        return recovery_read_bytes_;
    }

  private:
    void extend_reservations(const JournalReservation& next);
    void write_buffer();
    int fd_{-1};
    int state_lock_fd_{-1};
    std::filesystem::path state_path_;
    std::uint64_t device_{}, inode_{}, offset_{}, recovery_read_bytes_{};
    std::uint64_t durable_offset_{}, durable_boundary_{14695981039346656037ULL};
    std::chrono::steady_clock::time_point last_sync_{};
    JournalReservation reservations_;
    JournalReservation high_water_;
    std::string buffered_;
    bool dirty_{};
};
} // namespace moex::connector_host
