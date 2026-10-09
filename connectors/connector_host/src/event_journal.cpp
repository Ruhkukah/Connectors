#include "moex/connector_host/event_journal.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"
#include "moex/plaza2/cgate/cgate_logging.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace moex::connector_host {
namespace {
std::uint64_t reservation(std::string_view line, std::string_view field) {
    const auto key = std::string("\"") + std::string(field) + "\":";
    const auto found = line.find(key);
    if (found == std::string_view::npos)
        return 0;
    auto start = found + key.size();
    while (start < line.size() && line[start] == ' ')
        ++start;
    std::uint64_t value{};
    const auto result = std::from_chars(line.data() + start, line.data() + line.size(), value);
    return result.ec == std::errc{} ? value : 0;
}
std::string stamp(std::chrono::system_clock::time_point time, int hours) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
    std::time_t seconds = micros / 1000000 + hours * 3600;
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char out[64];
    std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%S", &utc);
    std::string fraction = std::to_string(micros % 1000000);
    fraction.insert(0, 6 - fraction.size(), '0');
    return std::string(out) + "." + fraction + (hours == 0 ? "Z" : "+03:00");
}
void update_reservations(JournalReservation& value, std::string_view text) {
    const auto ext = reservation(text, "next_ext_id"), user = reservation(text, "next_user_id");
    if (ext > INT32_MAX || user > UINT32_MAX)
        throw std::runtime_error("journal reservation exhausted");
    value.next_ext_id = std::max(value.next_ext_id, static_cast<std::int32_t>(ext));
    value.next_user_id = std::max(value.next_user_id, static_cast<std::uint32_t>(user));
}
JournalReservation reserve_block(JournalReservation next) {
    next.next_ext_id =
        static_cast<std::int32_t>(std::min<std::int64_t>(INT32_MAX, std::int64_t(next.next_ext_id) + 1000));
    next.next_user_id =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(UINT32_MAX, std::uint64_t(next.next_user_id) + 1000));
    return next;
}
std::uint64_t text_hash(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char value : text)
        hash = (hash ^ value) * 1099511628211ULL;
    return hash;
}
std::string checkpoint_record(std::uint64_t device, std::uint64_t inode, std::uint64_t offset, std::uint64_t ext,
                              std::uint64_t user, std::uint64_t boundary) {
    return "MOEXJ2 " + std::to_string(device) + " " + std::to_string(inode) + " " + std::to_string(offset) + " " +
           std::to_string(ext) + " " + std::to_string(user) + " " + std::to_string(boundary);
}
std::uint64_t boundary_hash(int fd, std::uint64_t offset) {
    std::array<char, 256> bytes{};
    const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(offset, bytes.size()));
    std::size_t read{};
    while (read < size) {
        const auto count = ::pread(fd, bytes.data() + read, size - read, static_cast<off_t>(offset - size + read));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("cannot verify interaction log checkpoint");
        read += static_cast<std::size_t>(count);
    }
    return text_hash(std::string_view(bytes.data(), size));
}
void write_checkpoint(const std::filesystem::path& path, std::string_view text) {
    auto temporary = path.string() + ".tmp.XXXXXX";
    int fd = ::mkstemp(temporary.data());
    if (fd < 0)
        throw std::runtime_error("cannot create identity checkpoint");
    try {
        std::size_t written{};
        while (written < text.size()) {
            const auto count = ::write(fd, text.data() + written, text.size() - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error("identity checkpoint write failed");
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(fd) != 0)
            throw std::runtime_error("identity checkpoint fsync failed");
        ::close(fd);
        fd = -1;
        if (::rename(temporary.c_str(), path.c_str()) != 0)
            throw std::runtime_error("identity checkpoint rename failed");
        const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
        const auto directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            throw std::runtime_error("cannot open checkpoint directory");
        const auto result = ::fsync(directory);
        ::close(directory);
        if (result != 0)
            throw std::runtime_error("checkpoint directory fsync failed");
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        ::unlink(temporary.c_str());
        throw;
    }
}
} // namespace

