#include "full_order_log_dtc_worker.hpp"

#include <iostream>
#include <latch>

namespace moex::connector_host {
struct FolDtcWorkerStressAccess {
    static auto market(FullOrderLogDtcWorker& worker) {
        return worker.market_.pop();
    }
    static auto market_reset(FullOrderLogDtcWorker& worker) {
        return worker.market_reset_.take();
    }
    static auto generation(const FullOrderLogDtcWorker& worker) {
        return worker.market_generation_.load(std::memory_order_acquire);
    }
    static auto private_snapshot(FullOrderLogDtcWorker& worker) {
        return worker.private_.take();
    }
};
} // namespace moex::connector_host

namespace {
using namespace moex::connector_host;
using Access = FolDtcWorkerStressAccess;
using Worker = FullOrderLogDtcWorker;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void ring_capacity() {
    FolDtcChannel<std::unique_ptr<int>, 3> ring;
    require(!ring.pop(), "new ring is empty");
    require(ring.push(std::make_unique<int>(1)) && ring.push(std::make_unique<int>(2)), "two usable slots");
    auto next = std::make_unique<int>(3);
    require(!ring.try_move(next) && next && *next == 3, "full ring preserves producer ownership");
    const auto first = ring.pop();
    require(first && *first && **first == 1 && ring.try_move(next) && !next, "consumer frees the wrapped slot");
    const auto second = ring.pop(), third = ring.pop();
    require(second && *second && **second == 2 && third && *third && **third == 3 && !ring.pop(),
            "wraparound must preserve FIFO");
}

void ring_concurrency() {
    struct Row {
        std::uint64_t sequence{};
        std::vector<std::uint64_t> payload;
    };
    FolDtcChannel<Row, 7> ring;
    constexpr std::uint64_t total = 50'000;
    std::latch start(2);
    std::thread producer([&] {
        start.arrive_and_wait();
        for (std::uint64_t n = 1; n <= total; ++n) {
            Row row{n, std::vector<std::uint64_t>(8, n)};
            while (!ring.try_move(row))
                std::this_thread::yield();
        }
    });
    start.arrive_and_wait();
    bool coherent = true;
    for (std::uint64_t n = 1; n <= total; ++n) {
        auto row = ring.pop();
        while (!row) {
            std::this_thread::yield();
            row = ring.pop();
        }
        coherent &= row->sequence == n && row->payload.size() == 8 &&
                    std::ranges::all_of(row->payload, [n](auto value) { return value == n; });
    }
    producer.join();
    require(coherent && !ring.pop(), "concurrent wraparound lost, duplicated, or corrupted a row");
}

Worker::Market market_at(std::uint64_t version) {
    Worker::Market result;
    result.snapshot.source_snapshot_version = version;
    result.snapshot.levels.resize(1);
    result.snapshot.levels.front().volume = static_cast<std::int64_t>(version);
    return result;
}

void generation_on_overflow() {
    Worker worker;
    for (std::uint64_t n = 1; n <= 128; ++n)
        worker.publish(market_at(n));
    require(Access::generation(worker) == 0 && !Access::market_reset(worker), "capacity alone is not an overflow");
    worker.publish(market_at(129));
    worker.publish(market_at(130));
    const auto reset = Access::market_reset(worker);
    require(reset && reset->generation == 2 && Access::generation(worker) == 2 &&
                reset->snapshot.source_snapshot_version == 130 && reset->snapshot.levels.front().volume == 130,
            "overflow must keep the newest full snapshot and advance generation");
    for (std::uint64_t n = 1; n <= 128; ++n) {
        const auto row = Access::market(worker);
        require(row && row->generation < reset->generation && row->snapshot.source_snapshot_version == n,
                "pre-overflow frames must stay identifiable as obsolete");
    }
    require(!Access::market(worker), "old generation fully drained");
    worker.publish(market_at(131));
    const auto current = Access::market(worker);
    require(current && current->generation == reset->generation && current->snapshot.source_snapshot_version == 131,
            "next increment must follow the resynchronized generation");
}

void private_concurrency() {
    Worker worker;
    constexpr std::uint64_t total = 20'000;
    std::latch first_published(1), first_read(1);
    std::thread producer([&] {
        for (std::uint64_t n = 1; n <= total; ++n) {
            const auto qty = static_cast<std::int64_t>(n);
            auto state = std::make_unique<Worker::Private>();
            state->snapshot.generation = n;
            state->snapshot.position = qty;
            state->snapshot.orders = {{.client_id = std::to_string(n), .quantity = qty}};
            state->fills = std::make_shared<const std::vector<dtc::DtcOwnFill>>(
                std::vector<dtc::DtcOwnFill>{{.execution_id = std::to_string(n), .quantity = qty}});
            state->balance = dtc::DtcOwnBalance{qty, -qty, 0};
            worker.publish(std::move(state));
            if (n == 1) {
                first_published.count_down();
                first_read.wait();
            }
        }
    });
    const auto verify = [](const Worker::Private& state) {
        const auto n = state.snapshot.generation;
        const auto qty = static_cast<std::int64_t>(n);
        return state.snapshot.position == qty && state.snapshot.orders.size() == 1 &&
               state.snapshot.orders.front().client_id == std::to_string(n) &&
               state.snapshot.orders.front().quantity == qty && state.fills && state.fills->size() == 1 &&
               state.fills->front().execution_id == std::to_string(n) && state.fills->front().quantity == qty &&
               state.balance == dtc::DtcOwnBalance{qty, -qty, 0};
    };
    first_published.wait();
    auto state = Access::private_snapshot(worker);
    first_read.count_down();
    require(state && state->snapshot.generation == 1, "first private publication missing");
    auto oldest = std::move(state);
    auto last = oldest->snapshot.generation;
    bool coherent = verify(*oldest);
    while (last < total) {
        state = Access::private_snapshot(worker);
        if (!state) {
            std::this_thread::yield();
            continue;
        }
        coherent &= state->snapshot.generation > last && verify(*state) && verify(*oldest);
        last = state->snapshot.generation;
    }
    producer.join();
    require(coherent && last == total && verify(*oldest) && !Access::private_snapshot(worker),
            "private snapshot swap corrupted current or retained state");
}
} // namespace

int main() {
    try {
        ring_capacity();
        ring_concurrency();
        generation_on_overflow();
        private_concurrency();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
