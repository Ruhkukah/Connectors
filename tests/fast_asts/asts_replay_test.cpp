// TCP replay: the client against an in-process stand-in for the gate, and the
// stream putting recovered packets back without waiting for snapshots.

#include "asts_test_encoder.hpp"
#include "moex/fast/asts/asts_order_book.hpp"
#include "moex/fast/asts/asts_replay_client.hpp"
#include "moex/fast/asts/asts_stream.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace moex::fast;
using namespace moex::fast::asts;
using asts_test::Bytes;
using asts_test::Dec;
using asts_test::Encoder;

namespace {

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition))                                                                                              \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition);       \
    } while (false)

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

using Fix = std::map<int, std::string>;

// A stand-in for the gate's replay port. It checks what the client sends as strictly
// as the real one is said to, and serves packets from a table keyed by sequence number.
struct FakeGate {
    struct Script {
        bool big_endian_length{};
        bool repeat_preamble{};   // put the 4-byte sequence preamble inside each frame
        std::string reject;       // answer the request with a Logout carrying this text
        int close_after{-1};      // drop the connection after this many messages
        bool silent{};            // accept and say nothing
        std::uint32_t skip_seq{}; // leave this one out of the replay
    };

    Script script;
    std::map<std::uint32_t, Bytes> packets; // preamble + FAST message, as on the UDP feed
    int listener{-1};
    std::uint16_t port{};
    std::thread thread;
    std::vector<Fix> received;
    std::string problem; // first protocol violation by the client
    std::atomic<bool> stop{false};

    FakeGate() {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        ::fcntl(listener, F_SETFL, ::fcntl(listener, F_GETFL, 0) | O_NONBLOCK);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener, 4) != 0 ||
            ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            throw std::runtime_error("fake gate cannot listen");
        port = ntohs(address.sin_port);
    }
    ~FakeGate() {
        finish();
        ::close(listener);
    }
    void serve_once() {
        thread = std::thread([this] { session(); });
    }
    void finish() {
        stop = true;
        if (thread.joinable())
            thread.join();
    }

  private:
    // Reads one FIX message and verifies BodyLength and CheckSum the way a strict server would.
    bool read_fix(int fd, Fix& out) {
        std::string text;
        char c;
        while (text.size() < 8 || text.compare(text.size() - 8, 4, "\x01"
                                                                   "10=") != 0 ||
               text.back() != '\x01') {
            timeval timeout{2, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            if (::recv(fd, &c, 1, 0) != 1)
                return false;
            text += c;
        }
        const std::size_t body_start = text.find('\x01', text.find("\x01"
                                                                   "9=") + 1) + 1;
        const std::size_t trailer = text.size() - 7;
        unsigned sum = 0;
        for (std::size_t i = 0; i < trailer; ++i)
            sum += static_cast<unsigned char>(text[i]);
        std::size_t at = 0;
        std::vector<int> order;
        while (at < text.size()) {
            const std::size_t equals = text.find('=', at), end = text.find('\x01', at);
            const int tag = std::stoi(text.substr(at, equals - at));
            out[tag] = text.substr(equals + 1, end - equals - 1);
            order.push_back(tag);
            at = end + 1;
        }
        if (order.size() < 4 || order[0] != 8 || order[1] != 9 || order[2] != 35 || order.back() != 10 ||
            out[8] != "FIXT.1.1" || std::stoul(out[9]) != trailer - body_start || std::stoul(out[10]) != sum % 256 ||
            out[52].size() != 18)
            problem = problem.empty() ? "malformed FIX: " + text : problem;
        received.push_back(out);
        return true;
    }
    void send_frame(int fd, const Bytes& message, std::uint32_t seq) {
        Bytes frame;
        const std::uint32_t length = static_cast<std::uint32_t>(message.size()) + (script.repeat_preamble ? 4 : 0);
        for (int i = 0; i < 4; ++i)
            frame.push_back(static_cast<std::uint8_t>(length >> (script.big_endian_length ? 24 - 8 * i : 8 * i)));
        if (script.repeat_preamble)
            for (int i = 0; i < 4; ++i)
                frame.push_back(static_cast<std::uint8_t>(seq >> (8 * i)));
        frame.insert(frame.end(), message.begin(), message.end());
        // Dribble it out so the client has to reassemble frames across reads.
        for (std::size_t at = 0; at < frame.size(); at += 7)
            ::send(fd, frame.data() + at, std::min<std::size_t>(7, frame.size() - at), 0);
    }
    static Bytes session_message(std::uint32_t template_id, std::uint32_t seq, bool logon, const std::string& text) {
        Encoder e;
        e.raw(0xC0).u(template_id).s("MoexConnector").u(seq).u(261120052233000001ull);
        if (logon)
            e.i(10).ns({}).ns({}).s("9");
        else
            e.ns(text.empty() ? std::optional<std::string_view>{} : std::string_view(text));
        return e.out;
    }
    void session() {
        // Wait with poll so a client that never connects, or leaves at once, cannot strand the thread.
        int fd = -1;
        while (fd < 0 && !stop) {
            pollfd waiting{listener, POLLIN, 0};
            if (::poll(&waiting, 1, 20) > 0)
                fd = ::accept(listener, nullptr, nullptr);
        }
        if (fd < 0)
            return;
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK); // BSD hands the listener's mode down
        Fix logon, request, logout;
        if (script.silent) {
            while (!stop)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else if (read_fix(fd, logon)) {
            if (logon[35] != "A" || logon[34] != "1" || logon[553] != "user0" || logon[554] != "pass0" ||
                logon[1137] != "9" || logon[49].empty() || logon[56] != "MOEX")
                problem = problem.empty() ? "bad Logon" : problem;
            send_frame(fd, session_message(2101, 1, true, {}), 1);
            if (read_fix(fd, request)) {
                if (request[35] != "V" || request[34] != "2" || request[1128] != "9" || request[1180].empty())
                    problem = problem.empty() ? "bad Market Data Request" : problem;
                const auto first = static_cast<std::uint32_t>(std::stoul(request[1182]));
                const auto last = static_cast<std::uint32_t>(std::stoul(request[1183]));
                int sent = 0;
                bool dropped = false;
                for (std::uint32_t seq = first; script.reject.empty() && seq <= last && !dropped; ++seq) {
                    if (seq == script.skip_seq || !packets.count(seq))
                        continue;
                    if (sent++ == script.close_after) {
                        dropped = true;
                        break;
                    }
                    send_frame(fd, Bytes(packets[seq].begin() + 4, packets[seq].end()), seq);
                }
                if (!dropped) {
                    send_frame(fd, session_message(2102, 2, false, script.reject), 2);
                    if (!read_fix(fd, logout) || logout[35] != "5")
                        problem = problem.empty() ? "no confirming Logout" : problem;
                }
            }
        }
        ::close(fd);
    }
};

