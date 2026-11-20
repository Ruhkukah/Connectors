#pragma once

// TCP replay client for the MOEX ASTS FAST gate: fetches a range of messages a
// UDP feed has already published, for when both feed copies lost a few packets.
//
// One request per connection, as the gate requires: connect, FIX Logon, the
// gate answers with a FAST Logon, FIX Market Data Request with the channel and
// sequence range, the gate streams the FAST messages (each behind a 4-byte
// length) and then a FAST Logout, which is confirmed with a FIX Logout.
//
// Non-blocking: start() begins a request and poll() advances it a little each
// time it is called, so it can share the thread that reads the UDP feeds.
//
// The gate's limits matter (TCP_Replay_Limits.pdf): at most 500 messages per
// request, a request within one second of logon, a bounded number of
// connections per day, and an address may be blocked for repeated protocol
// errors. So this client never retries by itself and refuses oversized ranges;
// a failed range is left to snapshot recovery.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace moex::fast::asts {

struct ReplayConfig {
    std::string host;                    // <ip> of the channel's "H" connection in the configuration file
    std::uint16_t port{};                // its <port>
    std::string sender{"MoexConnector"}; // SenderCompID; the gate asks for something that identifies the firm
    std::string target{"MOEX"};
    std::string username{"user0"}; // the guide publishes user0/pass0, user1/pass1, user2/pass2
    std::string password{"pass0"};
    std::uint32_t max_messages{500};
    std::uint64_t timeout_ns{3'000'000'000}; // whole exchange, connect to logout
};

class ReplayClient {
  public:
    enum class State { Idle, Connecting, AwaitingLogon, Streaming, Done, Failed };

    explicit ReplayClient(ReplayConfig config) : config_(std::move(config)) {}
    ReplayClient(const ReplayClient&) = delete;
    ReplayClient& operator=(const ReplayClient&) = delete;
    ~ReplayClient();

    // Begins fetching messages first_seq..last_seq of a channel ("OLR", "TLR", "MSR", "ISF").
    // Returns false, with error() set, if a request is in flight, the range is empty or
    // larger than max_messages, or the connection cannot be started.
    bool start(std::string_view channel, std::uint32_t first_seq, std::uint32_t last_seq, std::uint64_t now_ns);

    // Advances the request. on_message(seq, data, size) receives each replayed FAST
    // message (no length prefix, no sequence preamble), in sequence. Returns the state;
    // Done means every message of the range arrived and the session closed cleanly.
    // After Done or Failed the client stays there until the next start().
    template <class Handler> State poll(std::uint64_t now_ns, Handler&& on_message) {
        const auto thunk = [](void* context, std::uint32_t seq, const std::uint8_t* data, std::size_t size) {
            (*static_cast<Handler*>(context))(seq, data, size);
        };
        return step(now_ns, thunk, &on_message);
    }

    [[nodiscard]] State state() const noexcept {
        return state_;
    }
    [[nodiscard]] bool busy() const noexcept {
        return state_ == State::Connecting || state_ == State::AwaitingLogon || state_ == State::Streaming;
    }
    [[nodiscard]] std::uint32_t first_seq() const noexcept {
        return first_;
    }
    [[nodiscard]] std::uint32_t last_seq() const noexcept {
        return last_;
    }
    [[nodiscard]] std::uint32_t received() const noexcept {
        return next_ - first_;
    }
    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }
    // Connections opened since construction; the gate caps these per day.
    [[nodiscard]] std::uint64_t connections() const noexcept {
        return connections_;
    }

    // What the gate's framing turned out to be, once the first frame has been read:
    // 0 unknown, 1 little-endian, 2 big-endian length; and the bytes between the
    // length and the FAST message (-1 unknown, 0, or 4 for a repeated sequence preamble).
    [[nodiscard]] int length_order() const noexcept {
        return length_order_;
    }
    [[nodiscard]] int message_offset() const noexcept {
        return message_offset_;
    }

    // FIX message with BeginString, BodyLength and CheckSum around the given body
    // ("35=A<SOH>49=...<SOH>"). Exposed for testing.
    [[nodiscard]] static std::string frame_fix(std::string_view body);

  private:
    using Callback = void (*)(void* context, std::uint32_t seq, const std::uint8_t* data, std::size_t size);

    State step(std::uint64_t now_ns, Callback callback, void* context);
    bool send_all(const std::string& text);
    bool consume(Callback callback, void* context);
    State fail(std::string reason);
    void close_socket() noexcept;
    [[nodiscard]] std::string fix_message(std::string_view type, std::uint32_t seq, std::string_view fields) const;

    ReplayConfig config_;
    State state_{State::Idle};
    int fd_{-1};
    std::string channel_;
    std::string error_;
    std::uint32_t first_{}, last_{}, next_{};
    std::uint64_t deadline_ns_{};
    std::uint64_t connections_{};
    std::vector<std::uint8_t> buffer_;
    std::size_t buffered_{};
    int length_order_{};   // 0 unknown, 1 little-endian, 2 big-endian
    int message_offset_{-1}; // bytes between the length and the FAST message: 0, or 4 for a sequence preamble
    bool logout_sent_{};
};

} // namespace moex::fast::asts
