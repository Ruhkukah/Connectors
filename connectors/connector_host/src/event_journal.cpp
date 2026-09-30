#include "moex/connector_host/event_journal.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

namespace moex::connector_host {
namespace {
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
void check_logging(std::string_view text) {
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return std::tolower(ch); });
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return std::tolower(ch); });
        if ((key == "log" && value != "p2:p2syslog") || key == "minloglevel" || key == "loglevel" ||
            ((key == "logging" || key == "enabled") && (value == "0" || value == "false" || value == "off")))
            throw std::invalid_argument("CGate default logging must remain enabled; remove logging/severity overrides");
    }
}
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
} // namespace

std::string json_string(std::string_view text) {
    return plaza2::cgate::text::json_quote_utf8(text);
}

void validate_cgate_logging(std::string_view settings, const std::filesystem::path& config_dir) {
    std::string overrides(settings);
    std::replace(overrides.begin(), overrides.end(), ';', '\n');
    check_logging(overrides);
    const auto start = settings.find("ini=");
    if (start == std::string_view::npos)
        throw std::invalid_argument("CGate env settings require an ini with default logging");
    const auto path = settings.substr(start + 4, settings.find(';', start) - (start + 4));
    auto ini = std::filesystem::path(path);
    if (!ini.is_absolute() && !config_dir.empty()) {
        const auto nested = config_dir / ini;
        ini = std::filesystem::exists(nested) ? nested : config_dir / ini.filename();
    }
    std::ifstream file{ini};
    if (!file)
        throw std::invalid_argument("cannot read CGate logging ini");
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    check_logging(contents);
    if (contents.find("[p2syslog]") == std::string::npos)
        throw std::invalid_argument("CGate ini must contain the default [p2syslog] section");
}

EventJournal::EventJournal(const std::filesystem::path& path) {
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
        std::ifstream file(path);
        std::string line;
        std::uint64_t complete{};
        while (std::getline(file, line)) {
            // A partial final record cannot be a restart hint. Recover the
            // complete NDJSON prefix before adding another line.
            if (file.eof())
                break;
            complete += line.size() + 1;
            update_reservations(reservations_, line);
        }
        if (::ftruncate(fd_, static_cast<off_t>(complete)) != 0)
            throw std::runtime_error("cannot recover interaction log tail");
        last_sync_ = std::chrono::steady_clock::now();
    } catch (...) {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}
EventJournal::~EventJournal() {
    if (fd_ >= 0) {
        if (dirty_)
            ::fsync(fd_);
        ::close(fd_);
    }
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
    dirty_ = false;
    reservations_dirty_ = false;
    last_sync_ = std::chrono::steady_clock::now();
}
void EventJournal::flush_reservations() {
    if (reservations_dirty_)
        flush();
}
} // namespace moex::connector_host
