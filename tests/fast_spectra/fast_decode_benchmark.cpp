// Decode and full-path throughput for the FAST connector. Not a CTest: run it by hand.

#include "fast_test_encoder.hpp"
#include "moex/fast/fast_book.hpp"
#include "moex/fast/fast_market_stream.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {
bool counting = false;
std::uint64_t allocations = 0;
} // namespace

void* operator new(std::size_t size) {
    allocations += counting;
    if (void* pointer = std::malloc(size ? size : 1))
        return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

using namespace moex::fast;

namespace {

struct BookSink : StreamSink {
    std::vector<AggregateBook<20>> books;
    std::uint64_t applied{};

    void on_instrument(std::uint32_t, std::uint64_t) {
        books.emplace_back();
    }
    void on_book_clear(std::uint32_t slot) {
        books[slot].clear();
    }
    void on_incremental(std::uint32_t slot, const Incremental&, const Incremental::MDEntries& e) {
        books[slot].apply(e);
        ++applied;
    }
};

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main() {
    constexpr std::uint32_t kPackets = 200000;
    constexpr std::uint32_t kInstruments = 256;
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto next = [&](std::uint64_t bound) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state % bound;
    };
    std::vector<std::uint32_t> rpt_seq(kInstruments);
    std::vector<fast_test::Bytes> packets;
    packets.reserve(kPackets);
    std::uint64_t entries = 0, bytes = 0;
    for (std::uint32_t seq = 1; seq <= kPackets; ++seq) {
        std::vector<fast_test::IncrementalEntry> batch;
        const std::uint64_t instrument = next(kInstruments);
        for (std::uint64_t n = 1 + next(4); n > 0; --n) {
            fast_test::IncrementalEntry entry;
            entry.action = static_cast<std::uint32_t>(next(3));
            entry.type = next(2) ? '0' : '1';
            entry.security_id = 3000000 + instrument;
            entry.rpt_seq = ++rpt_seq[instrument];
            entry.level = 1 + next(20);
            entry.price = fast_test::Dec{9000000 + static_cast<std::int64_t>(next(20000)), -2};
            entry.size = 1 + static_cast<std::int64_t>(next(500));
            batch.push_back(entry);
        }
        entries += batch.size();
        packets.push_back(fast_test::incremental(seq, batch));
        bytes += packets.back().size();
    }

    // Decode only.
    std::uint64_t checksum = 0;
    constexpr int kRounds = 10;
    counting = true;
    auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < kRounds; ++round) {
        for (const fast_test::Bytes& packet : packets) {
            Reader r{packet.data() + kPreambleSize, packet.data() + packet.size()};
            DefaultIncrementalRefreshMessage message;
            if (read_template_id(r) == DefaultIncrementalRefreshMessage::kTemplateId)
                decode(r, message, [&](const auto&, const auto& e) {
                    checksum += e.RptSeq + static_cast<std::uint64_t>(e.MDEntryPx.value.mantissa);
                });
        }
    }
    const double decode_seconds = seconds_since(start);
    counting = false;
    const std::uint64_t decode_allocations = allocations;

    // Arbitration, per-instrument sequencing and book maintenance, both feeds.
    BookSink sink;
    sink.books.reserve(kInstruments);
    MarketStream<BookSink> stream(sink);
    const fast_test::Bytes reset = fast_test::sequence_reset(1, 1);
    stream.on_incremental(reset.data(), reset.size(), 0);
    allocations = 0;
    counting = true;
    start = std::chrono::steady_clock::now();
    for (const fast_test::Bytes& packet : packets) {
        stream.on_incremental(packet.data(), packet.size(), 0); // feed A
        stream.on_incremental(packet.data(), packet.size(), 0); // feed B duplicate
    }
    const double stream_seconds = seconds_since(start);
    counting = false;

    const double decoded_packets = double(kPackets) * kRounds;
    std::printf("packets %u, entries %llu, mean packet %.1f bytes\n", kPackets,
                static_cast<unsigned long long>(entries), double(bytes) / kPackets);
    std::printf("decode only     : %.1f ns/packet, %.1f ns/entry, %.0f MB/s, allocations %llu\n",
                decode_seconds * 1e9 / decoded_packets, decode_seconds * 1e9 / (double(entries) * kRounds),
                double(bytes) * kRounds / decode_seconds / 1e6, static_cast<unsigned long long>(decode_allocations));
    std::printf("stream + book   : %.1f ns/packet incl. the duplicate feed, allocations %llu\n",
                stream_seconds * 1e9 / kPackets, static_cast<unsigned long long>(allocations));
    std::printf("applied %llu of %llu entries, gaps %llu, checksum %llu\n",
                static_cast<unsigned long long>(sink.applied), static_cast<unsigned long long>(entries),
                static_cast<unsigned long long>(stream.stats().gaps), static_cast<unsigned long long>(checksum));
    return sink.applied == entries && stream.stats().gaps == 0 ? 0 : 1;
}
