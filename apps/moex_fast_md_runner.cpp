// Read-only FAST market-data runner: joins one stream's incremental and snapshot
// feeds, keeps aggregated books (or, with --orders-log, a per-order book) and
// reports feed health once a second.

#include "moex/fast/fast_book.hpp"
#include "moex/fast/fast_market_stream.hpp"
#include "moex/fast/fast_order_book.hpp"
#include "moex/fast/fast_udp.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace moex::fast;

std::atomic<bool> stop_requested{false};

void on_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

struct Sink : StreamSink {
    std::vector<AggregateBook<50>> books;
    std::uint64_t book_updates{}, trades{};

    void on_instrument(std::uint32_t, std::uint64_t) {
        books.emplace_back();
    }
    void on_book_clear(std::uint32_t slot) noexcept {
        books[slot].clear();
    }
    void on_snapshot(std::uint32_t slot, const Snapshot&, const Snapshot::MDEntries& e) noexcept {
        books[slot].apply(e);
    }
    void on_incremental(std::uint32_t slot, const Incremental&, const Incremental::MDEntries& e) noexcept {
        if (books[slot].apply(e))
            ++book_updates;
        else
            trades += e.MDEntryType.front() == '2';
    }
    void on_sequence_reset() noexcept {
        for (auto& book : books)
            book.clear();
    }
};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

