#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace moex::twime_trade::transport {

namespace {

constexpr int kPollTimeoutMs = 0;

bool is_would_block_error(int error_code) noexcept {
    return error_code == EAGAIN || error_code == EWOULDBLOCK;
}

TwimeTransportErrorCode map_connect_error(int error_code) {
    switch (error_code) {
    case 0:
        return TwimeTransportErrorCode::None;
    case EINPROGRESS:
        return TwimeTransportErrorCode::ConnectInProgress;
    case ECONNREFUSED:
        return TwimeTransportErrorCode::ConnectionRefused;
    default:
        if (is_would_block_error(error_code)) {
            return TwimeTransportErrorCode::ConnectInProgress;
        }
        return TwimeTransportErrorCode::ConnectFailed;
    }
}

TwimeTransportErrorCode map_read_error(int error_code) {
    switch (error_code) {
    case ECONNRESET:
        return TwimeTransportErrorCode::RemoteClosed;
    default:
        if (is_would_block_error(error_code)) {
            return TwimeTransportErrorCode::None;
        }
        return TwimeTransportErrorCode::ReadFault;
    }
}

TwimeTransportErrorCode map_write_error(int error_code) {
    switch (error_code) {
    case EPIPE:
    case ECONNRESET:
        return TwimeTransportErrorCode::RemoteClosed;
    default:
        if (is_would_block_error(error_code)) {
            return TwimeTransportErrorCode::None;
        }
        return TwimeTransportErrorCode::WriteFault;
    }
}

bool has_more_readable_bytes(int native_handle) {
    int pending = 0;
    if (::ioctl(native_handle, FIONREAD, &pending) != 0) {
        return false;
    }
    return pending > 0;
}

} // namespace

TwimeTcpTransport::TwimeTcpTransport(TwimeTcpConfig config) : config_(std::move(config)) {}

TwimeTransportResult TwimeTcpTransport::open() {
    ++metrics_.open_calls;
    if (state_ == TwimeTransportState::Open || state_ == TwimeTransportState::Opening) {
        return {
            .status = TwimeTransportStatus::InvalidState,
            .event = TwimeTransportEvent::Fault,
            .error_code = TwimeTransportErrorCode::InvalidState,
        };
    }

    const auto validation = validate_open_request();
    if (validation.status != TwimeTransportStatus::Ok) {
        return fail_open(validation.error_code, validation.os_error, validation.event);
    }

    const auto now_value = now();
    if (has_last_open_attempt_ && !twime_reconnect_allowed(now_value, last_open_attempt_, config_.reconnect_policy)) {
        state_ = TwimeTransportState::ReconnectBackoff;
        ++metrics_.reconnect_suppressed_events;
        return {
            .status = TwimeTransportStatus::WouldBlock,
            .event = TwimeTransportEvent::ReconnectSuppressed,
            .error_code = TwimeTransportErrorCode::ReconnectTooSoon,
        };
    }

    has_last_open_attempt_ = true;
    last_open_attempt_ = now_value;

    const int family = config_.endpoint.host == "::1" ? AF_INET6 : AF_INET;
    const int native_handle = ::socket(family, SOCK_STREAM, 0);
    if (native_handle < 0) {
        return fail_open(TwimeTransportErrorCode::SocketCreateFailed, errno);
    }

#ifdef SO_NOSIGPIPE
    {
        const int enable = 1;
        (void)::setsockopt(native_handle, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
    }
#endif

    const int flags = ::fcntl(native_handle, F_GETFL, 0);
    if (flags < 0 || ::fcntl(native_handle, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(native_handle);
        return fail_open(TwimeTransportErrorCode::SetNonBlockingFailed, errno);
    }

    socket_.reset(native_handle);
    state_ = TwimeTransportState::Opening;

    if (family == AF_INET6) {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(config_.endpoint.port);
        if (::inet_pton(AF_INET6, config_.endpoint.host.c_str(), &address.sin6_addr) != 1) {
            return fail_open(TwimeTransportErrorCode::InvalidConfiguration, 0);
        }
        if (::connect(native_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            state_ = TwimeTransportState::Open;
            ++metrics_.successful_open_events;
            return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::OpenSucceeded};
        }
    } else {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.endpoint.port);
        const std::string connect_host =
            config_.endpoint.host == "localhost" ? std::string("127.0.0.1") : config_.endpoint.host;
        if (::inet_pton(AF_INET, connect_host.c_str(), &address.sin_addr) != 1) {
            return fail_open(TwimeTransportErrorCode::InvalidConfiguration, 0);
        }
        if (::connect(native_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            state_ = TwimeTransportState::Open;
            ++metrics_.successful_open_events;
            return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::OpenSucceeded};
        }
    }

    const int connect_errno = errno;
    if (connect_errno == EINPROGRESS || is_would_block_error(connect_errno)) {
        return {
            .status = TwimeTransportStatus::WouldBlock,
            .event = TwimeTransportEvent::OpenStarted,
            .error_code = TwimeTransportErrorCode::ConnectInProgress,
            .os_error = connect_errno,
        };
    }

    return fail_open(map_connect_error(connect_errno), connect_errno);
}

TwimeTransportResult TwimeTcpTransport::close() {
    ++metrics_.close_calls;

    if (state_ == TwimeTransportState::Created || state_ == TwimeTransportState::Closed) {
        state_ = TwimeTransportState::Closed;
        return {.status = TwimeTransportStatus::Closed, .event = TwimeTransportEvent::LocalClose};
    }

    state_ = TwimeTransportState::Closing;
    socket_.close();
    state_ = TwimeTransportState::Closed;
    ++metrics_.local_close_events;
    return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::LocalClose};
}

