// Offline check of the FAST connector against a packet capture (classic pcap,
// Ethernet, micro- or nanosecond timestamps). Not a CTest: run it by hand.
//
//   fast_pcap_replay <capture.pcap>
//       survey: per destination group, packet counts, sequence range and template ids
//   fast_pcap_replay <capture.pcap> --incr G:P [--incr G:P] [--snap G:P] [--snap G:P] [--depth N]
//       replay one stream through MarketStream and report what it made of it
//   fast_pcap_replay <capture.pcap> --orders-log 1 --incr G:P --snap G:P
//       the same for ORDERS-LOG, maintaining the per-order book

#include "moex/fast/fast_book.hpp"
#include "moex/fast/fast_market_stream.hpp"
#include "moex/fast/fast_order_book.hpp"
#include "moex/fast/fast_udp.hpp"

#include <arpa/inet.h>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace moex::fast;

namespace {

struct Datagram {
    std::uint64_t time_ns;
    std::uint32_t destination; // host order
    std::uint16_t port;
    const std::uint8_t* data;
    std::size_t size;
};

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}
std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}
std::uint32_t le32(const std::uint8_t* p) {
    return read_preamble(p);
}

template <class Handler> bool for_each_datagram(const std::uint8_t* file, std::size_t size, Handler&& handler) {
    if (size < 24)
        return false;
    const std::uint32_t magic = le32(file);
    const bool nanoseconds = magic == 0xA1B23C4Du;
    if (!nanoseconds && magic != 0xA1B2C3D4u) {
        std::fprintf(stderr, "unsupported capture format (magic %08x)\n", magic);
        return false;
    }
    if (le32(file + 20) != 1) {
        std::fprintf(stderr, "unsupported link type %u\n", le32(file + 20));
        return false;
    }
    std::size_t offset = 24;
    while (offset + 16 <= size) {
        const std::uint64_t time_ns =
            std::uint64_t{le32(file + offset)} * 1'000'000'000ull +
            std::uint64_t{le32(file + offset + 4)} * (nanoseconds ? 1 : 1000);
        const std::uint32_t captured = le32(file + offset + 8);
        const std::uint8_t* frame = file + offset + 16;
        offset += 16 + std::size_t{captured};
        if (offset > size)
            break;
        std::size_t at = 12;
        while (at + 2 <= captured && be16(frame + at) == 0x8100)
            at += 4;
        if (at + 2 > captured || be16(frame + at) != 0x0800)
            continue;
        at += 2;
        if (at + 20 > captured || frame[at + 9] != 17)
            continue;
        const std::size_t header = std::size_t{frame[at] & 0x0Fu} * 4;
        const std::uint32_t destination = be32(frame + at + 16);
        at += header;
        if (at + 8 > captured)
            continue;
        const std::size_t length = be16(frame + at + 4);
        if (length < 8 || at + length > captured)
            continue;
        handler(Datagram{time_ns, destination, be16(frame + at + 2), frame + at + 8, length - 8});
    }
    return true;
}

std::string address(std::uint32_t destination, std::uint16_t port) {
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", destination >> 24, (destination >> 16) & 255,
                  (destination >> 8) & 255, destination & 255, port);
    return text;
}

