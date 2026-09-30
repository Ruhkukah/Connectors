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
    explicit EventJournal(const std::filesystem::path& path);
    ~EventJournal();
    EventJournal(const EventJournal&) = delete;
    EventJournal& operator=(const EventJournal&) = delete;
    void append(std::string_view kind, std::string_view fields = "{}");
    void flush();
    void flush_reservations();
    [[nodiscard]] JournalReservation reservations() const noexcept {
        return reservations_;
    }

  private:
    int fd_{-1};
    std::chrono::steady_clock::time_point last_sync_{};
    JournalReservation reservations_;
    bool dirty_{}, reservations_dirty_{};
};
} // namespace moex::connector_host
