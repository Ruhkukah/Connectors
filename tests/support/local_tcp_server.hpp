#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace moex::test {

class LocalTcpServer {
  public:
    LocalTcpServer();
    LocalTcpServer(const LocalTcpServer&) = delete;
    LocalTcpServer& operator=(const LocalTcpServer&) = delete;
    ~LocalTcpServer();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] bool wait_for_client(std::chrono::milliseconds timeout);

    void send_bytes(std::span<const std::byte> bytes, std::size_t max_chunk_size = static_cast<std::size_t>(-1));
    [[nodiscard]] std::vector<std::byte> receive_exact(std::size_t size, std::chrono::milliseconds timeout);
    [[nodiscard]] std::vector<std::byte> receive_up_to(std::size_t size, std::chrono::milliseconds timeout);

    void close_client();
    void stop();

  private:
    void accept_loop();
    [[nodiscard]] bool wait_for_socket_readable(int native_handle, std::chrono::milliseconds timeout) const;
    [[nodiscard]] bool wait_for_socket_writable(int native_handle, std::chrono::milliseconds timeout) const;

    int listen_socket_{-1};
    int client_socket_{-1};
    std::uint16_t port_{0};
    bool stop_requested_{false};
    bool client_connected_{false};
    std::thread accept_thread_{};
    mutable std::mutex mutex_{};
    std::condition_variable condition_{};
};

} // namespace moex::test