std::string json_string(std::string_view text) {
    return plaza2::cgate::text::json_quote_utf8(text);
}

void validate_cgate_logging(std::string_view settings, const std::filesystem::path& config_dir) {
    plaza2::cgate::validate_cgate_logging(settings, config_dir);
}

EventJournal::EventJournal(const std::filesystem::path& path, const std::filesystem::path& state_path,
                           std::size_t queue_bytes)
    : state_path_(state_path.empty() ? std::filesystem::path(path.string() + ".state") : state_path),
      boundary_path_(state_path_.string() + ".journal"), queue_limit_(queue_bytes) {
    if (queue_limit_ == 0)
        throw std::invalid_argument("interaction log queue must have positive capacity");
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC, 0600);
    if (fd_ < 0)
        throw std::runtime_error("cannot open interaction log");
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("interaction log already owned");
    }
    try {
        if (!state_path_.parent_path().empty())
            std::filesystem::create_directories(state_path_.parent_path());
        state_lock_fd_ = ::open((state_path_.string() + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (state_lock_fd_ < 0 || ::flock(state_lock_fd_, LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("identity state already owned or unavailable");
        struct stat info {};
        if (::fstat(fd_, &info) != 0)
            throw std::runtime_error("cannot stat interaction log");
        device_ = static_cast<std::uint64_t>(info.st_dev);
        inode_ = static_cast<std::uint64_t>(info.st_ino);
        const auto recover_checkpoint = [&](const std::filesystem::path& checkpoint_path, bool identity) {
            if (!std::filesystem::exists(checkpoint_path))
                return;
            std::ifstream state(checkpoint_path);
            std::string magic, extra;
            std::uint64_t device{}, inode{}, checkpoint{}, ext{}, user{}, hash{}, checksum{};
            if (!(state >> magic >> device >> inode >> checkpoint >> ext >> user >> hash >> checksum) ||
                magic != "MOEXJ2" || (state >> extra) || ext == 0 || ext > INT32_MAX || user == 0 ||
                user > UINT32_MAX ||
                checksum != text_hash(checkpoint_record(device, inode, checkpoint, ext, user, hash)))
                throw std::runtime_error("identity checkpoint is invalid; reconcile before restarting");
            if (identity)
                reservations_ = {.next_ext_id = static_cast<std::int32_t>(ext),
                                 .next_user_id = static_cast<std::uint32_t>(user)};
            // Separate files give the owner exclusive ID-reservation writes
            // and the writer exclusive durable-boundary writes. Legacy MOEXJ2
            // checkpoints remain valid on the first asynchronous restart.
            if (device == device_ && inode == inode_) {
                if (checkpoint > static_cast<std::uint64_t>(info.st_size) || boundary_hash(fd_, checkpoint) != hash)
                    throw std::runtime_error("interaction log checkpoint mismatch; reconcile before restarting");
                offset_ = std::max(offset_, checkpoint);
            }
        };
        if (!std::filesystem::exists(state_path_) && std::filesystem::exists(boundary_path_))
            throw std::runtime_error(
                "identity checkpoint missing while journal boundary exists; reconcile before restarting");
        recover_checkpoint(state_path_, true);
        recover_checkpoint(boundary_path_, false);
        std::ifstream file(path);
        file.seekg(static_cast<std::streamoff>(offset_));
        std::string line;
        std::uint64_t complete = offset_;
        while (std::getline(file, line)) {
            recovery_read_bytes_ += line.size() + (file.eof() ? 0 : 1);
            // A partial final record cannot be a restart hint. Recover the
            // complete NDJSON prefix before adding another line.
            if (file.eof())
                break;
            complete += line.size() + 1;
            update_reservations(reservations_, line);
        }
        if (::ftruncate(fd_, static_cast<off_t>(complete)) != 0)
            throw std::runtime_error("cannot recover interaction log tail");
        offset_ = complete;
        high_water_ = reserve_block(reservations_);
        dirty_ = true;
        const auto record =
            checkpoint_record(device_, inode_, 0, high_water_.next_ext_id, high_water_.next_user_id, text_hash({}));
        write_checkpoint(state_path_, record + " " + std::to_string(text_hash(record)) + "\n");
        // A boundary may hide recovered reservation records from the next
        // restart scan, so publish their durable ID ceiling first.
        sync_buffer();
        writer_ = std::thread([this] { writer_loop(); });
    } catch (...) {
        if (state_lock_fd_ >= 0)
            ::close(state_lock_fd_);
        state_lock_fd_ = -1;
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}
EventJournal::~EventJournal() {
    if (writer_.joinable()) {
        {
            std::lock_guard lock(queue_mutex_);
            stopping_ = true;
        }
        queue_changed_.notify_one();
        writer_.join();
    }
    if (fd_ >= 0)
        ::close(fd_);
    if (state_lock_fd_ >= 0)
        ::close(state_lock_fd_);
}
void EventJournal::append(std::string_view kind, std::string_view fields) {
    if (fields.size() < 2 || fields.front() != '{' || fields.back() != '}' ||
        fields.find('\n') != std::string_view::npos)
        throw std::invalid_argument("journal event fields must be a one-line JSON object");
    flush_if_due();
    auto next = reservations_;
    update_reservations(next, fields);
    extend_reservations(next);
    // Capture the receive timestamp on the owner; formatting belongs to the
    // writer. The byte ceiling includes the worst-case escaped event name.
    Record record{std::chrono::system_clock::now(), std::string(kind), std::string(fields),
                  fields.size() + kind.size() * 6 + 128};
    {
        std::lock_guard lock(queue_mutex_);
        if (!writer_error_.empty())
            throw std::runtime_error(writer_error_);
        if (record.bytes > queue_limit_ - std::min(queue_limit_, pending_bytes_) || pending_records_ == 65536) {
            writer_error_ = "interaction log queue capacity exceeded; record was not queued";
            throw std::runtime_error(writer_error_);
        }
        const auto bytes = record.bytes;
        queue_.push_back(std::move(record));
        pending_bytes_ += bytes;
        ++pending_records_;
    }
    reservations_ = next;
    queue_changed_.notify_one();
}
void EventJournal::extend_reservations(const JournalReservation& next) {
    auto reserved = high_water_;
    const auto block = reserve_block(next);
    if (next.next_ext_id > reserved.next_ext_id)
        reserved.next_ext_id = block.next_ext_id;
    if (next.next_user_id > reserved.next_user_id)
        reserved.next_user_id = block.next_user_id;
    if (reserved.next_ext_id == high_water_.next_ext_id && reserved.next_user_id == high_water_.next_user_id)
        return;
    // The ID checkpoint always uses the empty valid boundary. The writer's
    // separate checkpoint advances log recovery without replacing this ceiling.
    const auto record =
        checkpoint_record(device_, inode_, 0, reserved.next_ext_id, reserved.next_user_id, text_hash({}));
    write_checkpoint(state_path_, record + " " + std::to_string(text_hash(record)) + "\n");
    high_water_ = reserved;
}
void EventJournal::write_buffer() {
    std::size_t written{};
    while (written < buffered_.size()) {
        const auto count = ::write(fd_, buffered_.data() + written, buffered_.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            // A later best-effort flush must resume at the unwritten suffix.
            offset_ += written;
            buffered_.erase(0, written);
            throw std::runtime_error("interaction log write failed");
        }
        written += static_cast<std::size_t>(count);
    }
    offset_ += written;
    buffered_.clear();
}
void EventJournal::sync_buffer() {
    write_buffer();
    if (dirty_ && ::fsync(fd_) != 0)
        throw std::runtime_error("interaction log fsync failed");
    if (dirty_) {
        const auto boundary = boundary_hash(fd_, offset_);
        const auto record = checkpoint_record(device_, inode_, offset_, 1, 1, boundary);
        write_checkpoint(boundary_path_, record + " " + std::to_string(text_hash(record)) + "\n");
    }
    dirty_ = false;
    last_sync_ = std::chrono::steady_clock::now();
}
void EventJournal::flush_if_due() {
    std::lock_guard lock(queue_mutex_);
    if (!writer_error_.empty())
        throw std::runtime_error(writer_error_);
}
void EventJournal::request_recovery() {
    {
        std::lock_guard lock(queue_mutex_);
        if (recovery_pending_)
            return;
        recovery_pending_ = retry_requested_ = true;
        ++flush_requested_;
    }
    queue_changed_.notify_one();
}
bool EventJournal::recovery_pending() const {
    std::lock_guard lock(queue_mutex_);
    return recovery_pending_;
}
void EventJournal::flush() {
    std::unique_lock lock(queue_mutex_);
    const auto target = ++flush_requested_;
    retry_requested_ = true;
    queue_changed_.notify_one();
    queue_changed_.wait(lock, [&] { return flush_completed_ >= target; });
    if (!writer_error_.empty())
        throw std::runtime_error(writer_error_);
}
void EventJournal::writer_loop() noexcept {
    std::size_t batch_bytes{}, batch_records{};
    std::deque<Record> records;
    for (;;) {
        std::uint64_t target{};
        bool sync{}, stop{}, recovery{};
        {
            std::unique_lock lock(queue_mutex_);
            const auto ready = [&] {
                return stopping_ || retry_requested_ ||
                       (!io_failed_ && (!queue_.empty() || flush_requested_ > flush_completed_));
            };
            if (io_failed_)
                queue_changed_.wait(lock, ready);
            else
                queue_changed_.wait_until(lock, last_sync_ + std::chrono::milliseconds(250), ready);
            stop = stopping_;
            target = flush_requested_;
            recovery = retry_requested_;
            retry_requested_ = false;
            sync = stop || recovery || target > flush_completed_ ||
                   std::chrono::steady_clock::now() - last_sync_ >= std::chrono::milliseconds(250);
            if (buffered_.empty() && records.empty())
                records.swap(queue_);
        }
        try {
            while (!records.empty()) {
                const auto& record = records.front();
                buffered_ += "{\"utc\":" + json_string(stamp(record.wall, 0)) +
                             ",\"msk\":" + json_string(stamp(record.wall, 3)) +
                             ",\"event\":" + json_string(record.kind) + ",\"data\":" + record.fields + "}\n";
                batch_bytes += record.bytes;
                ++batch_records;
                records.pop_front();
            }
            if (!buffered_.empty()) {
                dirty_ = true;
                write_buffer();
                std::lock_guard lock(queue_mutex_);
                pending_bytes_ -= batch_bytes;
                pending_records_ -= batch_records;
                batch_bytes = batch_records = 0;
            }
            if (sync)
                sync_buffer();
            {
                std::lock_guard lock(queue_mutex_);
                io_failed_ = false;
                const bool drained = queue_.empty();
                if (!recovery || drained)
                    flush_completed_ = std::max(flush_completed_, target);
                if (recovery && !drained)
                    retry_requested_ = true;
                if (recovery && drained) {
                    writer_error_.clear();
                    recovery_pending_ = false;
                }
            }
            queue_changed_.notify_all();
            if (stop) {
                std::lock_guard lock(queue_mutex_);
                if (queue_.empty())
                    return;
            }
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(queue_mutex_);
                writer_error_ = error.what();
                io_failed_ = true;
                recovery_pending_ = false;
                flush_completed_ = flush_requested_;
            }
            queue_changed_.notify_all();
            if (stop)
                return;
        }
    }
}
} // namespace moex::connector_host
