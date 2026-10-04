#include "plaza2_runtime_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "../../apps/full_order_log_dtc_loop.hpp"
#include "moex/connector_host/operator_config.hpp"
#include "fake_cgate_control.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <thread>
#include <new>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
bool allocation_window{};
std::uint64_t allocations{};
} // namespace
void* operator new(std::size_t n) {
    if (allocation_window)
        ++allocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    return ::operator new(n);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
namespace {
namespace cg = moex::plaza2::cgate;
namespace dtc = moex::connector_host::dtc;
namespace test = moex::plaza2::test;
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;
std::uint64_t ns(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}
void num(Bytes& p, unsigned field, std::uint64_t value) {
    p.push_back(static_cast<std::uint8_t>(field * 8));
    while (value > 127) {
        p.push_back(static_cast<std::uint8_t>(value) | 128);
        value >>= 7;
    }
    p.push_back(value);
}
void txt(Bytes& p, unsigned field, std::string_view value) {
    p.push_back(static_cast<std::uint8_t>(field * 8 + 2));
    p.push_back(value.size());
    p.insert(p.end(), value.begin(), value.end());
}
void send_frame(int fd, unsigned type, Bytes p) {
    const auto n = p.size() + 4;
    Bytes out{static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(type),
              static_cast<std::uint8_t>(type >> 8)};
    out.insert(out.end(), p.begin(), p.end());
    test::require(::send(fd, out.data(), out.size(), 0) == static_cast<ssize_t>(out.size()), "send frame");
}
struct TimedBook : cg::Plaza2ListenerEventHandler {
    cg::Plaza2FullOrderLog& book;
    std::vector<std::uint64_t> rows;
    bool measuring{};
    Clock::time_point commit_start{};
    explicit TimedBook(cg::Plaza2FullOrderLog& b) : book(b) {
        rows.reserve(1000000);
    }
    bool wants_raw_replication(std::string_view table) const noexcept override {
        return book.wants_raw_replication(table);
    }
    bool should_log_listener_event(const cg::Plaza2ListenerEvent& e) const noexcept override {
        return book.should_log_listener_event(e);
    }
    void on_plaza2_listener_error(const cg::Plaza2Error& e) noexcept override {
        book.on_plaza2_listener_error(e);
    }
    cg::Plaza2Error on_plaza2_listener_event(const cg::Plaza2ListenerEvent& e) override {
        if (e.kind == cg::Plaza2ListenerEventKind::TransactionCommit)
            commit_start = Clock::now();
        if (!measuring || e.kind != cg::Plaza2ListenerEventKind::StreamData)
            return book.on_plaza2_listener_event(e);
        const auto start = Clock::now();
        const auto error = book.on_plaza2_listener_event(e);
        rows.push_back(ns(start));
        return error;
    }
};
void report(std::string_view key, std::vector<std::uint64_t>& data) {
    test::require(!data.empty(), "timing sample empty");
    std::sort(data.begin(), data.end());
    std::cout << '"' << key << "\":{\"samples\":" << data.size() << ",\"p50_ns\":" << data[data.size() / 2]
              << ",\"p99_ns\":" << data[data.size() * 99 / 100] << ",\"max_ns\":" << data.back() << '}';
}
} // namespace
int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "benchmark requires fake CGate library");
        const auto root = test::make_temp_directory("full-order-log-perf");
        struct Cleanup {
            std::filesystem::path root;
            ~Cleanup() {
                test::remove_tree(root);
            }
        } cleanup{root};
        const auto fixture =
            test::materialize_runtime_fixture(root, std::filesystem::absolute(argv[1]), cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        ::setenv("MOEX_PERF_KEY", "00000000", 1);
        test::fake::Control fake(fixture.library_path);
        test::fake::Scenario scenario;
        scenario.options[static_cast<std::size_t>(test::fake::Option::FullOrderLogRefdata)] = "1";
        fake.configure(scenario);
        fake.configure_refdata_count(5000);
        moex::connector_host::Plaza2HostConfigInputs inputs;
        inputs.read_only_market_data = true;
        inputs.runtime_root = fixture.root;
        inputs.library_path = fixture.library_path;
        inputs.scheme_dir = fixture.scheme_dir;
        inputs.config_dir = fixture.config_dir;
        inputs.env_open_settings = "ini=config/t1.ini;key=00000000";
        inputs.software_key_env_var = "MOEX_PERF_KEY";
        inputs.expected_spectra_release = "SPECTRA9.9.0";
        inputs.publisher_name = "offline_ordlog_perf";
        inputs.isin_ids = {1001};
        auto config = moex::connector_host::build_plaza2_host_config(inputs);
        config.market_data_now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds{1700000100}}; };
        std::array<std::int32_t, 1> ids{1001};
        cg::Plaza2FullOrderLog book(ids, 100010, 2048);
        TimedBook timed(book);
        auto& transport = config.transport.host;
        transport.mode = moex::plaza2_trade::CgateSessionMode::OfflineFake;
        transport.process_timeout_ms = 1;
        transport.aggr20_stream = {};
        transport.full_order_log_stream = {.stream_code = cg::kFullOrderLogStreamCode,
                                           .settings = "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL"};
        transport.full_order_log_handler = &timed;
        moex::connector_host::ConnectorHost host(std::move(config));
        moex::connector_host::FullOrderLogDtcLoop loop(host, book,
                                                       {.source_mode = dtc::DtcSourceMode::Replay,
                                                        .max_queued_bytes = 4 * 1024 * 1024,
                                                        .max_depth_levels = 20000,
                                                        .currency = "RUB",
                                                        .description = "Explicit offline fake definition",
                                                        .contract_size = 1,
                                                        .currency_value_per_increment = 12.5});
        auto& source = loop.source();
        auto& server = loop.server();
        std::vector<std::uint64_t> commits;
        commits.reserve(40000);
        loop.on_queued_commit = [&] {
            if (timed.measuring && server.last_commit_to_queue_ns())
                commits.push_back(ns(timed.commit_start));
        };
        loop.start();
        for (unsigned n = 0; n < 20 && !book.valid(); ++n)
            loop.poll();
        test::require(book.valid(), "book must become valid after actual host bootstrap");
        fake.enqueue({.kind = test::fake::EventKind::Begin, .stream_code = cg::kFullOrderLogStreamCode});
        fake.enqueue({.kind = test::fake::EventKind::ClearDeleted,
                      .stream_code = cg::kFullOrderLogStreamCode,
                      .table_code = moex::plaza2::generated::TableCode::kFortsUserorderbookReplOrders,
                      .revision = 23});
        fake.enqueue({.kind = test::fake::EventKind::Commit, .stream_code = cg::kFullOrderLogStreamCode});
        for (unsigned n = 0; n < 5; ++n)
            loop.poll();
        test::require(book.valid() && !book.order_count(),
                      "bootstrap fixture cleared before realistic tick-grid warmup");
        auto* handle = ::dlopen(fixture.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        test::require(handle, "fake controls");
        auto replay = reinterpret_cast<void (*)(std::uint64_t, std::uint64_t, std::uint32_t)>(
            ::dlsym(handle, "moex_fake_ordlog_replay"));
        test::require(replay, "wire replay hook");
        auto mixed_replay = reinterpret_cast<void (*)(std::uint64_t, std::uint64_t, std::uint32_t)>(
            ::dlsym(handle, "moex_fake_ordlog_mixed_replay"));
        auto remaining = reinterpret_cast<void (*)(std::uint64_t*)>(::dlsym(handle, "moex_fake_ordlog_remaining"));
        test::require(mixed_replay && remaining, "mixed replay controls");
        auto replay_pending = [&] {
            std::uint64_t rows{};
            remaining(&rows);
            return rows != 0;
        };
        const auto initial_count = book.order_count();
        replay(100000, 100000, 256);
        while (replay_pending())
            loop.poll();
        test::require(book.order_count() == 100000 + initial_count,
                      "retained universe: " + std::to_string(book.order_count()) +
                          " initial=" + std::to_string(initial_count));
        source.configure_depth_limit(20000);
        const auto snapshot_start = Clock::now();
        const auto snapshot = source.snapshot();
        const auto snapshot_ns = ns(snapshot_start);
        test::require(!snapshot.levels.empty(), "snapshot universe");
        const auto metadata = source.status_snapshot();
        test::require(!metadata.symbol.empty(), "committed real REFDATA metadata");
        test::require(metadata.valid && !book.crossed(1001), "realistic fixture is valid and uncrossed");
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        test::require(fd >= 0, "client socket");
        struct Fd {
            int fd;
            ~Fd() {
                ::close(fd);
            }
        } close_fd{fd};
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(server.port());
        test::require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        const int no_delay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
        std::uint64_t wire_bytes{}, initial_rows{}, update_rows{}, rejected{};
        std::array<std::uint64_t, 65536> messages{};
        Bytes pending;
        pending.reserve(131072);
        auto pump = [&] {
            loop.poll();
            std::array<std::uint8_t, 65536> bytes{};
            for (ssize_t received; (received = ::recv(fd, bytes.data(), bytes.size(), 0)) > 0;) {
                wire_bytes += received;
                pending.insert(pending.end(), bytes.begin(), bytes.begin() + received);
                std::size_t offset = 0;
                while (pending.size() - offset >= 4) {
                    const auto size = pending[offset] | (static_cast<unsigned>(pending[offset + 1]) << 8);
                    test::require(size >= 4, "well-framed DTC output");
                    if (pending.size() - offset < size)
                        break;
                    const auto type = pending[offset + 2] | (static_cast<unsigned>(pending[offset + 3]) << 8);
                    ++messages[type];
                    initial_rows += type == 145;
                    update_rows += type == 140;
                    rejected += type == 121;
                    offset += size;
                }
                pending.erase(pending.begin(), pending.begin() + offset);
            }
        };
        auto await_message = [&](unsigned type) {
            const auto deadline = Clock::now() + std::chrono::seconds(3);
            while (!messages[type] && server.has_client() && Clock::now() < deadline) {
                pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            test::require(messages[type], "DTC message missing: " + std::to_string(type) +
                                              " source_valid=" + std::to_string(source.status_snapshot().valid));
        };
        for (unsigned n = 0; n < 5; ++n)
            pump();
        const std::array<std::uint8_t, 16> handshake{16, 0, 6, 0, 8, 0, 0, 0, 4, 0, 0, 0, 'D', 'T', 'C', 0};
        test::require(::send(fd, handshake.data(), handshake.size(), 0) == 16, "handshake");
        await_message(7);
        Bytes p;
        num(p, 1, 8);
        num(p, 7, 10);
        txt(p, 11, "offline-perf");
        send_frame(fd, 1, p);
        await_message(2);
        p.clear();
        num(p, 1, 1);
        txt(p, 2, metadata.symbol);
        txt(p, 3, dtc::kDtcMoexSpectraExchange);
        send_frame(fd, 506, p);
        await_message(507);
        p.clear();
        num(p, 1, 1);
        num(p, 2, 1);
        txt(p, 3, metadata.symbol);
        txt(p, 4, dtc::kDtcMoexSpectraExchange);
        send_frame(fd, 102, p);
        await_message(145);
        test::require(server.has_client() && initial_rows && !rejected,
                      "actual DTC initial snapshot received without rejection");
        constexpr std::uint64_t count = 1000000;
        constexpr std::uint32_t batch = 32;
        mixed_replay(count, 100000, batch);
        auto set_allocation_window =
            reinterpret_cast<void (*)(void (*)(bool))>(::dlsym(handle, "moex_fake_ordlog_allocation_window"));
        test::require(set_allocation_window, "native callback allocation window");
        set_allocation_window([](bool active) { allocation_window = active; });
        const auto metrics_before = book.metrics();
        const auto refreshes_before = loop.metadata_refreshes;
        const auto polls_before = loop.polls;
        timed.measuring = true;
        const auto start = Clock::now();
        while (replay_pending())
            pump();
        const auto elapsed = ns(start);
        timed.measuring = false;
        set_allocation_window(nullptr);
        test::require(book.metrics().rows_total - metrics_before.rows_total == count &&
                          book.metrics().rows_filtered - metrics_before.rows_filtered == count * 9 / 10,
                      "exact input and filter accounting");
        test::require(timed.rows.size() == count && server.has_client(), "complete sustained replay and DTC client");
        test::require(update_rows && !rejected, "actual depth updates drained without subscription rejection");
        test::require(!commits.empty(), "DTC frames actually queued on commit");
        rusage usage{};
        ::getrusage(RUSAGE_SELF, &usage);
        std::vector<std::uint64_t> callbacks;
        auto samples = reinterpret_cast<void (*)(std::vector<std::uint64_t>*)>(
            ::dlsym(handle, "moex_fake_ordlog_callback_samples"));
        test::require(samples, "native callback samples hook");
        samples(&callbacks);
        const double rate = static_cast<double>(count) * 1e9 / elapsed;
        std::cout << "{\"kind\":\"offline_fake_actual_host_loop_mixed_book_commit_dtc\",\"configured_isins\":1,"
                     "\"refdata_rows\":5000,\"filtered_percent\":90,\"admitted_add_cancel_trade_percent\":[40,40,20],"
                     "\"retained_orders\":"
                  << book.order_count() << ",\"rows\":" << count << ",\"rows_per_commit\":" << batch
                  << ",\"msg_per_second\":" << rate << ',';
        report("callback_to_book", callbacks);
        std::cout << ',';
        report("handler_decode_to_book", timed.rows);
        std::cout << ',';
        report("commit_to_dtc_queue", commits);
        std::cout << ",\"snapshot_ns\":" << snapshot_ns << ",\"snapshot_levels\":" << snapshot.levels.size()
                  << ",\"book_memory_bytes\":" << book.memory_bytes()
                  << ",\"max_dtc_queue_bytes\":4194304,\"metadata_refreshes\":"
                  << loop.metadata_refreshes - refreshes_before << ",\"owner_polls\":" << loop.polls - polls_before
                  << ",\"filtered_rows\":" << book.metrics().rows_filtered - metrics_before.rows_filtered
                  << ",\"max_rss_native\":" << usage.ru_maxrss << ",\"wire_bytes_drained\":" << wire_bytes
                  << ",\"initial_rows\":" << initial_rows << ",\"update_rows\":" << update_rows
                  << ",\"row_heap_allocations\":" << allocations << "}\n";
        const bool accepted = rate >= 300000 && callbacks[callbacks.size() * 99 / 100] < 5000 &&
                              commits[commits.size() * 99 / 100] < 50000 && snapshot_ns < 2000000000ULL &&
                              allocations == 0;
        loop.stop();
        ::dlclose(handle);
        return accepted ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
