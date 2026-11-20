#include "moex/fast/asts/asts_replay_client.hpp"

#include "asts_messages.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace moex::fast::asts {

namespace {

constexpr char kSoh = '\x01';
constexpr std::size_t kMaxFrame = 1 << 16;

// SendingTime as the gate specifies it for TCP replay too: yyMMDDHHmmSSuuuuuu, UTC.
std::string sending_time() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1'000'000;
    std::tm utc{};
    ::gmtime_r(&seconds, &utc);
    char text[32];
    std::snprintf(text, sizeof(text), "%02d%02d%02d%02d%02d%02d%06lld", utc.tm_year % 100, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<long long>(micros));
    return text;
}

std::uint32_t load32(const std::uint8_t* p, bool little) {
    return little ? std::uint32_t{p[0]} | std::uint32_t{p[1]} << 8 | std::uint32_t{p[2]} << 16 | std::uint32_t{p[3]} << 24
                  : std::uint32_t{p[3]} | std::uint32_t{p[2]} << 8 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[0]} << 24;
}

// Template id and MsgSeqNum of a FAST message, or false if it does not start like one.
// MsgSeqNum is the first transmitted field of every template the replay channels carry.
bool peek(const std::uint8_t* data, std::size_t size, std::uint32_t& id, std::uint32_t& seq) {
    Reader r{data, data + size};
    std::uint64_t pmap;
    id = read_header(r, pmap);
    if (id == 0 || template_name(id).empty())
        return false;
    if (id == Logon::kTemplateId || id == Logout::kTemplateId)
        static_cast<void>(read_ascii(r)); // TargetCompID precedes MsgSeqNum in the session templates
    seq = read_u32(r);
    return r.ok;
}

} // namespace

ReplayClient::~ReplayClient() {
    close_socket();
}

void ReplayClient::close_socket() noexcept {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}

ReplayClient::State ReplayClient::fail(std::string reason) {
    error_ = std::move(reason);
    // Leave politely even when giving up: the gate counts sessions that end without a logout.
    if (fd_ >= 0 && state_ != State::Connecting && !logout_sent_)
        logout_sent_ = send_all(fix_message("5", state_ == State::Streaming ? 3 : 2, {}));
    close_socket();
    return state_ = State::Failed;
}

std::string ReplayClient::frame_fix(std::string_view body) {
    std::string message = "8=FIXT.1.1";
    message += kSoh;
    message += "9=" + std::to_string(body.size());
    message += kSoh;
    message += body;
    unsigned sum = 0;
    for (const char c : message)
        sum += static_cast<unsigned char>(c);
    char trailer[16];
    std::snprintf(trailer, sizeof(trailer), "10=%03u%c", sum % 256, kSoh);
    return message + trailer;
}

std::string ReplayClient::fix_message(std::string_view type, std::uint32_t seq, std::string_view fields) const {
    std::string body = "35=" + std::string(type) + kSoh;
    if (type == "V") // ApplVerID sits next to MsgType on application messages
        body += std::string("1128=9") + kSoh;
    body += "49=" + config_.sender + kSoh + "56=" + config_.target + kSoh + "34=" + std::to_string(seq) + kSoh +
            "52=" + sending_time() + kSoh;
    body += fields;
    return frame_fix(body);
}

bool ReplayClient::start(std::string_view channel, std::uint32_t first_seq, std::uint32_t last_seq,
                         std::uint64_t now_ns) {
    if (busy()) {
        error_ = "a replay request is already in flight";
        return false;
    }
    if (last_seq < first_seq || last_seq - first_seq >= config_.max_messages) {
        error_ = "range " + std::to_string(first_seq) + ".." + std::to_string(last_seq) + " is empty or exceeds " +
                 std::to_string(config_.max_messages) + " messages";
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
        error_ = "invalid replay host " + config_.host;
        return false;
    }
    close_socket();
    fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ < 0) {
        error_ = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    const int on = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0 ||
        (::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 && errno != EINPROGRESS)) {
        error_ = "connect " + config_.host + ":" + std::to_string(config_.port) + ": " + std::strerror(errno);
        close_socket();
        return false;
    }
    ++connections_;
    channel_ = channel;
    first_ = next_ = first_seq;
    last_ = last_seq;
    deadline_ns_ = now_ns + config_.timeout_ns;
    buffer_.resize(kMaxFrame + 8);
    buffered_ = 0;
    length_order_ = 0;
    message_offset_ = -1;
    logout_sent_ = false;
    error_.clear();
    state_ = State::Connecting;
    return true;
}

bool ReplayClient::send_all(const std::string& text) {
    // These messages are a few hundred bytes into an empty socket buffer; a short write
    // means the connection is not usable for a one-second exchange anyway.
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    return ::send(fd_, text.data(), text.size(), flags) == static_cast<ssize_t>(text.size());
}

