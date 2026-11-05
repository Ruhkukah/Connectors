#pragma once

#include "moex/connector_host/trading_host.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace moex::connector_host {

// The owner admits at most one Add at a time. The worker owns disk writes;
// only the owner publishes successfully persisted identity into its maps.
class FullOrderLogDtcIdentity {
  public:
    struct Identity {
        std::string client_id;
        bool automated{};
    };
    struct Result {
        std::int32_t ext_id{};
        bool persisted{};
    };

    explicit FullOrderLogDtcIdentity(const TradingHostConfig& config)
        : isin_(config.isin_ids.front()), ext_begin_(config.orders.ext_id_begin), ext_end_(config.orders.ext_id_end),
          account_(config.orders.broker_code + config.orders.client_code), login_(config.orders.login_from) {
        auto path = config.identity_state_path;
        if (path.empty())
            path = config.journal_path.parent_path() / (config.session.publisher_name + ".state");
        path_ = path.string() + ".dtc";
    }
    ~FullOrderLogDtcIdentity() {
        request_stop();
        join();
        if (fd_ >= 0)
            ::close(fd_);
    }
    void start() {
        const auto parent = path_.parent_path().empty() ? std::filesystem::path(".") : path_.parent_path();
        if (::mkdir(path_.c_str(), 0700) != 0 && errno != EEXIST)
            throw std::runtime_error("cannot create DTC identity ledger");
        const auto parent_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const bool parent_synced = parent_fd >= 0 && ::fsync(parent_fd) == 0;
        if (parent_fd >= 0)
            ::close(parent_fd);
        fd_ = ::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat info {};
        if (!parent_synced || fd_ < 0 || ::fstat(fd_, &info) != 0 || info.st_uid != ::geteuid() ||
            (info.st_mode & 0777) != 0700)
            throw std::runtime_error("DTC identity ledger must be owned and mode 0700");
        for (const auto& entry : std::filesystem::directory_iterator(path_))
            load(entry.path().filename().string());
        worker_ = std::thread([this] { work(); });
    }
    [[nodiscard]] bool contains(std::string_view id) const {
        return keys_.contains(id);
    }
    [[nodiscard]] const Identity* by_ext(std::int32_t ext) const {
        const auto entry = owned_.find(ext);
        return entry == owned_.end() ? nullptr : &entry->second;
    }
    [[nodiscard]] std::string submit(std::string_view key, std::int32_t ext, bool automated) {
        if (!key.starts_with("dtc:") || !valid_id(key.substr(4)) || ext < ext_begin_ || ext > ext_end_)
            return "invalid DTC identity";
        auto id = std::string(key.substr(4));
        if (keys_.contains(id) || reserved_.contains(ext) || keys_.size() >= capacity_)
            return "DTC identity already reserved or ledger full";
        auto record = std::to_string(ext) + '\n' + std::to_string(isin_) + '\n' + account_ + '\n' + login_ +
                      (automated ? "\nR\n" : "\nM\n");
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || failed_ || busy_)
                return "DTC identity persistence is unavailable";
            job_.emplace(Job{ext, {std::move(id), automated}, std::move(record)});
            busy_ = true;
        }
        changed_.notify_one();
        return {};
    }
    [[nodiscard]] std::optional<Result> take() {
        std::optional<Completion> result;
        {
            std::lock_guard lock(mutex_);
            if (!completion_)
                return std::nullopt;
            result = std::move(completion_);
            completion_.reset();
            busy_ = false;
        }
        if (result->persisted) {
            keys_.insert(result->job.identity.client_id);
            reserved_.insert(result->job.ext);
            owned_.emplace(result->job.ext, std::move(result->job.identity));
        }
        return Result{result->job.ext, result->persisted};
    }
    void request_stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            job_.reset();
        }
        changed_.notify_one();
    }
    void join() noexcept {
        if (worker_.joinable())
            worker_.join();
    }

  private:
    struct Job {
        std::int32_t ext;
        Identity identity;
        std::string record;
    };
    struct Completion {
        Job job;
        bool persisted;
    };
    static constexpr std::size_t capacity_ = 65536;
    std::int32_t isin_, ext_begin_, ext_end_;
    std::string account_, login_;
    std::filesystem::path path_;
    int fd_{-1};
    std::set<std::string, std::less<>> keys_;
    std::set<std::int32_t> reserved_;
    std::map<std::int32_t, Identity> owned_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Job> job_;
    std::optional<Completion> completion_;
    bool busy_{}, stopping_{}, failed_{};
    std::thread worker_;

    static bool valid_id(std::string_view id) {
        return !id.empty() && id.size() <= 64 &&
               id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
                   std::string_view::npos;
    }
    static std::int32_t parse_id(std::string_view text) {
        std::int32_t result{};
        const auto parse = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parse.ec != std::errc{} || parse.ptr != text.data() + text.size() || result <= 0)
            throw std::runtime_error("invalid DTC identity record ID");
        return result;
    }
    void load(std::string id) {
        if (!valid_id(id) || keys_.size() >= capacity_)
            throw std::runtime_error("invalid DTC identity ledger entry");
        const auto fd = ::openat(fd_, id.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat info {};
        std::array<char, 256> bytes{};
        const bool valid = fd >= 0 && ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
                           (info.st_mode & 0777) == 0600;
        const auto size = valid ? ::read(fd, bytes.data(), bytes.size()) : -1;
        if (fd >= 0)
            ::close(fd);
        if (size <= 0 || size >= static_cast<ssize_t>(bytes.size()))
            throw std::runtime_error("invalid or incomplete DTC identity record");

        std::string_view rest(bytes.data(), static_cast<std::size_t>(size));
        const auto field = [&] {
            const auto end = rest.find('\n');
            if (end == std::string_view::npos || !end)
                throw std::runtime_error("invalid or incomplete DTC identity record");
            const auto result = rest.substr(0, end);
            rest.remove_prefix(end + 1);
            return result;
        };
        const auto ext = parse_id(field());
        const auto isin = parse_id(field());
        const auto account = field();
        const auto login = field();
        const auto compliance = field();
        if (!rest.empty() || (compliance != "R" && compliance != "M") || !reserved_.insert(ext).second)
            throw std::runtime_error("invalid or conflicting DTC identity record");
        keys_.insert(id);
        if (isin == isin_ && ext >= ext_begin_ && ext <= ext_end_ && account == account_ && login == login_)
            owned_.emplace(ext, Identity{std::move(id), compliance == "R"});
    }
    bool persist(const Job& job) const noexcept {
        const auto& id = job.identity.client_id;
        const auto fd = ::openat(fd_, id.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return false;
        std::string_view remaining = job.record;
        while (!remaining.empty()) {
            const auto count = ::write(fd, remaining.data(), remaining.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            remaining.remove_prefix(static_cast<std::size_t>(count));
        }
        const bool file_synced = remaining.empty() && ::fsync(fd) == 0;
        const bool closed = ::close(fd) == 0;
        if (file_synced && closed && ::fsync(fd_) == 0)
            return true;
        if (::unlinkat(fd_, id.c_str(), 0) == 0)
            (void)::fsync(fd_);
        return false;
    }
    void work() noexcept {
        for (;;) {
            std::optional<Job> job;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stopping_ || job_.has_value(); });
                if (stopping_)
                    return;
                job = std::move(job_);
                job_.reset();
            }
            const bool persisted = persist(*job);
            {
                std::lock_guard lock(mutex_);
                failed_ |= !persisted;
                completion_.emplace(Completion{std::move(*job), persisted});
            }
        }
    }
};

} // namespace moex::connector_host
