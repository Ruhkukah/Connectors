#pragma once

#include "scope_exit.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace moex::connector_host {
namespace command_socket_detail {
inline constexpr std::size_t max_command = 65536;
inline constexpr auto timeout = std::chrono::seconds(5);

inline sockaddr_un address(const std::filesystem::path& path) {
    sockaddr_un out{};
    out.sun_family = AF_UNIX;
    const auto name = path.string();
    if (name.size() >= sizeof(out.sun_path))
        throw std::invalid_argument("command socket path too long; use --command-socket with a shorter path");
    std::memcpy(out.sun_path, name.c_str(), name.size() + 1);
    return out;
}
inline void nonblocking(int fd) {
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
        throw std::runtime_error("cannot configure command socket");
}
inline void wait(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd descriptor{.fd = fd, .events = events};
        if (::poll(&descriptor, 1, static_cast<int>(remaining.count()) + 1) > 0) {
            if (descriptor.revents & events)
                return;
            throw std::runtime_error("command socket disconnected");
        }
        if (errno != EINTR)
            break;
    }
    throw std::runtime_error("command socket timed out");
}
} // namespace command_socket_detail

// The host calls poll on its CGate owner thread. A disconnected/idle client cannot block replication.
class CommandSocket {
  public:
    explicit CommandSocket(std::filesystem::path path) : path_(std::move(path)) {
        const auto address = command_socket_detail::address(path_);
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0)
            throw std::runtime_error("cannot create command socket");
        ScopeExit failed([&] {
            ::close(fd_);
            if (bound_)
                ::unlink(path_.c_str());
        });
        command_socket_detail::nonblocking(fd_);
        // An existing endpoint is never removed or taken over, including after an unclean exit.
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
            throw std::runtime_error(std::string("cannot bind command socket: ") + std::strerror(errno) +
                                     " (remove a stale endpoint only after verifying its owner)");
        bound_ = true;
        if (::chmod(path_.c_str(), 0600) < 0 || ::lstat(path_.c_str(), &identity_) < 0 || ::listen(fd_, 4) < 0)
            throw std::runtime_error("cannot secure command socket");
        failed.release();
    }
    ~CommandSocket() {
        close_client();
        ::close(fd_);
        struct stat current {};
        if (::lstat(path_.c_str(), &current) == 0 && current.st_dev == identity_.st_dev &&
            current.st_ino == identity_.st_ino)
            ::unlink(path_.c_str());
    }
    CommandSocket(const CommandSocket&) = delete;
    CommandSocket& operator=(const CommandSocket&) = delete;

    template <typename Execute, typename Refuse> void poll(Execute execute, Refuse refuse) {
        if (client_ < 0) {
            client_ = ::accept(fd_, nullptr, nullptr);
            if (client_ < 0)
                return;
            try {
                command_socket_detail::nonblocking(client_);
            } catch (...) {
                close_client();
                throw;
            }
            deadline_ = std::chrono::steady_clock::now() + command_socket_detail::timeout;
        }
        // Keep malformed frames bounded in the interaction log.
        const auto rejected = [&](std::string_view error) {
            return refuse(input_.size() <= 4096 ? std::string_view(input_) : std::string_view{}, error);
        };
        if (std::chrono::steady_clock::now() >= deadline_) {
            if (!input_.empty() && response_.empty())
                (void)rejected("incomplete command timed out");
            close_client();
            return;
        }
        if (response_.empty()) {
            std::array<char, 4096> data{};
            const auto count = ::recv(client_, data.data(), data.size(), 0);
            if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                if (!input_.empty())
                    (void)rejected("incomplete command disconnected");
                close_client();
                return;
            }
            if (count < 0)
                return;
            input_.append(data.data(), static_cast<std::size_t>(count));
            const auto newline = input_.find('\n');
            if (input_.size() > command_socket_detail::max_command + 1)
                response_ = rejected("command line exceeds 65536 bytes");
            else if (newline != std::string::npos) {
                if (newline != input_.size() - 1 || newline == 0)
                    response_ = rejected("one nonempty command required");
                else
                    response_ = execute(input_.substr(0, newline));
            } else
                return;
            response_ += '\n';
        }
        const auto count = ::send(client_, response_.data() + sent_, response_.size() - sent_, 0);
        if (count > 0) {
            sent_ += static_cast<std::size_t>(count);
            if (sent_ == response_.size())
                close_client();
        } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            close_client();
    }

  private:
    void close_client() {
        if (client_ >= 0)
            ::close(client_);
        client_ = -1;
        input_.clear();
        response_.clear();
        sent_ = 0;
    }
    std::filesystem::path path_;
    int fd_{-1}, client_{-1};
    bool bound_{};
    struct stat identity_ {};
    std::chrono::steady_clock::time_point deadline_{};
    std::string input_, response_;
    std::size_t sent_{};
};

inline std::string send_command(const std::filesystem::path& path, std::string command) {
    using namespace command_socket_detail;
    if (command.empty() || command.size() > max_command || command.find('\n') != std::string::npos)
        throw std::invalid_argument("one command of at most 65536 bytes required");
    const auto endpoint = address(path);
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        throw std::runtime_error("cannot create command client");
    ScopeExit close([&] { ::close(fd); });
    nonblocking(fd);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0) {
        if (errno != EINPROGRESS)
            throw std::runtime_error("cannot connect to command socket");
        wait(fd, POLLOUT, deadline);
        int error{};
        socklen_t size = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
            throw std::runtime_error("cannot connect to command socket");
    }
    command += '\n';
    std::size_t sent{};
    while (sent != command.size()) {
        wait(fd, POLLOUT, deadline);
        const auto count = ::send(fd, command.data() + sent, command.size() - sent, 0);
        if (count > 0)
            sent += static_cast<std::size_t>(count);
        else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            throw std::runtime_error("command socket write failed");
    }
    std::string response;
    for (;;) {
        wait(fd, POLLIN, deadline);
        std::array<char, 4096> data{};
        const auto count = ::recv(fd, data.data(), data.size(), 0);
        if (count > 0) {
            response.append(data.data(), static_cast<std::size_t>(count));
            if (const auto newline = response.find('\n'); newline != std::string::npos)
                return response.substr(0, newline);
        } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            throw std::runtime_error("command socket response missing");
    }
}
} // namespace moex::connector_host
