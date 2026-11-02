#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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
    explicit EventJournal(const std::filesystem::path& path, const std::filesystem::path& state_path = {},
                          std::size_t queue_bytes = 8 * 1024 * 1024);
    ~EventJournal();
    EventJournal(const EventJournal&) = delete;
    EventJournal& operator=(const EventJournal&) = delete;
    void append(std::string_view kind, std::string_view fields = "{}");
    // Explicit shutdown/test drain. Ordinary owner turns never wait for I/O.
    void flush();
    // Nonblocking writer-health check; the writer owns the 250 ms group sync.
    void flush_if_due();
    void request_recovery();
    [[nodiscard]] bool recovery_pending() const;
    [[nodiscard]] JournalReservation reservations() const noexcept {
        return reservations_;
    }
    // The checkpoint's exclusive ceiling is unchanged after a failed block
    // extension. Storage-failed cancellation may use only this durable range.
    [[nodiscard]] bool user_id_reserved(std::uint32_t id) const noexcept {
        return id != 0 && id < high_water_.next_user_id;
    }
    [[nodiscard]] std::uint64_t recovery_read_bytes() const noexcept {
        return recovery_read_bytes_;
    }

  private:
    void extend_reservations(const JournalReservation& next);
    void write_buffer();
    void sync_buffer();
    void writer_loop() noexcept;
    int fd_{-1};
    int state_lock_fd_{-1};
    std::filesystem::path state_path_;
    std::filesystem::path boundary_path_;
    std::uint64_t device_{}, inode_{}, offset_{}, recovery_read_bytes_{};
    std::chrono::steady_clock::time_point last_sync_{};
    JournalReservation reservations_;
    JournalReservation high_water_;
    std::string buffered_;
    bool dirty_{};
    const std::size_t queue_limit_;
    struct Record {
        std::chrono::system_clock::time_point wall;
        std::string kind, fields;
        std::size_t bytes;
    };
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_changed_;
    std::deque<Record> queue_;
    std::size_t pending_bytes_{}, pending_records_{};
    std::uint64_t flush_requested_{}, flush_completed_{};
    std::string writer_error_;
    bool retry_requested_{}, recovery_pending_{}, stopping_{}, io_failed_{};
    std::thread writer_;
};
} // namespace moex::connector_host