int survey(const std::uint8_t* file, std::size_t size) {
    struct Group {
        std::uint64_t packets{}, bytes{}, bad{}, heartbeats{};
        std::uint32_t first_seq{}, last_seq{}, max_size{};
        std::uint64_t backwards{}, jumps{};
        std::map<std::uint32_t, std::uint64_t> templates;
    };
    std::map<std::pair<std::uint32_t, std::uint16_t>, Group> groups;
    if (!for_each_datagram(file, size, [&](const Datagram& d) {
            Group& group = groups[{d.destination, d.port}];
            if (d.size <= kPreambleSize) {
                ++group.bad;
                return;
            }
            const std::uint32_t seq = le32(d.data);
            Reader r{d.data + kPreambleSize, d.data + d.size};
            const std::uint32_t id = read_template_id(r);
            // The preamble must repeat the message's own MsgSeqNum.
            if (id == 0 || read_u32(r) != seq || !r.ok)
                ++group.bad;
            if (group.packets == 0)
                group.first_seq = seq;
            else if (seq < group.last_seq)
                ++group.backwards;
            else if (seq > group.last_seq + 1)
                ++group.jumps;
            group.last_seq = seq;
            ++group.packets;
            group.bytes += d.size;
            group.max_size = std::max<std::uint32_t>(group.max_size, static_cast<std::uint32_t>(d.size));
            ++group.templates[id];
        }))
        return 1;
    for (const auto& [key, group] : groups) {
        std::printf("%-22s packets=%-8" PRIu64 " seq=%u..%u backwards=%" PRIu64 " jumps=%" PRIu64 " max=%u bad=%" PRIu64
                    " templates:",
                    address(key.first, key.second).c_str(), group.packets, group.first_seq, group.last_seq,
                    group.backwards, group.jumps, group.max_size, group.bad);
        for (const auto& [id, count] : group.templates)
            std::printf(" %u=%" PRIu64, id, count);
        std::printf("\n");
    }
    return 0;
}

struct ReplaySink : StreamSink {
    std::vector<AggregateBook<50>> books;
    std::uint64_t book_entries{}, other_entries{}, snapshot_entries{}, clears{}, all_stale{}, resets{};
    std::uint64_t bad_level{}, unsorted{}, crossed{}, session_status{};
    std::map<char, std::uint64_t> types;
    std::uint32_t depth{50};
    std::uint64_t with_market_depth{}, depth_mismatch{};
    std::vector<std::uint32_t> touched;
    std::unordered_map<std::uint64_t, std::uint32_t> slots;
    std::vector<std::uint32_t> last_seq;   // MsgSeqNum of the last incremental applied to the book
    std::vector<std::uint32_t> since_sync; // incrementals applied since the book was last rebuilt

    void on_instrument(std::uint32_t slot, std::uint64_t id) {
        books.emplace_back().set_depth(depth);
        slots[id] = slot;
        last_seq.push_back(0);
        since_sync.push_back(0);
    }
    void on_book_clear(std::uint32_t slot) noexcept {
        books[slot].clear();
        since_sync[slot] = 0;
        ++clears;
    }
    void on_snapshot(std::uint32_t slot, const Snapshot&, const Snapshot::MDEntries& e) noexcept {
        books[slot].apply(e);
        ++snapshot_entries;
    }
    void on_incremental(std::uint32_t slot, const Incremental& m, const Incremental::MDEntries& e) {
        ++types[e.MDEntryType.front()];
        const char type = e.MDEntryType.front();
        if (type == '0' || type == '1') {
            // A level the book cannot address means our picture of it has drifted.
            const auto& side = type == '0' ? books[slot].bids() : books[slot].asks();
            const std::uint32_t level = e.MDPriceLevel.value_or(0);
            const std::uint32_t limit = e.MDUpdateAction == 0 ? side.count + 1 : side.count;
            bad_level += level == 0 || level > limit || level > depth;
            with_market_depth += e.MarketDepth.present;
            depth_mismatch += e.MarketDepth.present && e.MarketDepth.value != depth;
        }
        last_seq[slot] = m.MsgSeqNum;
        ++since_sync[slot];
        if (books[slot].apply(e)) {
            ++book_entries;
            touched.push_back(slot);
        } else {
            ++other_entries;
        }
    }
    void on_all_stale() noexcept {
        ++all_stale;
    }
    void on_sequence_reset() noexcept {
        ++resets;
        for (auto& book : books)
            book.clear();
        for (auto& seq : last_seq)
            seq = 0;
    }
    void on_session_status(const TradingSessionStatus&) noexcept {
        ++session_status;
    }
    // Between events the book must be price-ordered on each side.
    void on_batch_end() {
        for (const std::uint32_t slot : touched) {
            const auto& book = books[slot];
            for (std::uint32_t i = 1; i < book.bids().count; ++i)
                unsorted += book.bids().levels[i].price.to_double() >= book.bids().levels[i - 1].price.to_double();
            for (std::uint32_t i = 1; i < book.asks().count; ++i)
                unsorted += book.asks().levels[i].price.to_double() <= book.asks().levels[i - 1].price.to_double();
            if (book.bids().count && book.asks().count)
                crossed += book.bids().levels[0].price.to_double() >= book.asks().levels[0].price.to_double();
        }
        touched.clear();
    }
};