TwimeTransportResult TwimeTcpTransport::write(std::span<const std::byte> bytes) {
    ++metrics_.write_calls;

    if (state_ == TwimeTransportState::Opening) {
        const auto connect_result = finalize_connect_from_poll();
        if (connect_result.event == TwimeTransportEvent::OpenSucceeded && state_ == TwimeTransportState::Open) {
            // continue into write
        } else {
            return connect_result;
        }
    }

    if (state_ != TwimeTransportState::Open) {
        return {
            .status = TwimeTransportStatus::InvalidState,
            .event = TwimeTransportEvent::Fault,
            .error_code = TwimeTransportErrorCode::InvalidState,
        };
    }
    if (bytes.empty()) {
        return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::BytesWritten};
    }

    const auto allowed = capped_write_size(bytes.size());
    if (allowed == 0) {
        ++metrics_.write_would_block_events;
        return {
            .status = TwimeTransportStatus::WouldBlock,
            .event = TwimeTransportEvent::WriteWouldBlock,
            .error_code = TwimeTransportErrorCode::BufferLimitExceeded,
        };
    }

    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif

    const auto sent = ::send(socket_.native_handle(), bytes.data(), static_cast<int>(allowed), flags);
    if (sent > 0) {
        const auto bytes_sent = static_cast<std::size_t>(sent);
        metrics_.bytes_written += bytes_sent;
        metrics_.max_write_buffer_depth = std::max(metrics_.max_write_buffer_depth, bytes_sent);
        if (bytes_sent < bytes.size()) {
            ++metrics_.partial_write_events;
            return {
                .status = TwimeTransportStatus::Ok,
                .event = TwimeTransportEvent::PartialWrite,
                .bytes_transferred = bytes_sent,
            };
        }
        return {
            .status = TwimeTransportStatus::Ok,
            .event = TwimeTransportEvent::BytesWritten,
            .bytes_transferred = bytes_sent,
        };
    }

    if (sent == 0) {
        ++metrics_.write_would_block_events;
        return {
            .status = TwimeTransportStatus::WouldBlock,
            .event = TwimeTransportEvent::WriteWouldBlock,
            .error_code = TwimeTransportErrorCode::WriteFault,
        };
    }

    const int write_errno = errno;
    if (is_would_block_error(write_errno)) {
        ++metrics_.write_would_block_events;
        return {
            .status = TwimeTransportStatus::WouldBlock,
            .event = TwimeTransportEvent::WriteWouldBlock,
            .error_code = TwimeTransportErrorCode::None,
            .os_error = write_errno,
        };
    }
    if (write_errno == EPIPE || write_errno == ECONNRESET) {
        socket_.close();
        state_ = TwimeTransportState::Closed;
        ++metrics_.remote_close_events;
        return {
            .status = TwimeTransportStatus::RemoteClosed,
            .event = TwimeTransportEvent::RemoteClose,
            .error_code = TwimeTransportErrorCode::RemoteClosed,
            .os_error = write_errno,
        };
    }

    state_ = TwimeTransportState::Faulted;
    ++metrics_.fault_events;
    return {
        .status = TwimeTransportStatus::Fault,
        .event = TwimeTransportEvent::Fault,
        .error_code = map_write_error(write_errno),
        .os_error = write_errno,
    };
}