int usage() {
    std::fputs("usage: moex_fast_md_runner --incr-a GROUP:PORT[@SOURCE] [--incr-b ...]\n"
               "                           [--snap-a GROUP:PORT[@SOURCE]] [--snap-b ...]\n"
               "                           [--interface LOCAL_IP] [--security-id ID] [--seconds N] [--spin]\n"
               "                           [--orders-log]   the feeds are ORDERS-LOG; keep a per-order book\n"
               "Addresses come from the feed's configuration.xml (<ip>, <port>, <src-ip>).\n",
               stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::pair<bool, UdpEndpoint>> endpoints; // snapshot?, endpoint
    std::string interface;
    std::uint64_t watch_id = 0;
    long seconds = 0;
    bool spin = false;
    bool orders_log = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (arg == "--spin") {
            spin = true;
            continue;
        }
        if (arg == "--orders-log") {
            orders_log = true;
            continue;
        }
        if (!value)
            return usage();
        ++i;
        if (arg == "--incr-a" || arg == "--incr-b" || arg == "--snap-a" || arg == "--snap-b") {
            UdpEndpoint endpoint;
            if (!parse_endpoint(value, endpoint)) {
                std::fprintf(stderr, "bad endpoint: %s\n", value);
                return 2;
            }
            endpoints.emplace_back(arg.substr(2, 4) == "snap", endpoint);
        } else if (arg == "--interface") {
            interface = value;
        } else if (arg == "--security-id") {
            watch_id = std::strtoull(value, nullptr, 10);
        } else if (arg == "--seconds") {
            seconds = std::strtol(value, nullptr, 10);
        } else {
            return usage();
        }
    }
    bool have_incremental = false;
    for (const auto& endpoint : endpoints)
        have_incremental |= !endpoint.first;
    if (!have_incremental)
        return usage();

    UdpReceiver receiver;
    std::vector<bool> is_snapshot;
    std::vector<std::uint64_t> datagrams; // raw receipt per socket, before any sequencing or decoding
    for (auto& [snapshot, endpoint] : endpoints) {
        endpoint.interface = interface;
        if (receiver.add(endpoint) < 0) {
            std::fprintf(stderr, "%s\n", receiver.error().c_str());
            return 1;
        }
        is_snapshot.push_back(snapshot);
        datagrams.push_back(0);
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    // One loop for both kinds of stream; `describe` prints what is specific to the sink.
    const auto run = [&](auto& stream, auto&& describe) {
        const std::uint64_t started = now_ns();
        std::uint64_t next_report = started + 1'000'000'000ull;
        bool snapshots_joined = true;
        while (!stop_requested.load(std::memory_order_relaxed)) {
            receiver.poll(spin ? 0 : 1, [&](int index, const std::uint8_t* data, std::size_t size) {
                ++datagrams[static_cast<std::size_t>(index)];
                if (is_snapshot[static_cast<std::size_t>(index)])
                    stream.on_snapshot(data, size);
                else
                    stream.on_incremental(data, size, now_ns());
            });
            const std::uint64_t now = now_ns();
            stream.on_timer(now);
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
            std::printf("packets=%llu dup=%llu reordered=%llu gaps=%llu malformed=%llu unknown=%llu instruments=%zu "
                        "unsynced=%zu snapshots=%llu snapshot_feed=%s\n",
                        static_cast<unsigned long long>(stats.packets),
                        static_cast<unsigned long long>(stats.duplicates),
                        static_cast<unsigned long long>(stats.reordered), static_cast<unsigned long long>(stats.gaps),
                        static_cast<unsigned long long>(stats.malformed),
                        static_cast<unsigned long long>(stats.unknown_templates), stream.instruments(),
                        stream.unsynced(), static_cast<unsigned long long>(stats.snapshots_applied),
                        snapshots_joined ? "joined" : "left");
            std::printf("  recovery: too_old=%llu resyncs=%llu empty=%llu queued=%llu replayed=%llu dropped=%llu "
                        "instrument_gaps=%llu market_clears=%llu sequence_resets=%llu next_seq=%u\n",
                        static_cast<unsigned long long>(stats.snapshots_too_old),
                        static_cast<unsigned long long>(stats.snapshot_resyncs),
                        static_cast<unsigned long long>(stats.empty_adopted),
                        static_cast<unsigned long long>(stats.queued), static_cast<unsigned long long>(stats.replayed),
                        static_cast<unsigned long long>(stats.queue_dropped),
                        static_cast<unsigned long long>(stats.instrument_gaps),
                        static_cast<unsigned long long>(stats.market_clears),
                        static_cast<unsigned long long>(stats.sequence_resets), stream.next_sequence());
            std::printf("  datagrams");
            for (std::size_t index = 0; index < datagrams.size(); ++index)
                std::printf(" %s:%u=%llu", endpoints[index].second.group.c_str(), endpoints[index].second.port,
                            static_cast<unsigned long long>(datagrams[index]));
            std::printf("\n");
            describe();
            std::fflush(stdout);
            if (seconds > 0 && now - started >= static_cast<std::uint64_t>(seconds) * 1'000'000'000ull)
                break;
        }
    };

    if (orders_log) {
        OrderBookSink sink;
        OrdersLogStream<OrderBookSink> stream(sink);
        run(stream, [&] {
            const OrderBook::Stats& book = sink.book.stats();
            std::printf("  orders active=%zu added=%llu changed=%llu deleted=%llu unknown_change=%llu "
                        "unknown_delete=%llu\n",
                        sink.book.size(), static_cast<unsigned long long>(book.added),
                        static_cast<unsigned long long>(book.changed), static_cast<unsigned long long>(book.deleted),
                        static_cast<unsigned long long>(book.unknown_change),
                        static_cast<unsigned long long>(book.unknown_delete));
            for (std::uint32_t slot = 0; watch_id && slot < stream.instruments(); ++slot)
                if (stream.security_id(slot) == watch_id)
                    std::printf("  %llu %s active orders %zu\n", static_cast<unsigned long long>(watch_id),
                                stream.synced(slot) ? "synced" : "STALE", sink.book.count(slot));
        });
        return 0;
    }
    Sink sink;
    MarketStream<Sink> stream(sink);
    run(stream, [&] {
        std::printf("  book_updates=%llu trades=%llu\n", static_cast<unsigned long long>(sink.book_updates),
                    static_cast<unsigned long long>(sink.trades));
        for (std::uint32_t slot = 0; watch_id && slot < stream.instruments(); ++slot) {
            if (stream.security_id(slot) != watch_id)
                continue;
            const auto* bid = sink.books[slot].bids().best();
            const auto* ask = sink.books[slot].asks().best();
            std::printf("  %llu %s bid %.5f x %lld | ask %.5f x %lld\n", static_cast<unsigned long long>(watch_id),
                        stream.synced(slot) ? "synced" : "STALE", bid ? bid->price.to_double() : 0.0,
                        bid ? static_cast<long long>(bid->size) : 0, ask ? ask->price.to_double() : 0.0,
                        ask ? static_cast<long long>(ask->size) : 0);
        }
    });
    return 0;
}