ReplayClient::State ReplayClient::step(std::uint64_t now_ns, Callback callback, void* context) {
    if (!busy())
        return state_;
    if (now_ns >= deadline_ns_)
        return fail("timed out after " + std::to_string(received()) + " of " + std::to_string(last_ - first_ + 1) +
                    " messages");
    if (state_ == State::Connecting) {
        pollfd waiting{fd_, POLLOUT, 0};
        if (::poll(&waiting, 1, 0) <= 0)
            return state_;
        int status = 0;
        socklen_t length = sizeof(status);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &status, &length) != 0 || status != 0)
            return fail(std::string("connect: ") + std::strerror(status ? status : errno));
        const std::string fields = std::string("98=0") + kSoh + "108=10" + kSoh + "553=" + config_.username + kSoh +
                                   "554=" + config_.password + kSoh + "1137=9" + kSoh;
        if (!send_all(fix_message("A", 1, fields)))
            return fail("could not send Logon");
        state_ = State::AwaitingLogon;
    }
    for (;;) {
        const ssize_t got = ::recv(fd_, buffer_.data() + buffered_, buffer_.size() - buffered_, 0);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return state_;
            return fail(std::string("recv: ") + std::strerror(errno));
        }
        if (got == 0)
            return fail("the gate closed the connection after " + std::to_string(received()) + " of " +
                        std::to_string(last_ - first_ + 1) + " messages");
        buffered_ += static_cast<std::size_t>(got);
        if (!consume(callback, context) || !busy())
            return state_;
    }
}

// Handles every complete frame in the buffer. Returns false once the exchange is over.
bool ReplayClient::consume(Callback callback, void* context) {
    std::size_t at = 0;
    while (buffered_ - at >= 4) {
        const std::uint8_t* frame = buffer_.data() + at;
        if (length_order_ == 0) {
            // The guide does not give the byte order of the length; exactly one reading is sane.
            const bool little = load32(frame, true) != 0 && load32(frame, true) <= kMaxFrame;
            const bool big = load32(frame, false) != 0 && load32(frame, false) <= kMaxFrame;
            if (little == big) {
                fail("cannot tell the byte order of the frame length");
                return false;
            }
            length_order_ = little ? 1 : 2;
        }
        const std::uint32_t length = load32(frame, length_order_ == 1);
        if (length == 0 || length > kMaxFrame) {
            fail("implausible frame length " + std::to_string(length));
            return false;
        }
        if (buffered_ - at < 4 + std::size_t{length})
            break;
        const std::uint8_t* body = frame + 4;
        std::uint32_t id = 0, seq = 0;
        if (message_offset_ < 0) {
            // Nor does it say whether the UDP sequence preamble is repeated inside the frame.
            if (peek(body, length, id, seq))
                message_offset_ = 0;
            else if (length > 4 && peek(body + 4, length - 4, id, seq))
                message_offset_ = 4;
            else {
                fail("the first frame is not a FAST message");
                return false;
            }
        } else if (length <= static_cast<std::uint32_t>(message_offset_) ||
                   !peek(body + message_offset_, length - message_offset_, id, seq)) {
            fail("undecodable frame after " + std::to_string(received()) + " messages");
            return false;
        }
        const std::uint8_t* message = body + message_offset_;
        const std::size_t size = length - static_cast<std::size_t>(message_offset_);
        at += 4 + std::size_t{length};
        if (id == Logon::kTemplateId) {
            if (state_ != State::AwaitingLogon) {
                fail("unexpected Logon");
                return false;
            }
            const std::string fields = "1180=" + channel_ + kSoh + "1182=" + std::to_string(first_) + kSoh +
                                       "1183=" + std::to_string(last_) + kSoh;
            if (!send_all(fix_message("V", 2, fields))) {
                fail("could not send Market Data Request");
                return false;
            }
            state_ = State::Streaming;
        } else if (id == Logout::kTemplateId) {
            std::string reason;
            Reader r{message, message + size};
            std::uint64_t pmap;
            Logout logout;
            if (read_header(r, pmap) == Logout::kTemplateId && decode(r, pmap, logout) && logout.Text.present)
                reason = logout.Text.value.str();
            // Always confirm: an unanswered logout counts against this address.
            logout_sent_ = send_all(fix_message("5", state_ == State::Streaming ? 3 : 2, {}));
            close_socket();
            if (state_ == State::Streaming && next_ == last_ + 1) {
                state_ = State::Done;
            } else {
                error_ = "the gate ended the session after " + std::to_string(received()) + " of " +
                         std::to_string(last_ - first_ + 1) + " messages" + (reason.empty() ? "" : ": " + reason);
                state_ = State::Failed;
            }
            return false;
        } else if (state_ != State::Streaming) {
            fail("data before Logon");
            return false;
        } else if (seq != next_ || seq > last_) {
            fail("expected message " + std::to_string(next_) + ", received " + std::to_string(seq));
            return false;
        } else {
            ++next_;
            callback(context, seq, message, size);
        }
    }
    std::memmove(buffer_.data(), buffer_.data() + at, buffered_ - at);
    buffered_ -= at;
    return true;
}

} // namespace moex::fast::asts