TwimeTransportPollResult TwimeTcpTransport::poll_read(std::span<std::byte> out) {
    ++metrics_.read_calls;

    if (state_ == TwimeTransportState::Opening) {
        const auto connect_result = finalize_connect_from_poll();
        if (connect_result.event == TwimeTransportEvent::OpenSucceeded ||
            connect_result.status != TwimeTransportStatus::Ok) {
            return connect_result;
        }
    }

    if (state_ != TwimeTransportState::Open) {
        return {
            .status = TwimeTransportStatus::InvalidState,
            .event = TwimeTransportEvent::Fault,
            .error_code = TwimeTransportErrorCode::InvalidState,
        };
    }
    if (out.empty()) {
        return {
            .status = TwimeTransportStatus::BufferTooSmall,
            .event = TwimeTransportEvent::Fault,
            .error_code = TwimeTransportErrorCode::BufferLimitExceeded,
        };
    }

    const auto max_read = capped_read_size(out.size());
    if (max_read == 0) {
        return {
            .status = TwimeTransportStatus::BufferTooSmall,
            .event = TwimeTransportEvent::Fault,
            .error_code = TwimeTransportErrorCode::BufferLimitExceeded,
        };
    }

    pollfd descriptor{};
    descriptor.fd = socket_.native_handle();
    descriptor.events = POLLIN | POLLHUP | POLLERR;
    const int poll_result = ::poll(&descriptor, 1, kPollTimeoutMs);
    if (poll_result == 0) {
        return {
            .status = TwimeTransportStatus::WouldBlock,
            .event = TwimeTransportEvent::ReadWouldBlock,
            .error_code = TwimeTransportErrorCode::None,
        };
    }
    if (poll_result < 0) {
        state_ = TwimeTransportState::Faulted;
        ++metrics_.fault_events;
        return {
            .status = TwimeTransportStatus::Fault,
            .event = TwimeTransportEvent::Fault,
            .error_code = TwimeTransportErrorCode::ReadFault,
            .os_error = errno,
        };
    }
    if ((descriptor.revents & POLLHUP) != 0 && (descriptor.revents & POLLIN) == 0) {
        socket_.close();
        state_ = TwimeTransportState::Closed;
        ++metrics_.remote_close_events;
        return {
            .status = TwimeTransportStatus::RemoteClosed,
            .event = TwimeTransportEvent::RemoteClose,
            .error_code = TwimeTransportErrorCode::RemoteClosed,
        };
    }
    if ((descriptor.revents & POLLERR) != 0 && (descriptor.revents & POLLIN) == 0) {
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        (void)::getsockopt(socket_.native_handle(), SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size);
        if (socket_error == ECONNRESET || socket_error == 0) {
            socket_.close();
            state_ = TwimeTransportState::Closed;
            ++metrics_.remote_close_events;
            return {
                .status = TwimeTransportStatus::RemoteClosed,
                .event = TwimeTransportEvent::RemoteClose,
                .error_code = TwimeTransportErrorCode::RemoteClosed,
                .os_error = socket_error,
            };
        }
        state_ = TwimeTransportState::Faulted;
        ++metrics_.fault_events;
        return {
            .status = TwimeTransportStatus::Fault,
            .event = TwimeTransportEvent::Fault,
            .error_code = map_read_error(socket_error),
            .os_error = socket_error,
        };
    }

    const auto received = ::recv(socket_.native_handle(), out.data(), static_cast<int>(max_read), 0);
    if (received > 0) {
        const auto bytes_read = static_cast<std::size_t>(received);
        metrics_.bytes_read += bytes_read;
        metrics_.max_read_buffer_depth = std::max(metrics_.max_read_buffer_depth, bytes_read);
        if ((bytes_read < out.size() && bytes_read == max_read) || has_more_readable_bytes(socket_.native_handle())) {
            ++metrics_.partial_read_events;
            return {
                .status = TwimeTransportStatus::Ok,
                .event = TwimeTransportEvent::PartialRead,
                .bytes_transferred = bytes_read,
            };
        }
        return {
            .status = TwimeTransportStatus::Ok,
            .event = TwimeTransportEvent::BytesRead,
            .bytes_transferred = bytes_read,
        };
    }

    if (received == 0) {
        socket_.close();
        state_ = TwimeTransportState::Closed;
        ++metrics_.remote_close_events;
        return {
            .status = TwimeTransportStatus::RemoteClosed,
            .event = TwimeTransportEvent::RemoteClose,
            .error_code = TwimeTransportErrorCode::RemoteClosed,
        };
    }

    const int read_errno = errno;
    if (is_would_block_error(read_errno)) {
        return {
            .status = TwimeTransportStatus::WouldBlock,
            .event = TwimeTransportEvent::ReadWouldBlock,
            .error_code = TwimeTransportErrorCode::None,
            .os_error = read_errno,
        };
    }
    if (read_errno == ECONNRESET) {
        socket_.close();
        state_ = TwimeTransportState::Closed;
        ++metrics_.remote_close_events;
        return {
            .status = TwimeTransportStatus::RemoteClosed,
            .event = TwimeTransportEvent::RemoteClose,
            .error_code = TwimeTransportErrorCode::RemoteClosed,
            .os_error = read_errno,
        };
    }

    state_ = TwimeTransportState::Faulted;
    ++metrics_.fault_events;
    return {
        .status = TwimeTransportStatus::Fault,
        .event = TwimeTransportEvent::Fault,
        .error_code = map_read_error(read_errno),
        .os_error = read_errno,
    };
}

