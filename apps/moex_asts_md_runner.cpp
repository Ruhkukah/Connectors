// Read-only ASTS FAST market-data runner for the securities and FX markets: joins
// one order (OLR/OLS) or trade (TLR/TLS) stream pair, keeps an order-by-order
// book and reports feed health once a second.

#include "moex/fast/asts/asts_order_book.hpp"
#include "moex/fast/asts/asts_replay_client.hpp"
#include "moex/fast/asts/asts_stream.hpp"
#include "moex/fast/fast_udp.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace moex::fast;
using namespace moex::fast::asts;

std::atomic<bool> stop_requested{false};

void on_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Options {
    std::vector<std::pair<bool, UdpEndpoint>> endpoints; // snapshot?, endpoint
    std::string interface, symbol, board;
    UdpEndpoint replay; // the channel's TCP replay address; empty group disables replay
    std::string channel;
    long replay_budget{20}; // requests this run may make; the gate caps connections per day
    long seconds{};
    bool spin{};
};

template <class Incremental, class Snapshot, bool Orders> struct Sink : StreamSink<Incremental, Snapshot> {
    OrderBook book;
    std::uint64_t updates{}, trades{}, rejected{};
    std::deque<std::pair<std::uint32_t, std::uint32_t>> losses; // ranges waiting for a replay request

    void on_loss(std::uint32_t first, std::uint32_t last) {
        losses.emplace_back(first, last);
    }

    void on_clear(std::uint32_t slot) {
        if constexpr (Orders)
            book.clear(slot);
    }
    void on_snapshot(std::uint32_t slot, const Snapshot&, const typename Snapshot::GroupMDEntries& e) {
        if constexpr (Orders)
            rejected += !book.apply_snapshot(slot, e);
    }
    void on_incremental(std::uint32_t slot, const Incremental&, const typename Incremental::GroupMDEntries& e) {
        if constexpr (Orders) {
            ++updates;
            rejected += !book.apply(slot, e);
        } else {
            trades += e.MDEntryType.front() == 'z';
        }
    }
};

