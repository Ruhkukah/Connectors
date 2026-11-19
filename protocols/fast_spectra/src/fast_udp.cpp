#include "moex/fast/fast_udp.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

namespace moex::fast {

bool parse_endpoint(const std::string& text, UdpEndpoint& out) {
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0)
        return false;
    const std::size_t at = text.find('@', colon);
    const std::string port = text.substr(colon + 1, at == std::string::npos ? at : at - colon - 1);
    char* end = nullptr;
    const long value = std::strtol(port.c_str(), &end, 10);
    if (port.empty() || *end != '\0' || value <= 0 || value > 65535)
        return false;
    out.group = text.substr(0, colon);
    out.port = static_cast<std::uint16_t>(value);
    out.source = at == std::string::npos ? std::string{} : text.substr(at + 1);
    in_addr parsed{};
    return inet_pton(AF_INET, out.group.c_str(), &parsed) == 1 &&
           (out.source.empty() || inet_pton(AF_INET, out.source.c_str(), &parsed) == 1);
}

UdpReceiver::~UdpReceiver() {
    for (const pollfd& fd : fds_)
        ::close(fd.fd);
}

bool UdpReceiver::membership(int fd, const UdpEndpoint& endpoint, bool join) {
    in_addr group{}, source{}, interface{};
    interface.s_addr = htonl(INADDR_ANY);
    if (inet_pton(AF_INET, endpoint.group.c_str(), &group) != 1 ||
        (!endpoint.source.empty() && inet_pton(AF_INET, endpoint.source.c_str(), &source) != 1) ||
        (!endpoint.interface.empty() && inet_pton(AF_INET, endpoint.interface.c_str(), &interface) != 1)) {
        error_ = "invalid address in endpoint " + endpoint.group;
        return false;
    }
    int result;
    if (endpoint.source.empty()) {
        ip_mreq request{};
        request.imr_multiaddr = group;
        request.imr_interface = interface;
        result = ::setsockopt(fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &request,
                              sizeof(request));
    } else {
        ip_mreq_source request{};
        request.imr_multiaddr = group;
        request.imr_sourceaddr = source;
        request.imr_interface = interface;
        result = ::setsockopt(fd, IPPROTO_IP, join ? IP_ADD_SOURCE_MEMBERSHIP : IP_DROP_SOURCE_MEMBERSHIP, &request,
                              sizeof(request));
    }
    if (result != 0) {
        error_ = std::string(join ? "join " : "leave ") + endpoint.group + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

int UdpReceiver::add(const UdpEndpoint& endpoint) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        error_ = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    // Best effort: the kernel clamps this to net.core.rmem_max.
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &endpoint.receive_buffer_bytes, sizeof(endpoint.receive_buffer_bytes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
#ifdef __linux__
    // Binding to the group keeps other groups on the same port out of this socket.
    inet_pton(AF_INET, endpoint.group.c_str(), &address.sin_addr);
#else
    address.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || flags < 0 ||
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        error_ = "bind " + endpoint.group + ":" + std::to_string(endpoint.port) + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (!membership(fd, endpoint, true)) {
        ::close(fd);
        return -1;
    }
    fds_.push_back({fd, POLLIN, 0});
    sockets_.push_back({endpoint, true});
    return static_cast<int>(fds_.size() - 1);
}

bool UdpReceiver::set_joined(int index, bool joined) {
    Socket& socket = sockets_[static_cast<std::size_t>(index)];
    if (socket.joined == joined)
        return true;
    if (!membership(fds_[static_cast<std::size_t>(index)].fd, socket.endpoint, joined))
        return false;
    socket.joined = joined;
    return true;
}

} // namespace moex::fast