ReplayConfig config_for(const FakeGate& gate) {
    ReplayConfig config;
    config.host = "127.0.0.1";
    config.port = gate.port;
    config.timeout_ns = 2'000'000'000;
    return config;
}

struct Fetched {
    ReplayClient::State state;
    std::vector<std::uint32_t> seqs;
    std::vector<Bytes> messages;
};

Fetched fetch(ReplayClient& client, std::uint32_t first, std::uint32_t last) {
    Fetched out{};
    REQUIRE(client.start("OLR", first, last, now_ns()));
    while (client.busy()) {
        client.poll(now_ns(), [&](std::uint32_t seq, const std::uint8_t* data, std::size_t size) {
            out.seqs.push_back(seq);
            out.messages.emplace_back(data, data + size);
        });
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    out.state = client.state();
    return out;
}

asts_test::Order add(std::string symbol, std::string id, std::int32_t rpt, char type, std::int64_t price) {
    asts_test::Order order;
    order.action = 0;
    order.type = type;
    order.id = std::move(id);
    order.symbol = std::move(symbol);
    order.rpt_seq = rpt;
    order.price = Dec{price, -2};
    order.size = Dec{1, 0};
    order.board = "CETS";
    return order;
}

void fix_framing() {
    const std::string message = ReplayClient::frame_fix("35=0\x01"
                                                        "34=7\x01");
    REQUIRE(message.rfind("8=FIXT.1.1\x01"
                          "9=10\x01"
                          "35=0\x01"
                          "34=7\x01"
                          "10=",
                          0) == 0);
    unsigned sum = 0;
    for (std::size_t i = 0; i + 7 < message.size(); ++i)
        sum += static_cast<unsigned char>(message[i]);
    char expected[8];
    std::snprintf(expected, sizeof(expected), "%03u", sum % 256);
    REQUIRE(message.substr(message.size() - 4, 3) == expected && message.back() == '\x01');
}

// The guide leaves the length's byte order and the presence of the sequence preamble
// open, so the client has to cope with every combination.
void fetches_a_range() {
    for (int variant = 0; variant < 4; ++variant) {
        FakeGate gate;
        gate.script.big_endian_length = variant & 1;
        gate.script.repeat_preamble = variant & 2;
        for (std::uint32_t seq = 300; seq <= 306; ++seq)
            gate.packets[seq] = seq == 303 ? asts_test::heartbeat(seq)
                                           : asts_test::orders(seq, {add("USD", std::to_string(seq), 1, '0', 9000)});
        gate.serve_once();
        ReplayClient client(config_for(gate));
        const Fetched fetched = fetch(client, 301, 305);
        gate.finish();
        REQUIRE(fetched.state == ReplayClient::State::Done && gate.problem.empty());
        REQUIRE((fetched.seqs == std::vector<std::uint32_t>{301, 302, 303, 304, 305}));
        for (std::size_t i = 0; i < fetched.messages.size(); ++i) // exactly the FAST message, nothing around it
            REQUIRE(fetched.messages[i] ==
                    Bytes(gate.packets[301 + static_cast<std::uint32_t>(i)].begin() + 4, gate.packets[301 + static_cast<std::uint32_t>(i)].end()));
        REQUIRE(gate.received.size() == 3 && gate.received[1][1180] == "OLR" && gate.received[1][1182] == "301" &&
                gate.received[1][1183] == "305" && gate.received[2][34] == "3" && client.connections() == 1);
    }
}

void failures_are_reported_not_retried() {
    { // The gate refuses the request: its reason is kept and the logout is still confirmed.
        FakeGate gate;
        gate.script.reject = "Too many messages requested";
        gate.serve_once();
        ReplayClient client(config_for(gate));
        const Fetched fetched = fetch(client, 10, 12);
        gate.finish();
        REQUIRE(fetched.state == ReplayClient::State::Failed && fetched.seqs.empty() && gate.problem.empty());
        REQUIRE(client.error().find("Too many messages requested") != std::string::npos && gate.received.size() == 3);
    }
    { // A message missing from the replay is a failure even though the session ends normally.
        FakeGate gate;
        for (std::uint32_t seq = 10; seq <= 12; ++seq)
            gate.packets[seq] = asts_test::heartbeat(seq);
        gate.script.skip_seq = 11;
        gate.serve_once();
        ReplayClient client(config_for(gate));
        const Fetched fetched = fetch(client, 10, 12);
        gate.finish();
        REQUIRE(fetched.state == ReplayClient::State::Failed && (fetched.seqs == std::vector<std::uint32_t>{10}));
    }
    { // The connection drops midway.
        FakeGate gate;
        for (std::uint32_t seq = 10; seq <= 14; ++seq)
            gate.packets[seq] = asts_test::heartbeat(seq);
        gate.script.close_after = 2;
        gate.serve_once();
        ReplayClient client(config_for(gate));
        const Fetched fetched = fetch(client, 10, 14);
        gate.finish();
        REQUIRE(fetched.state == ReplayClient::State::Failed && fetched.seqs.size() == 2 && client.received() == 2);
    }
    { // The gate never answers: the client gives up at its deadline.
        FakeGate gate;
        gate.script.silent = true;
        gate.serve_once();
        ReplayConfig config = config_for(gate);
        config.timeout_ns = 150'000'000;
        ReplayClient client(config);
        const std::uint64_t started = now_ns();
        const Fetched fetched = fetch(client, 10, 11);
        gate.finish();
        REQUIRE(fetched.state == ReplayClient::State::Failed && client.error().find("timed out") != std::string::npos);
        REQUIRE(now_ns() - started < 1'500'000'000);
    }
    { // Ranges the gate would reject are never sent, and nothing is in flight twice.
        FakeGate gate;
        ReplayClient client(config_for(gate));
        REQUIRE(!client.start("OLR", 1, 501, now_ns()) && !client.start("OLR", 9, 8, now_ns()));
        REQUIRE(client.connections() == 0 && client.state() == ReplayClient::State::Idle);
        gate.script.silent = true;
        gate.serve_once();
        REQUIRE(client.start("OLR", 1, 500, now_ns()) && !client.start("OLR", 1, 2, now_ns()));
        REQUIRE(client.connections() == 1);
    }
}

struct BookSink : StreamSink<X_OLR_CURR, W_OLS_CURR> {
    OrderBook book;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> losses;
    int clears{}, rejected{};

    void on_clear(std::uint32_t slot) {
        book.clear(slot);
        ++clears;
    }
    void on_snapshot(std::uint32_t slot, const W_OLS_CURR&, const SnapshotEntry& e) {
        rejected += !book.apply_snapshot(slot, e);
    }
    void on_incremental(std::uint32_t slot, const X_OLR_CURR&, const IncrementalEntry& e) {
        rejected += !book.apply(slot, e);
    }
    void on_loss(std::uint32_t first, std::uint32_t last) {
        losses.emplace_back(first, last);
    }
};

// Loss, live traffic carrying on, then the lost packets fetched from the gate: every
// instrument is whole again and no snapshot was needed.
void stream_recovers_through_the_client() {
    FakeGate gate;
    BookSink sink;
    StreamConfig stream_config;
    stream_config.tcp_replay = true;
    stream_config.reorder_timeout_ns = 1000;
    CurrencyOrders<BookSink> stream(sink, stream_config);
    const auto feed = [&](const Bytes& packet, std::uint64_t now) { stream.on_incremental(packet.data(), packet.size(), now); };
    const auto snap = [&](const Bytes& packet) { stream.on_snapshot(packet.data(), packet.size()); };

    feed(asts_test::heartbeat(99), 0);
    snap(asts_test::heartbeat(40));
    snap(asts_test::order_snapshot(1, "USD", "CETS", 5, {{'0', "1", {9000, -2}, {1, 0}}}));
    snap(asts_test::order_snapshot(2, "EUR", "CETS", 8, {{'1', "2", {9900, -2}, {1, 0}}}));
    snap(asts_test::order_snapshot(3, "CNY", "CETS", 3, {{'1', "3", {1200, -2}, {1, 0}}}));
    snap(asts_test::order_snapshot(1, "USD", "CETS", 5, {}));
    REQUIRE(stream.unsynced() == 0 && sink.book.orders() == 3);
    const std::uint64_t applied_before = stream.stats().snapshots_applied;

    gate.packets[100] = asts_test::orders(100, {add("USD", "10", 6, '0', 9010)});
    gate.packets[101] = asts_test::orders(101, {add("EUR", "11", 9, '1', 9910), add("USD", "12", 7, '0', 9020)});
    gate.packets[102] = asts_test::orders(102, {add("EUR", "13", 10, '1', 9920)});
    gate.packets[103] = asts_test::orders(103, {add("USD", "14", 8, '0', 9030), add("EUR", "15", 11, '1', 9930)});
    gate.packets[104] = asts_test::orders(104, {add("EUR", "16", 12, '1', 9940)});
    feed(gate.packets[100], 10);
    // 101 and 102 are lost on both feeds; 103 and 104 arrive and have to wait.
    feed(gate.packets[103], 20);
    feed(gate.packets[104], 30);
    stream.on_timer(5000);
    REQUIRE((sink.losses == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{101, 102}}));
    REQUIRE(stream.unsynced() == 3 && stream.recovery_pending() && sink.book.orders() == 4 && sink.clears == 3);

    gate.serve_once();
    ReplayClient client(config_for(gate));
    REQUIRE(client.start("OLR", 101, 102, now_ns()));
    while (client.busy()) {
        client.poll(now_ns(), [&](std::uint32_t, const std::uint8_t* data, std::size_t size) { stream.on_recovered(data, size); });
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    gate.finish();
    REQUIRE(client.state() == ReplayClient::State::Done && gate.problem.empty());
    REQUIRE(stream.unsynced() == 3); // nothing is declared whole until the range is confirmed complete
    stream.replay_finished(101, 102, true);

    const std::uint32_t usd = stream.find("USD", "CETS"), eur = stream.find("EUR", "CETS"), cny = stream.find("CNY", "CETS");
    REQUIRE(stream.unsynced() == 0 && !stream.recovery_pending() && sink.rejected == 0 && sink.clears == 3);
    REQUIRE(stream.rpt_seq(usd) == 8 && stream.rpt_seq(eur) == 12 && stream.rpt_seq(cny) == 3);
    REQUIRE(sink.book.orders(usd) == 4 && sink.book.orders(eur) == 5 && sink.book.orders(cny) == 1);
    REQUIRE(stream.stats().snapshots_applied == applied_before && stream.stats().recovered_packets == 2);
    REQUIRE(stream.stats().confirmed_by_replay == 1); // CNY: nothing lost, nothing held

    // A range that cannot be fetched leaves recovery to snapshots, exactly as without replay.
    feed(asts_test::heartbeat(9000), 2'000'000'000);
    REQUIRE(sink.losses.size() == 2 && stream.unsynced() == 3);
    stream.replay_finished(sink.losses[1].first, sink.losses[1].second, false);
    REQUIRE(stream.unsynced() == 3 && stream.stats().replay_ranges_failed == 1);
    snap(asts_test::order_snapshot(1, "USD", "CETS", 8, {}));
    snap(asts_test::order_snapshot(2, "EUR", "CETS", 12, {}));
    snap(asts_test::order_snapshot(3, "CNY", "CETS", 3, {}));
    REQUIRE(stream.unsynced() == 0 && stream.stats().confirmed_by_snapshot == 3 && sink.book.orders() == 10);
}

// Replay repairs one outage. An instrument still carrying an earlier, unrepaired loss
// must not be declared whole because a later range came back complete.
void later_replay_does_not_vouch_for_an_older_loss() {
    BookSink sink;
    StreamConfig stream_config;
    stream_config.tcp_replay = true;
    CurrencyOrders<BookSink> stream(sink, stream_config);
    const auto feed = [&](const Bytes& packet) { stream.on_incremental(packet.data(), packet.size(), 0); };
    const auto snap = [&](const Bytes& packet) { stream.on_snapshot(packet.data(), packet.size()); };
    feed(asts_test::heartbeat(99));
    snap(asts_test::heartbeat(40));
    snap(asts_test::order_snapshot(1, "USD", "CETS", 5, {{'0', "1", {9000, -2}, {1, 0}}}));
    snap(asts_test::order_snapshot(2, "EUR", "CETS", 8, {{'1', "2", {9900, -2}, {1, 0}}}));
    snap(asts_test::order_snapshot(1, "USD", "CETS", 5, {}));
    REQUIRE(stream.unsynced() == 0);

    // First outage: packets 100..299 are gone (EUR's RptSeq 9 among them) and cannot be fetched.
    feed(asts_test::heartbeat(300));
    REQUIRE((sink.losses.back() == std::pair<std::uint32_t, std::uint32_t>{100, 299}));
    stream.replay_finished(100, 299, false);
    // USD carries on by itself; EUR stays quiet.
    feed(asts_test::orders(301, {add("USD", "10", 6, '0', 9010)}));
    const std::uint32_t usd = stream.find("USD", "CETS"), eur = stream.find("EUR", "CETS");
    REQUIRE(stream.synced(usd) && !stream.synced(eur));

    // Second outage, fetched in full, and it holds nothing for EUR.
    const Bytes lost = asts_test::orders(302, {add("USD", "11", 7, '0', 9020)});
    feed(asts_test::heartbeat(303));
    stream.on_timer(10'000'000'000); // one missing packet is only given up on after the reorder wait
    REQUIRE((sink.losses.back() == std::pair<std::uint32_t, std::uint32_t>{302, 302}) && stream.unsynced() == 2);
    stream.on_recovered(lost.data() + kPreambleSize, lost.size() - kPreambleSize);
    stream.replay_finished(302, 302, true);
    REQUIRE(stream.synced(usd) && stream.rpt_seq(usd) == 7 && sink.book.orders(usd) == 3);
    REQUIRE(!stream.synced(eur) && stream.needs_snapshot());

    // Finishing a range that a newer loss has superseded changes nothing.
    feed(asts_test::heartbeat(400));
    feed(asts_test::heartbeat(500));
    REQUIRE((sink.losses.back() == std::pair<std::uint32_t, std::uint32_t>{401, 499}) && stream.unsynced() == 2);
    stream.replay_finished(304, 399, true);
    REQUIRE(stream.unsynced() == 2 && stream.recovery_pending());
}

} // namespace

int main() {
    try {
        fix_framing();
        fetches_a_range();
        failures_are_reported_not_retried();
        stream_recovers_through_the_client();
        later_replay_does_not_vouch_for_an_older_loss();
    } catch (const std::exception& error) {
        std::cerr << "asts_replay_test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