template <class Incremental, class Snapshot, bool Orders> int run(const Options& options) {
    UdpReceiver receiver;
    std::vector<bool> is_snapshot;
    std::vector<std::uint64_t> datagrams; // raw receipt per socket, before any sequencing or decoding
    for (auto [snapshot, endpoint] : options.endpoints) {
        endpoint.interface = options.interface;
        if (receiver.add(endpoint) < 0) {
            std::fprintf(stderr, "%s\n", receiver.error().c_str());
            return 1;
        }
        is_snapshot.push_back(snapshot);
        datagrams.push_back(0);
    }
    Sink<Incremental, Snapshot, Orders> sink;
    StreamConfig stream_config;
    stream_config.tcp_replay = !options.replay.group.empty();
    Stream<Incremental, Snapshot, Sink<Incremental, Snapshot, Orders>> stream(sink, stream_config);
    ReplayConfig replay_config;
    replay_config.host = options.replay.group;
    replay_config.port = options.replay.port;
    ReplayClient replay(replay_config);
    long replay_budget = options.replay_budget;
    std::uint64_t replays_done = 0, replays_failed = 0;
    const std::uint64_t started = now_ns();
    std::uint64_t next_report = started + 1'000'000'000ull;
    bool snapshots_joined = true;
    while (!stop_requested.load(std::memory_order_relaxed)) {
        receiver.poll(options.spin ? 0 : 1, [&](int index, const std::uint8_t* data, std::size_t size) {
            ++datagrams[static_cast<std::size_t>(index)];
            if (is_snapshot[static_cast<std::size_t>(index)])
                stream.on_snapshot(data, size);
            else
                stream.on_incremental(data, size, now_ns());
        });
        const std::uint64_t now = now_ns();
        stream.on_timer(now);
        // One replay request at a time; a range that cannot be fetched is left to the snapshots.
        if (replay.busy()) {
            const auto state = replay.poll(now, [&](std::uint32_t, const std::uint8_t* data, std::size_t size) {
                stream.on_recovered(data, size);
            });
            if (state == ReplayClient::State::Done || state == ReplayClient::State::Failed) {
                const bool ok = state == ReplayClient::State::Done;
                ++(ok ? replays_done : replays_failed);
                std::printf("replay %u..%u: %s%s\n", replay.first_seq(), replay.last_seq(),
                            ok ? "recovered" : "failed: ", ok ? "" : replay.error().c_str());
                stream.replay_finished(replay.first_seq(), replay.last_seq(), ok);
            }
        } else if (!sink.losses.empty()) {
            const auto [first, last] = sink.losses.front();
            sink.losses.pop_front();
            if (replay_budget > 0 && replay.start(options.channel, first, last, now)) {
                --replay_budget;
            } else {
                ++replays_failed;
                std::printf("replay %u..%u: not requested: %s\n", first, last,
                            replay_budget > 0 ? replay.error().c_str() : "request budget for this run is spent");
                stream.replay_finished(first, last, false);
            }
        }
        // The snapshot feed is only worth its bandwidth while something is out of sync.
        if (stream.needs_snapshot() != snapshots_joined && (stream.needs_snapshot() || stream.instruments() > 0)) {
            snapshots_joined = stream.needs_snapshot();
            for (std::size_t index = 0; index < is_snapshot.size(); ++index)
                if (is_snapshot[index] && !receiver.set_joined(static_cast<int>(index), snapshots_joined))
                    std::fprintf(stderr, "%s\n", receiver.error().c_str());
        }
        if (now < next_report)
            continue;
        next_report += 1'000'000'000ull;
        const StreamStats& stats = stream.stats();
        std::printf("packets=%llu dup=%llu losses=%llu malformed=%llu instruments=%zu unsynced=%zu rebuilt=%llu "
                    "confirmed=%llu orders=%zu updates=%llu trades=%llu rejected=%llu replays=%llu/%llu "
                    "snapshot_feed=%s\n",
                    static_cast<unsigned long long>(stats.packets),
                    static_cast<unsigned long long>(stream.arbiter().duplicates),
                    static_cast<unsigned long long>(stats.losses), static_cast<unsigned long long>(stats.malformed),
                    stream.instruments(), stream.unsynced(), static_cast<unsigned long long>(stats.snapshots_applied),
                    static_cast<unsigned long long>(stats.confirmed_by_snapshot + stats.confirmed_by_replay),
                    sink.book.orders(),
                    static_cast<unsigned long long>(sink.updates), static_cast<unsigned long long>(sink.trades),
                    static_cast<unsigned long long>(sink.rejected), static_cast<unsigned long long>(replays_done),
                    static_cast<unsigned long long>(replays_done + replays_failed), snapshots_joined ? "joined" : "left");
        std::printf("  datagrams");
        for (std::size_t index = 0; index < datagrams.size(); ++index)
            std::printf(" %s:%u=%llu", options.endpoints[index].second.group.c_str(),
                        options.endpoints[index].second.port, static_cast<unsigned long long>(datagrams[index]));
        std::printf("\n");
        const std::uint32_t slot = options.symbol.empty() ? UINT32_MAX : stream.find(options.symbol, options.board);
        if (Orders && slot != UINT32_MAX) {
            const Order* bid = nullptr;
            const Order* offer = nullptr;
            sink.book.for_each(slot, [&](const Order& order) {
                if (order.side == '0' && (!bid || order.price.to_double() > bid->price.to_double()))
                    bid = &order;
                if (order.side == '1' && (!offer || order.price.to_double() < offer->price.to_double()))
                    offer = &order;
            });
            std::printf("  %s/%s %s orders=%zu best bid %.6f x %.0f | best offer %.6f x %.0f\n", options.symbol.c_str(),
                        options.board.c_str(), stream.synced(slot) ? "synced" : "STALE", sink.book.orders(slot),
                        bid ? bid->price.to_double() : 0.0, bid ? bid->size.to_double() : 0.0,
                        offer ? offer->price.to_double() : 0.0, offer ? offer->size.to_double() : 0.0);
        }
        std::fflush(stdout);
        if (options.seconds > 0 && now - started >= static_cast<std::uint64_t>(options.seconds) * 1'000'000'000ull)
            break;
    }
    return 0;
}

int usage() {
    std::fputs("usage: moex_asts_md_runner --market curr|fond --kind orders|trades\n"
               "                           --incr-a GROUP:PORT[@SOURCE] [--incr-b ...]\n"
               "                           [--snap-a GROUP:PORT[@SOURCE]] [--snap-b ...]\n"
               "                           [--interface LOCAL_IP] [--symbol SYMBOL --board BOARD] [--seconds N] [--spin]\n"
               "                           [--replay HOST:PORT [--replay-budget N]]\n"
               "Addresses come from the ASTS configuration file: OLR/OLS for orders, TLR/TLS for trades.\n"
               "--replay enables TCP replay of lost packets from the channel's H connection (off by default);\n"
               "the gate limits requests per day, so each run makes at most --replay-budget of them (default 20).\n",
               stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string market, kind;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--spin") {
            options.spin = true;
            continue;
        }
        if (i + 1 >= argc)
            return usage();
        const char* value = argv[++i];
        if (arg == "--incr-a" || arg == "--incr-b" || arg == "--snap-a" || arg == "--snap-b") {
            UdpEndpoint endpoint;
            if (!parse_endpoint(value, endpoint)) {
                std::fprintf(stderr, "bad endpoint: %s\n", value);
                return 2;
            }
            options.endpoints.emplace_back(arg.substr(2, 4) == "snap", endpoint);
        } else if (arg == "--replay") {
            if (!parse_endpoint(value, options.replay)) {
                std::fprintf(stderr, "bad replay address: %s\n", value);
                return 2;
            }
        } else if (arg == "--replay-budget") {
            options.replay_budget = std::strtol(value, nullptr, 10);
        } else if (arg == "--market") {
            market = value;
        } else if (arg == "--kind") {
            kind = value;
        } else if (arg == "--interface") {
            options.interface = value;
        } else if (arg == "--symbol") {
            options.symbol = value;
        } else if (arg == "--board") {
            options.board = value;
        } else if (arg == "--seconds") {
            options.seconds = std::strtol(value, nullptr, 10);
        } else {
            return usage();
        }
    }
    bool have_incremental = false;
    for (const auto& endpoint : options.endpoints)
        have_incremental |= !endpoint.first;
    if (!have_incremental)
        return usage();
    options.channel = kind == "trades" ? "TLR" : "OLR";
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    if (market == "curr" && kind == "orders")
        return run<X_OLR_CURR, W_OLS_CURR, true>(options);
    if (market == "curr" && kind == "trades")
        return run<X_TLR_CURR, W_TLS_CURR, false>(options);
    if (market == "fond" && kind == "orders")
        return run<X_OLR_FOND, W_OLS_FOND, true>(options);
    if (market == "fond" && kind == "trades")
        return run<X_TLR_FOND, W_TLS_FOND, false>(options);
    return usage();
}
