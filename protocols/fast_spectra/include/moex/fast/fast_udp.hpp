#pragma once

// Non-blocking UDP multicast receiver for the FAST feeds (POSIX sockets).
// One socket per group; poll() drains every ready socket into a caller buffer
// and hands each datagram to a callback, so there is no copy and no allocation
// per packet.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace moex::fast {

struct UdpEndpoint {
    std::string group;     // multicast address from configuration.xml <ip>
    std::uint16_t port{};  // <port>
    std::string source;    // <src-ip>; empty joins any source
    std::string interface; // local interface address; empty lets the kernel choose
    int receive_buffer_bytes{8 << 20};
};

// "group:port" or "group:port@source".
[[nodiscard]] bool parse_endpoint(const std::string& text, UdpEndpoint& out);

class UdpReceiver {
  public:
    UdpReceiver() = default;
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;
    ~UdpReceiver();

    // Opens and joins. Returns the endpoint index, or -1 with error() set.
    int add(const UdpEndpoint& endpoint);
    // Leaves or rejoins the group without closing the socket.
    bool set_joined(int index, bool joined);
    [[nodiscard]] bool joined(int index) const noexcept {
        return sockets_[static_cast<std::size_t>(index)].joined;
    }
    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

    // Waits up to timeout_ms (0 spins) and calls on_packet(index, data, size)
    // for every datagram available. Returns the number of datagrams handled.
    template <class Handler> int poll(int timeout_ms, Handler&& on_packet) {
        if (::poll(fds_.data(), static_cast<nfds_t>(fds_.size()), timeout_ms) <= 0)
            return 0;
        int handled = 0;
        for (std::size_t i = 0; i < fds_.size(); ++i) {
            if (!(fds_[i].revents & POLLIN))
                continue;
            for (int burst = 0; burst < kBurst; ++burst) {
                const ssize_t size = ::recv(fds_[i].fd, buffer_, sizeof(buffer_), MSG_DONTWAIT);
                if (size <= 0)
                    break;
                on_packet(static_cast<int>(i), static_cast<const std::uint8_t*>(buffer_),
                          static_cast<std::size_t>(size));
                ++handled;
            }
        }
        return handled;
    }

  private:
    struct Socket {
        UdpEndpoint endpoint;
        bool joined{};
    };
    static constexpr int kBurst = 64; // bound per socket so one busy feed cannot starve the others

    bool membership(int fd, const UdpEndpoint& endpoint, bool join);

    std::vector<pollfd> fds_;
    std::vector<Socket> sockets_;
    std::string error_;
    std::uint8_t buffer_[65536];
};

} // namespace moex::fast
