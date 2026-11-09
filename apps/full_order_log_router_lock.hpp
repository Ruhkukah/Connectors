#pragma once

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string_view>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace moex::connector_host {
// Port-based on purpose: localhost aliases must contend for the same lock.
// Every native FOL entry point uses the same machine-local directory.
class FullOrderLogRouterLock {
  public:
    explicit FullOrderLogRouterLock(std::string_view connection, const std::filesystem::path& directory = "/tmp") {
        constexpr std::string_view scheme = "p2tcp://";
        const auto endpoint = connection.substr(0, connection.find(';'));
        const auto colon = endpoint.rfind(':');
        if (!endpoint.starts_with(scheme) || colon == endpoint.npos || colon <= scheme.size())
            throw std::invalid_argument("invalid Full Order Log router endpoint");
        const auto digits = endpoint.substr(colon + 1);
        std::uint16_t port{};
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
        if (error != std::errc{} || end != digits.data() + digits.size() || !port)
            throw std::invalid_argument("invalid Full Order Log router port");
        const auto path = directory / ("moex-fol-router-" + std::to_string(port) + ".lock");
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (fd_ < 0)
            throw std::runtime_error("cannot open Full Order Log router lock");
        struct stat info {};
        if (::fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1 ||
            (info.st_mode & (S_IRWXG | S_IRWXO))) {
            ::close(fd_);
            throw std::runtime_error("Full Order Log router lock is not a private owned regular file");
        }
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            const auto locked = errno == EWOULDBLOCK || errno == EAGAIN;
            ::close(fd_);
            throw std::runtime_error(locked ? "Full Order Log router already in use"
                                            : "cannot lock Full Order Log router");
        }
        struct stat current {};
        if (::lstat(path.c_str(), &current) != 0 || current.st_dev != info.st_dev || current.st_ino != info.st_ino) {
            ::close(fd_);
            throw std::runtime_error("Full Order Log router lock changed during acquisition");
        }
    }
    ~FullOrderLogRouterLock() {
        ::close(fd_);
    }
    FullOrderLogRouterLock(const FullOrderLogRouterLock&) = delete;
    FullOrderLogRouterLock& operator=(const FullOrderLogRouterLock&) = delete;

  private:
    int fd_;
};
} // namespace moex::connector_host
