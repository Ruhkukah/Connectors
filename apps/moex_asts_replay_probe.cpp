// One-shot check of the ASTS TCP replay gate: listens to an incremental feed until a
// few consecutive packets have arrived, asks the gate to replay exactly those, and
// compares the replayed messages with the ones received over UDP, byte for byte.
// Makes a single connection and a single request, then exits.

#include "moex/fast/asts/asts_replay_client.hpp"
#include "moex/fast/fast_udp.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace moex::fast;
using namespace moex::fast::asts;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

int usage() {
    std::fputs("usage: moex_asts_replay_probe --incr GROUP:PORT[@SOURCE] --replay HOST:PORT --channel OLR|TLR|MSR|ISF\n"
               "                              [--interface LOCAL_IP] [--count N (default 3, max 10)] [--listen SECONDS]\n",
               stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    UdpEndpoint feed, gate;
    std::string channel, interface;
    long count = 3, listen_seconds = 30;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view arg = argv[i];
        const char* value = argv[i + 1];
        if (arg == "--incr" && parse_endpoint(value, feed))
            continue;
        if (arg == "--replay" && parse_endpoint(value, gate))
            continue;
        if (arg == "--channel")
            channel = value;
        else if (arg == "--interface")
            interface = value;
        else if (arg == "--count")
            count = std::strtol(value, nullptr, 10);
        else if (arg == "--listen")
            listen_seconds = std::strtol(value, nullptr, 10);
        else
            return usage();
    }
    if (feed.group.empty() || gate.group.empty() || channel.empty() || count < 1 || count > 10)
        return usage();

    feed.interface = interface;
    UdpReceiver receiver;
    if (receiver.add(feed) < 0) {
        std::fprintf(stderr, "%s\n", receiver.error().c_str());
        return 1;
    }
    // Collect a run of consecutive packets: sequence -> FAST message without the preamble.
    std::map<std::uint32_t, std::vector<std::uint8_t>> seen;
    const std::uint64_t listen_until = now_ns() + static_cast<std::uint64_t>(listen_seconds) * 1'000'000'000ull;
    while (static_cast<long>(seen.size()) < count && now_ns() < listen_until) {
        receiver.poll(100, [&](int, const std::uint8_t* data, std::size_t size) {
            if (size <= 4 || static_cast<long>(seen.size()) >= count)
                return;
            const std::uint32_t seq = std::uint32_t{data[0]} | std::uint32_t{data[1]} << 8 |
                                      std::uint32_t{data[2]} << 16 | std::uint32_t{data[3]} << 24;
            if (!seen.empty() && seq != seen.rbegin()->first + 1)
                seen.clear(); // only an unbroken run is worth asking for
            seen.emplace(seq, std::vector<std::uint8_t>(data + 4, data + size));
        });
    }
    if (seen.empty()) {
        std::printf("no packets on the feed in %ld s; nothing requested\n", listen_seconds);
        return 1;
    }
    const std::uint32_t first = seen.begin()->first, last = seen.rbegin()->first;
    std::printf("udp: received %zu packets, seq %u..%u\n", seen.size(), first, last);
    std::fflush(stdout);

    ReplayConfig config;
    config.host = gate.group;
    config.port = gate.port;
    config.timeout_ns = 5'000'000'000ull;
    ReplayClient client(config);
    const std::uint64_t started = now_ns();
    if (!client.start(channel, first, last, started)) {
        std::printf("not requested: %s\n", client.error().c_str());
        return 1;
    }
    std::size_t identical = 0, different = 0;
    ReplayClient::State state = client.state();
    while (client.busy()) {
        state = client.poll(now_ns(), [&](std::uint32_t seq, const std::uint8_t* data, std::size_t size) {
            const auto it = seen.find(seq);
            const bool same = it != seen.end() && it->second.size() == size &&
                              std::memcmp(it->second.data(), data, size) == 0;
            ++(same ? identical : different);
            std::printf("replayed seq %u: %zu bytes, %s\n", seq, size,
                        same ? "identical to the UDP packet" : "DIFFERS from the UDP packet");
            if (!same) {
                std::printf("  tcp:");
                for (std::size_t i = 0; i < size && i < 64; ++i)
                    std::printf(" %02x", data[i]);
                std::printf("\n  udp:");
                if (it != seen.end())
                    for (std::size_t i = 0; i < it->second.size() && i < 64; ++i)
                        std::printf(" %02x", it->second[i]);
                std::printf("\n");
            }
        });
        struct timespec nap {0, 1'000'000};
        nanosleep(&nap, nullptr);
    }
    const double ms = static_cast<double>(now_ns() - started) / 1e6;
    std::printf("result: %s in %.1f ms; %u of %u messages, %zu identical, %zu different\n",
                state == ReplayClient::State::Done ? "DONE" : "FAILED", ms, client.received(), last - first + 1,
                identical, different);
    std::printf("framing: length is %s, sequence preamble inside the frame: %s\n",
                client.length_order() == 1 ? "little-endian" : client.length_order() == 2 ? "big-endian" : "unknown",
                client.message_offset() == 4 ? "yes" : client.message_offset() == 0 ? "no" : "unknown");
    if (state != ReplayClient::State::Done)
        std::printf("error: %s\n", client.error().c_str());
    return state == ReplayClient::State::Done && different == 0 ? 0 : 1;
}
