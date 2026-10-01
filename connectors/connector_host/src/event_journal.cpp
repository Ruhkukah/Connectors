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

EventJournal::EventJournal(const std::filesystem::path& path, const std::filesystem::path& state_path)
    : state_path_(state_path.empty() ? std::filesystem::path(path.string() + ".state") : state_path) {
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
        struct stat info{};
        if (::fstat(fd_, &info) != 0)
            throw std::runtime_error("cannot stat interaction log");
        device_ = static_cast<std::uint64_t>(info.st_dev);
        inode_ = static_cast<std::uint64_t>(info.st_ino);
        if (std::filesystem::exists(state_path_)) {
            std::ifstream state(state_path_);
            std::string magic, extra;
            std::uint64_t device{}, inode{}, checkpoint{}, ext{}, user{}, hash{}, checksum{};
            if (!(state >> magic >> device >> inode >> checkpoint >> ext >> user >> hash >> checksum) ||
                magic != "MOEXJ2" || (state >> extra) || ext == 0 || ext > INT32_MAX || user == 0 ||
                user > UINT32_MAX ||
                checksum != text_hash(checkpoint_record(device, inode, checkpoint, ext, user, hash)))
                throw std::runtime_error("identity checkpoint is invalid; reconcile before restarting");
            reservations_ = {.next_ext_id = static_cast<std::int32_t>(ext),
                             .next_user_id = static_cast<std::uint32_t>(user)};
            // A new log filename retains the instance counters. Its own tail
            // starts at zero; the stable state lock prevents concurrent owners.
            if (device == device_ && inode == inode_) {
                if (checkpoint > static_cast<std::uint64_t>(info.st_size) || boundary_hash(fd_, checkpoint) != hash)
                    throw std::runtime_error("interaction log checkpoint mismatch; reconcile before restarting");
                offset_ = checkpoint;
            }
        }
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
        dirty_ = true;
        flush();
        last_sync_ = std::chrono::steady_clock::now();
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
    if (fd_ >= 0) {
        try {
            flush();
        } catch (...) {
        }
        ::close(fd_);
    }
    if (state_lock_fd_ >= 0)
        ::close(state_lock_fd_);
}
void EventJournal::append(std::string_view kind, std::string_view fields) {
    if (fields.size() < 2 || fields.front() != '{' || fields.back() != '}' ||
        fields.find('\n') != std::string_view::npos)
        throw std::invalid_argument("journal event fields must be a one-line JSON object");
    const auto wall = std::chrono::system_clock::now();
    const auto line = "{\"utc\":" + json_string(stamp(wall, 0)) + ",\"msk\":" + json_string(stamp(wall, 3)) +
                      ",\"event\":" + json_string(kind) + ",\"data\":" + std::string(fields) + "}\n";
    std::size_t offset{};
    while (offset < line.size()) {
        const auto count = ::write(fd_, line.data() + offset, line.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("interaction log write failed");
        offset += static_cast<std::size_t>(count);
    }
    offset_ += line.size();
    const auto before = reservations_;
    update_reservations(reservations_, fields);
    reservations_dirty_ |=
        before.next_ext_id != reservations_.next_ext_id || before.next_user_id != reservations_.next_user_id;
    dirty_ = true;
    if (std::chrono::steady_clock::now() - last_sync_ >= std::chrono::milliseconds(250))
        flush();
}
void EventJournal::flush() {
    if (dirty_ && ::fsync(fd_) != 0)
        throw std::runtime_error("interaction log fsync failed");
    if (dirty_) {
        const auto record = checkpoint_record(device_, inode_, offset_, reservations_.next_ext_id,
                                              reservations_.next_user_id, boundary_hash(fd_, offset_));
        write_checkpoint(state_path_, record + " " + std::to_string(text_hash(record)) + "\n");
    }
    dirty_ = false;
    reservations_dirty_ = false;
    last_sync_ = std::chrono::steady_clock::now();
}
void EventJournal::flush_reservations() {
    if (reservations_dirty_)
        flush();
}
} // namespace moex::connector_host
