#include "local_tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>

namespace moex::test {

namespace {

constexpr std::chrono::milliseconds kDefaultTimeout{3000};

void close_if_open(int& native_handle) noexcept {
    if (native_handle >= 0) {
        ::close(native_handle);
        native_handle = -1;
    }
}

} // namespace

LocalTcpServer::LocalTcpServer() {
    listen_socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_ < 0) {
        throw std::runtime_error("failed to create LocalTcpServer listen socket");
    }

    const int enable = 1;
    (void)::setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listen_socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close_if_open(listen_socket_);
        throw std::runtime_error("failed to bind LocalTcpServer to loopback");
    }
    if (::listen(listen_socket_, 1) != 0) {
        close_if_open(listen_socket_);
        throw std::runtime_error("failed to listen on LocalTcpServer");
    }

    socklen_t address_size = sizeof(address);
    if (::getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
        close_if_open(listen_socket_);
        throw std::runtime_error("failed to query LocalTcpServer port");
    }
    port_ = ntohs(address.sin_port);
    accept_thread_ = std::thread(&LocalTcpServer::accept_loop, this);
}

LocalTcpServer::~LocalTcpServer() {
    stop();
}

std::uint16_t LocalTcpServer::port() const noexcept {
    return port_;
}

bool LocalTcpServer::wait_for_client(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return client_connected_ || stop_requested_; }) &&
           client_connected_;
}

void LocalTcpServer::send_bytes(std::span<const std::byte> bytes, std::size_t max_chunk_size) {
    if (!wait_for_client(kDefaultTimeout)) {
        throw std::runtime_error("LocalTcpServer send_bytes timed out waiting for client");
    }

    const std::size_t chunk_limit = max_chunk_size == 0 ? 1 : max_chunk_size;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk_size = std::min(chunk_limit, bytes.size() - offset);
        int native_handle = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            native_handle = client_socket_;
        }
        if (native_handle < 0) {
            throw std::runtime_error("LocalTcpServer client socket is closed");
        }
        if (!wait_for_socket_writable(native_handle, kDefaultTimeout)) {
            throw std::runtime_error("LocalTcpServer timed out waiting for writable client socket");
        }

        const auto sent =
            ::send(native_handle, bytes.data() + static_cast<std::ptrdiff_t>(offset), static_cast<int>(chunk_size), 0);
        if (sent <= 0) {
            throw std::runtime_error("LocalTcpServer failed to send scripted bytes");
        }
        offset += static_cast<std::size_t>(sent);
    }
}

std::vector<std::byte> LocalTcpServer::receive_exact(std::size_t size, std::chrono::milliseconds timeout) {
    std::vector<std::byte> bytes;
    bytes.reserve(size);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (bytes.size() < size) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw std::runtime_error("LocalTcpServer timed out waiting for exact byte count");
        }
        const auto chunk =
            receive_up_to(size - bytes.size(), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    }
    return bytes;
}

std::vector<std::byte> LocalTcpServer::receive_up_to(std::size_t size, std::chrono::milliseconds timeout) {
    if (!wait_for_client(kDefaultTimeout)) {
        throw std::runtime_error("LocalTcpServer receive_up_to timed out waiting for client");
    }
    if (size == 0) {
        return {};
    }

    int native_handle = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        native_handle = client_socket_;
    }
    if (native_handle < 0) {
        return {};
    }
    if (!wait_for_socket_readable(native_handle, timeout)) {
        return {};
    }

    std::vector<std::byte> bytes(size);
    const auto received = ::recv(native_handle, bytes.data(), static_cast<int>(size), 0);
    if (received <= 0) {
        return {};
    }
    bytes.resize(static_cast<std::size_t>(received));
    return bytes;
}

void LocalTcpServer::close_client() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_socket_ >= 0) {
        ::shutdown(client_socket_, SHUT_RDWR);
    }
    close_if_open(client_socket_);
}

void LocalTcpServer::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
            return;
        }
        stop_requested_ = true;
    }
    condition_.notify_all();
    if (client_socket_ >= 0) {
        ::shutdown(client_socket_, SHUT_RDWR);
    }
    close_if_open(client_socket_);
    close_if_open(listen_socket_);
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void LocalTcpServer::accept_loop() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_) {
                return;
            }
        }

        sockaddr_in client_address{};
        socklen_t client_size = sizeof(client_address);
        const int accepted = ::accept(listen_socket_, reinterpret_cast<sockaddr*>(&client_address), &client_size);
        if (accepted < 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_) {
                return;
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            client_socket_ = accepted;
            client_connected_ = true;
        }
        condition_.notify_all();
        return;
    }
}

bool LocalTcpServer::wait_for_socket_readable(int native_handle, std::chrono::milliseconds timeout) const {
    pollfd descriptor{};
    descriptor.fd = native_handle;
    descriptor.events = POLLIN | POLLHUP | POLLERR;
    return ::poll(&descriptor, 1, static_cast<int>(timeout.count())) > 0;
}

bool LocalTcpServer::wait_for_socket_writable(int native_handle, std::chrono::milliseconds timeout) const {
    pollfd descriptor{};
    descriptor.fd = native_handle;
    descriptor.events = POLLOUT | POLLHUP | POLLERR;
    return ::poll(&descriptor, 1, static_cast<int>(timeout.count())) > 0;
}

} // namespace moex::test