TwimeTransportState TwimeTcpTransport::state() const noexcept {
    return state_;
}

TwimeTransportMetrics TwimeTcpTransport::metrics() const noexcept {
    return metrics_;
}

const TwimeTcpConfig& TwimeTcpTransport::config() const noexcept {
    return config_;
}

void TwimeTcpTransport::set_time_source(std::function<std::chrono::steady_clock::time_point()> time_source) {
    time_source_ = std::move(time_source);
}

TwimeTcpTransport::TimePoint TwimeTcpTransport::now() const {
    if (time_source_) {
        return time_source_();
    }
    return std::chrono::steady_clock::now();
}

TwimeTransportResult TwimeTcpTransport::fail_open(TwimeTransportErrorCode error_code, int os_error,
                                                  TwimeTransportEvent event) {
    socket_.close();
    state_ = TwimeTransportState::Faulted;
    ++metrics_.open_failed_events;
    if (event == TwimeTransportEvent::Fault) {
        ++metrics_.fault_events;
    }
    return {
        .status = TwimeTransportStatus::Fault,
        .event = event,
        .error_code = error_code,
        .os_error = os_error,
    };
}

TwimeTransportResult TwimeTcpTransport::validate_open_request() const {
    if (config_.environment != TwimeTcpEnvironment::Test) {
        return {
            .status = TwimeTransportStatus::Fault,
            .event = TwimeTransportEvent::OpenFailed,
            .error_code = TwimeTransportErrorCode::EnvironmentBlocked,
        };
    }
    if (config_.endpoint.port == 0) {
        return {
            .status = TwimeTransportStatus::Fault,
            .event = TwimeTransportEvent::OpenFailed,
            .error_code = TwimeTransportErrorCode::InvalidConfiguration,
        };
    }

    const bool explicit_loopback = twime_is_explicit_loopback_host(config_.endpoint.host);
    if (!explicit_loopback) {
        return {
            .status = TwimeTransportStatus::Fault,
            .event = TwimeTransportEvent::OpenFailed,
            .error_code = TwimeTransportErrorCode::LocalOnlyViolation,
        };
    }

    return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::OpenStarted};
}

TwimeTransportResult TwimeTcpTransport::complete_nonblocking_connect() {
    pollfd descriptor{};
    descriptor.fd = socket_.native_handle();
    descriptor.events = POLLOUT | POLLERR | POLLHUP;

    const int poll_result = ::poll(&descriptor, 1, kPollTimeoutMs);
    if (poll_result == 0) {
        return {
            .status = TwimeTransportStatus::WouldBlock,
            .event = TwimeTransportEvent::OpenStarted,
            .error_code = TwimeTransportErrorCode::ConnectInProgress,
        };
    }
    if (poll_result < 0) {
        return fail_open(TwimeTransportErrorCode::ConnectFailed, errno, TwimeTransportEvent::OpenFailed);
    }

    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    if (::getsockopt(socket_.native_handle(), SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0) {
        return fail_open(TwimeTransportErrorCode::ConnectFailed, errno, TwimeTransportEvent::OpenFailed);
    }
    if (socket_error != 0) {
        return fail_open(map_connect_error(socket_error), socket_error, TwimeTransportEvent::OpenFailed);
    }

    state_ = TwimeTransportState::Open;
    ++metrics_.successful_open_events;
    return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::OpenSucceeded};
}

TwimeTransportResult TwimeTcpTransport::finalize_connect_from_poll() {
    if (state_ != TwimeTransportState::Opening) {
        return {
            .status = TwimeTransportStatus::InvalidState,
            .event = TwimeTransportEvent::Fault,
            .error_code = TwimeTransportErrorCode::InvalidState,
        };
    }

    return complete_nonblocking_connect();
}

std::size_t TwimeTcpTransport::capped_read_size(std::size_t requested) const noexcept {
    return std::min({requested, config_.buffer_policy.read_chunk_bytes, config_.buffer_policy.max_inbound_bytes});
}

std::size_t TwimeTcpTransport::capped_write_size(std::size_t requested) const noexcept {
    return std::min({requested, config_.buffer_policy.write_chunk_bytes, config_.buffer_policy.max_outbound_bytes});
}

} // namespace moex::twime_trade::transport
