// Offline check of the ASTS connector against a packet capture. Not a CTest: run it by hand.
//
//   asts_pcap_replay <capture.pcap>
//       survey: per destination group, packet counts, sequence range, template ids, undecodable packets
//   asts_pcap_replay <capture.pcap> --market curr|fond --kind orders|trades --incr G:P --snap G:P
//                    [--join-after SECONDS] [--drop-every N] [--replay-dropped PACKETS]
//       replay one stream pair and audit the result against the capture's own snapshots.
//       --join-after withholds the first seconds of incrementals (a late join);
//       --drop-every discards every Nth incremental packet (forced loss);
//       --replay-dropped hands each lost range back, as a TCP replay would, that many
//       incremental packets after the loss was noticed.

#include "moex/fast/asts/asts_order_book.hpp"
#include "moex/fast/asts/asts_stream.hpp"
#include "moex/fast/fast_udp.hpp"
#include "pcap_reader.hpp"

#include <arpa/inet.h>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace moex::fast;
using namespace moex::fast::asts;

namespace {

int survey(const pcap::File& file) {
    struct Group {
        std::uint64_t packets{}, bad{}, leftover{}, backwards{}, jumps{};
        std::uint32_t first{}, last{};
        std::map<std::uint32_t, std::uint64_t> templates;
    };
    std::map<std::pair<std::uint32_t, std::uint16_t>, Group> groups;
    const auto ignore = [](const auto&, const auto&) {};
    if (!file.for_each([&](const pcap::Datagram& d) {
            Group& group = groups[{d.destination, d.port}];
            if (d.size <= kPreambleSize) {
                ++group.bad;
                return;
            }
            const std::uint32_t seq = read_preamble(d.data);
            Reader r{d.data + kPreambleSize, d.data + d.size};
            std::uint64_t pmap;
            const std::uint32_t id = read_header(r, pmap);
            bool ok = false;
#define ASTS_PLAIN(Type)                                                                                               \
    case Type::kTemplateId: {                                                                                          \
        Type m;                                                                                                        \
        ok = decode(r, pmap, m) && m.MsgSeqNum == seq;                                                                 \
        break;                                                                                                         \
    }
#define ASTS_GROUPS(Type)                                                                                              \
    case Type::kTemplateId: {                                                                                          \
        Type m;                                                                                                        \
        ok = decode(r, pmap, m, ignore) && m.MsgSeqNum == seq;                                                         \
        break;                                                                                                         \
    }
            switch (id) {
                ASTS_PLAIN(Logon)
                ASTS_PLAIN(Logout)
                ASTS_PLAIN(Heartbeat)
                ASTS_PLAIN(SecurityStatus)
                ASTS_PLAIN(TradingSessionStatus)
                ASTS_GROUPS(SecurityDefinition)
                ASTS_GROUPS(W_Generic)
                ASTS_GROUPS(X_Generic)
                ASTS_GROUPS(W_OLS_FOND)
                ASTS_GROUPS(W_OLS_CURR)
                ASTS_GROUPS(W_TLS_FOND)
                ASTS_GROUPS(W_TLS_CURR)
                ASTS_GROUPS(X_MSR_FOND)
                ASTS_GROUPS(X_MSR_CURR)
                ASTS_GROUPS(X_OLR_FOND)
                ASTS_GROUPS(X_OLR_CURR)
                ASTS_GROUPS(X_TLR_FOND)
                ASTS_GROUPS(X_TLR_CURR)
            default:
                break;
            }
            // A correct decode consumes the packet exactly and reproduces the preamble's sequence number.
            if (!ok)
                ++group.bad;
            else if (r.p != r.end)
                ++group.leftover;
            if (group.packets == 0)
                group.first = seq;
            else if (seq < group.last)
                ++group.backwards;
            else if (seq > group.last + 1)
                ++group.jumps;
            group.last = seq;
            ++group.packets;
            ++group.templates[id];
        }))
        return 1;
    for (const auto& [key, group] : groups) {
        std::printf("%-22s packets=%-8" PRIu64 " seq=%u..%u backwards=%" PRIu64 " jumps=%" PRIu64 " undecodable=%" PRIu64
                    " leftover=%" PRIu64 " templates:",
                    pcap::address(key.first, key.second).c_str(), group.packets, group.first, group.last,
                    group.backwards, group.jumps, group.bad, group.leftover);
        for (const auto& [id, count] : group.templates)
            std::printf(" %s=%" PRIu64, std::string(template_name(id)).c_str(), count);
        std::printf("\n");
    }
    return 0;
}

struct Options {
    std::unordered_map<std::uint64_t, int> roles; // (ip << 16 | port) -> 1 incremental, 2 snapshot
    double join_after{};
    std::uint64_t drop_every{};
    std::uint64_t replay_after{}; // 0: no replay
};

// What the exchange's own snapshot says an instrument holds.
struct Expected {
    std::string id;
    char side;
    Decimal price, size;
};

template <class Incremental, class Snapshot, bool Orders> struct ReplaySink : StreamSink<Incremental, Snapshot> {
    using IncrementalEntry = typename Incremental::GroupMDEntries;
    using SnapshotEntry = typename Snapshot::GroupMDEntries;

    OrderBook book;
    std::vector<std::uint64_t> items;      // trades per instrument (trade streams)
    std::vector<std::uint32_t> since_sync; // incrementals applied since the instrument was last rebuilt
    std::uint64_t incrementals{}, snapshot_entries{}, clears{}, all_stale{}, rejected{}, session_status{};
    std::map<std::string, std::uint64_t> kinds;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> losses;

    void on_loss(std::uint32_t first, std::uint32_t last) {
        losses.emplace_back(first, last);
    }

    void on_instrument(std::uint32_t, std::string_view, std::string_view) {
        items.push_back(0);
        since_sync.push_back(0);
    }
    void on_clear(std::uint32_t slot) {
        if constexpr (Orders)
            book.clear(slot);
        items[slot] = 0;
        since_sync[slot] = 0;
        ++clears;
    }
    void on_snapshot(std::uint32_t slot, const Snapshot&, const SnapshotEntry& e) {
        ++snapshot_entries;
        if constexpr (Orders)
            rejected += !book.apply_snapshot(slot, e);
        else
            items[slot] += e.MDEntryType.front() != 'J';
    }
    void on_incremental(std::uint32_t slot, const Incremental&, const IncrementalEntry& e) {
        ++incrementals;
        ++since_sync[slot];
        if constexpr (Orders) {
            ++kinds[std::string(1, e.MDEntryType.value.front()) + (e.MDUpdateAction.value_or(0) == 0   ? " add"
                                                                    : e.MDUpdateAction.value == 1 ? " change"
                                                                                                  : " delete")];
            rejected += !book.apply(slot, e);
        } else {
            ++kinds[std::string(1, e.MDEntryType.front())];
            items[slot] += e.MDEntryType.front() != 'J';
        }
    }
    void on_all_stale() {
        ++all_stale;
    }
    void on_session_status(const TradingSessionStatus&) {
        ++session_status;
    }
};

template <class Incremental, class Snapshot, bool Orders> int replay(const pcap::File& file, const Options& options) {
    using Sink = ReplaySink<Incremental, Snapshot, Orders>;
    Sink sink;
    StreamConfig config;
    config.tcp_replay = options.replay_after != 0;
    Stream<Incremental, Snapshot, Sink> stream(sink, config);
    // Stand-in for the gate's replay service: the packets this run withheld, by sequence number.
    std::unordered_map<std::uint32_t, pcap::Datagram> withheld_packets;
    struct Request {
        std::uint32_t first, last;
        std::uint64_t due;
    };
    std::vector<Request> requests;
    std::size_t losses_seen = 0;
    std::uint64_t ranges_recovered = 0, ranges_failed = 0;
    std::uint64_t incremental = 0, withheld = 0, dropped = 0, snapshot = 0, first_ns = 0, last_ns = 0, synced_ns = 0;
    std::size_t max_unsynced = 0;

    // Independent audit. A snapshot states an instrument as of its RptSeq; when our
    // instrument is in sync at exactly that RptSeq the two must hold the same thing.
    std::uint64_t audited = 0, audited_independent = 0, audit_mismatch = 0, audited_items = 0;
    std::vector<Expected> expected;
    std::string expected_key;
    bool expected_open = false, printed = false;
    const auto audit = [&](const pcap::Datagram& d) {
        Reader r{d.data + kPreambleSize, d.data + d.size};
        std::uint64_t pmap;
        if (read_header(r, pmap) != Snapshot::kTemplateId)
            return;
        Snapshot message;
        std::vector<Expected> entries;
        if (!decode(r, pmap, message, [&](const auto&, const auto& e) {
                if constexpr (Orders) {
                    if (e.MDEntryType.value.front() != 'J')
                        entries.push_back(
                            {e.MDEntryID.value.str(), e.MDEntryType.value.front(), e.MDEntryPx.value, e.MDEntrySize.value});
                } else if (e.MDEntryType.front() != 'J') {
                    entries.push_back({e.MDEntryID.value.str(), 'z', {}, {}});
                }
            }))
            return;
        const std::string symbol = message.Symbol.str();
        const std::string board = message.TradingSessionID.present ? message.TradingSessionID.value.str() : std::string();
        const std::string key = symbol + "|" + board;
        if (message.RouteFirst.value_or(1) != 0) {
            expected.clear();
            expected_key = key;
            expected_open = true;
        } else if (!expected_open || expected_key != key) {
            expected_open = false;
            return;
        }
        expected.insert(expected.end(), entries.begin(), entries.end());
        if (message.LastFragment.value_or(1) == 0)
            return;
        expected_open = false;
        const std::uint32_t slot = stream.find(symbol, board);
        if (slot == UINT32_MAX || !stream.synced(slot) || stream.rpt_seq(slot) != static_cast<std::uint32_t>(message.RptSeq))
            return;
        bool same;
        if constexpr (Orders) {
            same = sink.book.orders(slot) == expected.size();
            for (std::size_t i = 0; same && i < expected.size(); ++i) {
                const Order* order = sink.book.find(slot, expected[i].id);
                same = order && order->side == expected[i].side && order->price == expected[i].price &&
                       order->size == expected[i].size;
            }
        } else {
            same = sink.items[slot] == expected.size();
        }
        ++audited;
        audited_independent += sink.since_sync[slot] > 0;
        audited_items += expected.size();
        audit_mismatch += !same;
        if (!same && !printed) {
            printed = true;
            std::printf("first audit mismatch: %s RptSeq %d: snapshot holds %zu, we hold %zu\n", key.c_str(),
                        message.RptSeq, expected.size(),
                        Orders ? sink.book.orders(slot) : static_cast<std::size_t>(sink.items[slot]));
        }
    };

    if (!file.for_each([&](const pcap::Datagram& d) {
            const auto role = options.roles.find(std::uint64_t{d.destination} << 16 | d.port);
            if (role == options.roles.end() || d.size <= kPreambleSize)
                return;
            if (!first_ns)
                first_ns = d.time_ns;
            last_ns = d.time_ns;
            if (role->second == 1) {
                if (double(d.time_ns - first_ns) / 1e9 < options.join_after) {
                    ++withheld;
                    return;
                }
                if (options.drop_every && ++incremental % options.drop_every == 0) {
                    ++dropped;
                    withheld_packets[read_preamble(d.data)] = d;
                    return;
                }
                incremental += options.drop_every == 0;
                stream.on_incremental(d.data, d.size, d.time_ns);
                for (; losses_seen < sink.losses.size(); ++losses_seen)
                    requests.push_back({sink.losses[losses_seen].first, sink.losses[losses_seen].second,
                                        incremental + options.replay_after});
                for (std::size_t i = 0; i < requests.size();) {
                    if (requests[i].due > incremental) {
                        ++i;
                        continue;
                    }
                    // The capture itself has holes; only what this run withheld can be "replayed".
                    bool ok = true;
                    for (std::uint32_t seq = requests[i].first; ok && seq <= requests[i].last; ++seq)
                        ok = withheld_packets.count(seq) != 0;
                    for (std::uint32_t seq = requests[i].first; ok && seq <= requests[i].last; ++seq)
                        stream.on_recovered(withheld_packets[seq].data + kPreambleSize,
                                            withheld_packets[seq].size - kPreambleSize);
                    stream.replay_finished(requests[i].first, requests[i].last, ok);
                    ++(ok ? ranges_recovered : ranges_failed);
                    requests.erase(requests.begin() + static_cast<std::ptrdiff_t>(i));
                }
            } else {
                ++snapshot;
                audit(d);
                stream.on_snapshot(d.data, d.size);
            }
            stream.on_timer(d.time_ns);
            max_unsynced = std::max(max_unsynced, stream.unsynced());
            if (stream.needs_snapshot())
                synced_ns = 0;
            else if (!synced_ns && stream.instruments())
                synced_ns = d.time_ns;
        }))
        return 1;

    const StreamStats& s = stream.stats();
    std::printf("capture span %.1f s; incremental packets fed %" PRIu64 " (withheld %" PRIu64 ", dropped %" PRIu64
                "), snapshot packets %" PRIu64 "\n",
                double(last_ns - first_ns) / 1e9, incremental, withheld, dropped, snapshot);
    std::printf("stream: packets=%" PRIu64 " malformed=%" PRIu64 " unknown_templates=%" PRIu64 " unkeyed=%" PRIu64
                " losses=%" PRIu64 " restarts=%" PRIu64 " instrument_gaps=%" PRIu64 " duplicates=%" PRIu64 "\n",
                s.packets, s.malformed, s.unknown_templates, s.unkeyed_entries, s.losses, s.restarts, s.instrument_gaps,
                stream.arbiter().duplicates);
    std::printf("recovery: queued=%" PRIu64 " replayed=%" PRIu64 " passes=%" PRIu64 " queue_dropped=%" PRIu64
                " resumed_without_snapshot=%" PRIu64 " confirmed_by_snapshot=%" PRIu64 "\n",
                s.queued, s.replayed, s.replay_passes, s.queue_dropped, s.resumed_without_snapshot,
                s.confirmed_by_snapshot);
    std::printf("tcp replay: ranges recovered=%" PRIu64 " failed=%" PRIu64 " packets=%" PRIu64
                " instruments confirmed=%" PRIu64 "\n",
                ranges_recovered, ranges_failed, s.recovered_packets, s.confirmed_by_replay);
    std::printf("snapshots: packets=%" PRIu64 " cycles=%" PRIu64 " resyncs=%" PRIu64 " applied=%" PRIu64
                " too_old=%" PRIu64 "\n",
                s.snapshot_packets, s.snapshot_cycles, s.snapshot_resyncs, s.snapshots_applied, s.snapshots_too_old);
    std::printf("instruments=%zu unsynced_at_end=%zu max_unsynced=%zu needs_snapshot_at_end=%d", stream.instruments(),
                stream.unsynced(), max_unsynced, stream.needs_snapshot());
    if (synced_ns)
        std::printf(" in_sync_since=%.2fs", double(synced_ns - first_ns) / 1e9);
    std::printf("\nsink: incrementals=%" PRIu64 " snapshot_entries=%" PRIu64 " clears=%" PRIu64 " all_stale=%" PRIu64
                " rejected=%" PRIu64 " live_orders=%zu\n",
                sink.incrementals, sink.snapshot_entries, sink.clears, sink.all_stale, sink.rejected, sink.book.orders());
    std::printf("audit against exchange snapshots: instruments compared=%" PRIu64 " (built from incrementals: %" PRIu64
                "), items compared=%" PRIu64 ", mismatches=%" PRIu64 "\nentries:",
                audited, audited_independent, audited_items, audit_mismatch);
    for (const auto& [kind, count] : sink.kinds)
        std::printf(" '%s'=%" PRIu64, kind.c_str(), count);
    std::printf("\n");
    return audit_mismatch == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: asts_pcap_replay <capture.pcap> [--market curr|fond --kind orders|trades --incr G:P "
                   "--snap G:P [--join-after SECONDS] [--drop-every N]]\n",
                   stderr);
        return 2;
    }
    Options options;
    std::string market = "curr", kind = "orders";
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        if (flag == "--market") {
            market = argv[i + 1];
        } else if (flag == "--kind") {
            kind = argv[i + 1];
        } else if (flag == "--join-after") {
            options.join_after = std::strtod(argv[i + 1], nullptr);
        } else if (flag == "--drop-every") {
            options.drop_every = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (flag == "--replay-dropped") {
            options.replay_after = std::strtoull(argv[i + 1], nullptr, 10);
        } else {
            UdpEndpoint endpoint;
            in_addr parsed{};
            if ((flag != "--incr" && flag != "--snap") || !parse_endpoint(argv[i + 1], endpoint) ||
                inet_pton(AF_INET, endpoint.group.c_str(), &parsed) != 1) {
                std::fprintf(stderr, "bad argument: %s %s\n", argv[i], argv[i + 1]);
                return 2;
            }
            options.roles[std::uint64_t{ntohl(parsed.s_addr)} << 16 | endpoint.port] = flag == "--incr" ? 1 : 2;
        }
    }
    pcap::File file;
    if (!file.open(argv[1]))
        return 1;
    if (options.roles.empty())
        return survey(file);
    if (market == "curr" && kind == "orders")
        return replay<X_OLR_CURR, W_OLS_CURR, true>(file, options);
    if (market == "curr" && kind == "trades")
        return replay<X_TLR_CURR, W_TLS_CURR, false>(file, options);
    if (market == "fond" && kind == "orders")
        return replay<X_OLR_FOND, W_OLS_FOND, true>(file, options);
    if (market == "fond" && kind == "trades")
        return replay<X_TLR_FOND, W_TLS_FOND, false>(file, options);
    std::fputs("unknown --market/--kind\n", stderr);
    return 2;
}