struct OrdersSink : OrderBookSink {
    std::unordered_map<std::uint64_t, std::uint32_t> slots;
    std::vector<std::uint32_t> last_seq, since_sync;
    std::uint64_t entries{}, book_changes{}, trades{}, all_stale{}, resets{}, market_clears{};

    void on_instrument(std::uint32_t slot, std::uint64_t id) {
        slots[id] = slot;
        last_seq.push_back(0);
        since_sync.push_back(0);
    }
    void on_book_clear(std::uint32_t slot) noexcept {
        OrderBookSink::on_book_clear(slot);
        since_sync[slot] = 0;
    }
    void on_incremental(std::uint32_t slot, const Incremental& m, const Incremental::MDEntries& e) {
        ++entries;
        last_seq[slot] = m.MsgSeqNum;
        ++since_sync[slot];
        book_changes += book.apply(slot, e);
        trades += e.TradeID.present;
    }
    void on_market_clear(std::uint32_t slot, const Incremental& m, const Incremental::MDEntries& e) noexcept {
        OrderBookSink::on_market_clear(slot, m, e);
        last_seq[slot] = m.MsgSeqNum;
        ++market_clears;
    }
    void on_all_stale() noexcept {
        ++all_stale;
    }
    void on_sequence_reset() noexcept {
        OrderBookSink::on_sequence_reset();
        ++resets;
        for (auto& seq : last_seq)
            seq = 0;
    }
};

int replay_orders_log(const std::uint8_t* file, std::size_t size, const std::unordered_map<std::uint64_t, int>& roles) {
    OrdersSink sink;
    OrdersLogStream<OrdersSink> stream(sink);
    std::uint64_t incremental = 0, snapshot = 0, first_ns = 0, last_ns = 0, first_synced_ns = 0;
    std::size_t max_unsynced = 0, max_orders = 0;
    // Audit: an exchange snapshot lists an instrument's active orders as of LastMsgSeqNumProcessed.
    // Where our book was built from incrementals and is unchanged since then, the sets must match.
    struct Expected {
        Decimal price;
        std::int64_t size;
        bool bid;
    };
    std::unordered_map<std::int64_t, Expected> expected;
    std::uint64_t expected_id = 0, audited = 0, audit_orders = 0, audit_mismatch = 0;
    bool printed = false;
    const auto audit = [&](const Datagram& d) {
        Reader r{d.data + kPreambleSize, d.data + d.size};
        if (read_template_id(r) != BookMessage::kTemplateId)
            return;
        BookMessage message;
        bool first = true;
        if (!decode(r, message, [&](const auto& m, const auto& e) {
                if (first && (m.RouteFirst || m.SecurityID.value != expected_id))
                    expected.clear();
                first = false;
                const char type = e.MDEntryType.front();
                if ((type == '0' || type == '1') && e.MDEntryID.present && !(e.MDFlags.value_or(0) & OrderBook::kOtcFlag))
                    expected[e.MDEntryID.value] = {e.MDEntryPx.value, e.MDEntrySize.value_or(0), type == '0'};
            }) ||
            !message.SecurityID.present)
            return;
        if (first && message.RouteFirst)
            expected.clear();
        expected_id = message.SecurityID.value;
        const auto found = sink.slots.find(expected_id);
        if (!message.LastFragment || found == sink.slots.end())
            return;
        const std::uint32_t slot = found->second;
        if (!stream.synced(slot) || sink.since_sync[slot] == 0 || sink.last_seq[slot] > message.LastMsgSeqNumProcessed ||
            stream.next_sequence() <= message.LastMsgSeqNumProcessed)
            return;
        ++audited;
        audit_orders += expected.size();
        bool same = sink.book.count(slot) == expected.size();
        sink.book.for_each(slot, [&](const OrderBook::Order& o) {
            const auto it = expected.find(o.id);
            same = same && it != expected.end() && it->second.price == o.price && it->second.size == o.size &&
                   it->second.bid == o.bid;
        });
        audit_mismatch += !same;
        if (!same && !printed) {
            printed = true;
            std::printf("first audit mismatch: security %" PRIu64 " ours %zu orders, snapshot %zu, L=%u last_seq=%u\n",
                        expected_id, sink.book.count(slot), expected.size(), message.LastMsgSeqNumProcessed,
                        sink.last_seq[slot]);
        }
    };
    if (!for_each_datagram(file, size, [&](const Datagram& d) {
            const auto role = roles.find(std::uint64_t{d.destination} << 16 | d.port);
            if (role == roles.end())
                return;
            if (!first_ns)
                first_ns = d.time_ns;
            last_ns = d.time_ns;
            if (role->second == 1) {
                ++incremental;
                stream.on_incremental(d.data, d.size, d.time_ns);
            } else {
                ++snapshot;
                audit(d);
                stream.on_snapshot(d.data, d.size);
            }
            max_unsynced = std::max(max_unsynced, stream.unsynced());
            max_orders = std::max(max_orders, sink.book.size());
            if (!first_synced_ns && stream.instruments() && !stream.needs_snapshot())
                first_synced_ns = d.time_ns;
        }))
        return 1;
    const StreamStats& s = stream.stats();
    const OrderBook::Stats& b = sink.book.stats();
    std::printf("capture span %.1f s, incremental datagrams %" PRIu64 ", snapshot datagrams %" PRIu64 "\n",
                double(last_ns - first_ns) / 1e9, incremental, snapshot);
    std::printf("stream: packets=%" PRIu64 " gaps=%" PRIu64 " malformed=%" PRIu64 " unknown_templates=%" PRIu64
                " instrument_gaps=%" PRIu64 " unkeyed_entries=%" PRIu64 " market_clears=%" PRIu64
                " sequence_resets=%" PRIu64 "\n",
                s.packets, s.gaps, s.malformed, s.unknown_templates, s.instrument_gaps, s.unkeyed_entries,
                s.market_clears, s.sequence_resets);
    std::printf("recovery: queued=%" PRIu64 " replayed=%" PRIu64 " queue_dropped=%" PRIu64 " replay_passes=%" PRIu64
                " snapshots_applied=%" PRIu64 " too_old=%" PRIu64 " empty_adopted=%" PRIu64 " resyncs=%" PRIu64 "\n",
                s.queued, s.replayed, s.queue_dropped, s.replay_passes, s.snapshots_applied, s.snapshots_too_old,
                s.empty_adopted, s.snapshot_resyncs);
    std::printf("instruments=%zu unsynced_at_end=%zu max_unsynced=%zu needs_snapshot_at_end=%d", stream.instruments(),
                stream.unsynced(), max_unsynced, stream.needs_snapshot());
    if (first_synced_ns)
        std::printf(" fully_synced_after=%.2fs", double(first_synced_ns - first_ns) / 1e9);
    std::printf("\nentries=%" PRIu64 " book_changes=%" PRIu64 " with_trade=%" PRIu64 " per_instrument_market_clears=%" PRIu64
                "\n",
                sink.entries, sink.book_changes, sink.trades, sink.market_clears);
    std::printf("order book: active_at_end=%zu peak=%zu added=%" PRIu64 " changed=%" PRIu64 " deleted=%" PRIu64
                " readded=%" PRIu64 " unknown_change=%" PRIu64 " unknown_delete=%" PRIu64 "\n",
                sink.book.size(), max_orders, b.added, b.changed, b.deleted, b.readded, b.unknown_change,
                b.unknown_delete);
    std::printf("audit against exchange snapshots: instruments compared=%" PRIu64 " (orders %" PRIu64 "), mismatches=%" PRIu64
                "\n",
                audited, audit_orders, audit_mismatch);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: fast_pcap_replay <capture.pcap> [--incr G:P]... [--snap G:P]... [--depth N]\n", stderr);
        return 2;
    }
    std::unordered_map<std::uint64_t, int> roles; // (ip << 16 | port) -> 1 incremental, 2 snapshot
    std::uint32_t depth = 50;
    bool orders_log = false;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        if (flag == "--orders-log") {
            orders_log = true;
            continue;
        }
        if (flag == "--depth") {
            depth = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
            continue;
        }
        UdpEndpoint endpoint;
        in_addr parsed{};
        if ((flag != "--incr" && flag != "--snap") || !parse_endpoint(argv[i + 1], endpoint) ||
            inet_pton(AF_INET, endpoint.group.c_str(), &parsed) != 1) {
            std::fprintf(stderr, "bad argument: %s %s\n", argv[i], argv[i + 1]);
            return 2;
        }
        roles[std::uint64_t{ntohl(parsed.s_addr)} << 16 | endpoint.port] = flag == "--incr" ? 1 : 2;
    }
    const int fd = ::open(argv[1], O_RDONLY);
    struct stat info {};
    if (fd < 0 || ::fstat(fd, &info) != 0) {
        std::perror(argv[1]);
        return 1;
    }
    const auto size = static_cast<std::size_t>(info.st_size);
    void* mapping = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED) {
        std::perror("mmap");
        return 1;
    }
    const auto* file = static_cast<const std::uint8_t*>(mapping);
    if (roles.empty())
        return survey(file, size);
    if (orders_log)
        return replay_orders_log(file, size, roles);

    ReplaySink sink;
    sink.depth = depth;
    MarketStream<ReplaySink> stream(sink);
    std::uint64_t incremental = 0, snapshot = 0, first_synced_ns = 0, first_ns = 0, last_ns = 0;
    std::size_t max_unsynced = 0;
    // Independent audit: the exchange's snapshot states a book as of LastMsgSeqNumProcessed.
    // If our book for that instrument was built from incrementals and has not changed
    // since that point, the two must be identical.
    std::uint64_t audited = 0, audit_mismatch = 0, audit_levels = 0;
    AggregateBook<50> expected_book;
    std::uint64_t expected_id = 0;
    bool expected_open = false, printed = false;
    const auto audit = [&](const Datagram& d) {
        Reader r{d.data + kPreambleSize, d.data + d.size};
        if (read_template_id(r) != DefaultSnapshotMessage::kTemplateId)
            return;
        DefaultSnapshotMessage message;
        bool first = true, bookish = false;
        if (!decode(r, message, [&](const auto& m, const auto& e) {
                if (first && !(expected_open && expected_id == m.SecurityID.value))
                    expected_book.clear();
                first = false;
                bookish |= expected_book.apply(e);
            }) ||
            !message.SecurityID.present)
            return;
        expected_id = message.SecurityID.value;
        expected_open = message.LastFragment.present && message.LastFragment.value == 0;
        const auto found = sink.slots.find(expected_id);
        if (expected_open || !bookish || found == sink.slots.end())
            return;
        const std::uint32_t slot = found->second;
        if (!stream.synced(slot) || sink.since_sync[slot] == 0 || sink.last_seq[slot] > message.LastMsgSeqNumProcessed ||
            stream.next_sequence() <= message.LastMsgSeqNumProcessed)
            return;
        ++audited;
        const auto& ours = sink.books[slot];
        bool same = ours.bids().count == expected_book.bids().count && ours.asks().count == expected_book.asks().count;
        for (std::uint32_t i = 0; same && i < ours.bids().count; ++i)
            same = ours.bids().levels[i].price == expected_book.bids().levels[i].price &&
                   ours.bids().levels[i].size == expected_book.bids().levels[i].size;
        for (std::uint32_t i = 0; same && i < ours.asks().count; ++i)
            same = ours.asks().levels[i].price == expected_book.asks().levels[i].price &&
                   ours.asks().levels[i].size == expected_book.asks().levels[i].size;
        audit_levels += ours.bids().count + ours.asks().count;
        audit_mismatch += !same;
        if (!same && !printed) {
            printed = true;
            std::printf("first audit mismatch: security %" PRIu64 " ours %u/%u levels, snapshot %u/%u, L=%u last_seq=%u\n",
                        expected_id, ours.bids().count, ours.asks().count, expected_book.bids().count,
                        expected_book.asks().count, message.LastMsgSeqNumProcessed, sink.last_seq[slot]);
        }
    };
    if (!for_each_datagram(file, size, [&](const Datagram& d) {
            const auto role = roles.find(std::uint64_t{d.destination} << 16 | d.port);
            if (role == roles.end())
                return;
            if (!first_ns)
                first_ns = d.time_ns;
            last_ns = d.time_ns;
            if (role->second == 1) {
                ++incremental;
                stream.on_incremental(d.data, d.size, d.time_ns);
            } else {
                ++snapshot;
                audit(d);
                stream.on_snapshot(d.data, d.size);
            }
            max_unsynced = std::max(max_unsynced, stream.unsynced());
            if (!first_synced_ns && stream.instruments() && !stream.needs_snapshot())
                first_synced_ns = d.time_ns;
        }))
        return 1;
    const StreamStats& s = stream.stats();
    std::printf("capture span %.1f s, incremental datagrams %" PRIu64 ", snapshot datagrams %" PRIu64 "\n",
                double(last_ns - first_ns) / 1e9, incremental, snapshot);
    std::printf("stream: packets=%" PRIu64 " duplicates=%" PRIu64 " reordered=%" PRIu64 " gaps=%" PRIu64
                " malformed=%" PRIu64 " unknown_templates=%" PRIu64 " instrument_gaps=%" PRIu64 "\n",
                s.packets, s.duplicates, s.reordered, s.gaps, s.malformed, s.unknown_templates, s.instrument_gaps);
    std::printf("        sequence_resets=%" PRIu64 " unkeyed_entries=%" PRIu64 " queued=%" PRIu64 " replayed=%" PRIu64
                " queue_dropped=%" PRIu64 " replay_passes=%" PRIu64 "\n",
                s.sequence_resets, s.unkeyed_entries, s.queued, s.replayed, s.queue_dropped, s.replay_passes);
    std::printf("snapshots: packets=%" PRIu64 " resyncs=%" PRIu64 " applied=%" PRIu64 " too_old=%" PRIu64
                " empty_adopted=%" PRIu64 "\n",
                s.snapshot_packets, s.snapshot_resyncs, s.snapshots_applied, s.snapshots_too_old, s.empty_adopted);
    std::printf("instruments=%zu unsynced_at_end=%zu max_unsynced=%zu needs_snapshot_at_end=%d", stream.instruments(),
                stream.unsynced(), max_unsynced, stream.needs_snapshot());
    if (first_synced_ns)
        std::printf(" fully_synced_after=%.2fs", double(first_synced_ns - first_ns) / 1e9);
    std::printf("\nsink: book_entries=%" PRIu64 " other_entries=%" PRIu64 " snapshot_entries=%" PRIu64
                " all_stale=%" PRIu64 " resets=%" PRIu64 " session_status=%" PRIu64 "\n",
                sink.book_entries, sink.other_entries, sink.snapshot_entries, sink.all_stale, sink.resets,
                sink.session_status);
    std::printf("checks: bad_level=%" PRIu64 " unsorted=%" PRIu64 " crossed=%" PRIu64 " entries_with_MarketDepth=%" PRIu64
                " depth_mismatch=%" PRIu64 "\nentry types:",
                sink.bad_level, sink.unsorted, sink.crossed, sink.with_market_depth, sink.depth_mismatch);
    for (const auto& [type, count] : sink.types)
        std::printf(" '%c'=%" PRIu64, type ? type : '?', count);
    std::printf("\naudit against exchange snapshots: books compared=%" PRIu64 " (levels %" PRIu64 "), mismatches=%" PRIu64,
                audited, audit_levels, audit_mismatch);
    std::size_t with_bid = 0, with_ask = 0;
    for (const auto& book : sink.books) {
        with_bid += book.bids().count > 0;
        with_ask += book.asks().count > 0;
    }
    std::printf("\nbooks with bids=%zu, with asks=%zu\n", with_bid, with_ask);
    return 0;
}
