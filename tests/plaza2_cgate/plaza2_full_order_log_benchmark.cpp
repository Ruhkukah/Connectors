#include "plaza2_runtime_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/connector_host/full_order_log_dtc.hpp"
#include "moex/connector_host/dtc_read_only_server.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
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
    bool wants_negotiated_raw_replication() const noexcept override {
        return true;
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
        allocation_window = true;
        const auto error = book.on_plaza2_listener_event(e);
        allocation_window = false;
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
                                              test::build_vendor_like_runtime_scheme("9.9", "990.1", "TEST"));
        cg::Plaza2Settings settings{.runtime_root = fixture.root,
                                    .library_path = fixture.library_path,
                                    .scheme_dir = fixture.scheme_dir,
                                    .config_dir = fixture.config_dir,
                                    .env_open_settings = "ini=fake.ini"};
        std::array<std::int32_t, 1> ids{4001};
        cg::Plaza2FullOrderLog book(ids, 100010, 2048);
        TimedBook timed(book);
        dtc::DtcMarketDataSnapshot metadata;
        metadata.symbol = "PERF-FAKE";
        metadata.underlying_board = "RFUD";
        metadata.min_step = "1";
        metadata.transport_active = metadata.refdata_metadata_current = true;
        dtc::DtcFullOrderLogSource source(book, 4001, metadata);
        dtc::DtcReadOnlyServerConfig cfg;
        cfg.max_depth_levels = 20000;
        cfg.max_queued_bytes = 4 * 1024 * 1024;
        cfg.currency = "RUB";
        cfg.description = "Offline deterministic benchmark";
        cfg.contract_size = 1;
        cfg.currency_value_per_increment = 1;
        dtc::DtcReadOnlyServer server(source, cfg);
        std::string error;
        test::require(server.start(error), error);
        std::vector<std::uint64_t> commits;
        commits.reserve(40000);
        book.on_commit = [&](const auto&) {
            source.committed();
            server.publish_depth_commit();
            if (timed.measuring && server.last_commit_to_queue_ns())
                commits.push_back(ns(timed.commit_start));
        };
        cg::Plaza2Env env;
        test::require(!env.open(settings), "env open");
        cg::Plaza2Connection conn;
        test::require(!conn.create(env, "p2tcp://fake:4001;app_name=offline_ordlog_perf"), "conn create");
        test::require(!conn.open(""), "conn open");
        cg::Plaza2Listener listener;
        test::require(!listener.create(conn, cg::kFullOrderLogStreamCode,
                                       "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL", &timed),
                      "listener create");
        test::require(!listener.open(""), "listener open");
        for (unsigned n = 0; n < 10 && !book.valid(); ++n)
            test::require(!conn.process(0), "bootstrap");
        test::require(book.valid(),
                      "book must become valid after ONLINE commit: " + listener.last_callback_error().message);
        auto* handle = ::dlopen(fixture.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        test::require(handle, "fake controls");
        auto replay = reinterpret_cast<void (*)(std::uint64_t, std::uint64_t, std::uint32_t)>(
            ::dlsym(handle, "moex_fake_ordlog_replay"));
        test::require(replay, "wire replay hook");
        const auto initial_count = book.order_count();
        replay(100000, 100000, 256);
        for (unsigned n = 0; n < (100000 + 255) / 256; ++n)
            test::require(!conn.process(0), "warmup");
        test::require(book.order_count() == 100000 + initial_count,
                      "retained universe: " + std::to_string(book.order_count()) +
                          " initial=" + std::to_string(initial_count));
        source.configure_depth_limit(20000);
        const auto snapshot_start = Clock::now();
        const auto snapshot = source.snapshot();
        const auto snapshot_ns = ns(snapshot_start);
        test::require(!snapshot.levels.empty(), "snapshot universe");
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
        auto pump = [&] {
            server.poll();
            std::array<std::uint8_t, 65536> bytes{};
            while (::recv(fd, bytes.data(), bytes.size(), 0) > 0) {
            }
        };
        for (unsigned n = 0; n < 5; ++n)
            pump();
        const std::array<std::uint8_t, 16> handshake{16, 0, 6, 0, 8, 0, 0, 0, 4, 0, 0, 0, 'D', 'T', 'C', 0};
        test::require(::send(fd, handshake.data(), handshake.size(), 0) == 16, "handshake");
        for (unsigned n = 0; n < 5; ++n)
            pump();
        Bytes p;
        num(p, 1, 8);
        num(p, 7, 10);
        txt(p, 11, "offline-perf");
        send_frame(fd, 1, p);
        for (unsigned n = 0; n < 5; ++n)
            pump();
        p.clear();
        num(p, 1, 1);
        txt(p, 2, metadata.symbol);
        txt(p, 3, dtc::kDtcMoexSpectraExchange);
        send_frame(fd, 506, p);
        for (unsigned n = 0; n < 5; ++n)
            pump();
        p.clear();
        num(p, 1, 1);
        num(p, 2, 7);
        txt(p, 3, metadata.symbol);
        txt(p, 4, dtc::kDtcMoexSpectraExchange);
        send_frame(fd, 102, p);
        for (unsigned n = 0; n < 50; ++n)
            pump();
        test::require(server.has_client(), "DTC subscriber live");
        constexpr std::uint64_t count = 1000000;
        constexpr std::uint32_t batch = 32;
        replay(count, 100000, batch);
        timed.measuring = true;
        const auto start = Clock::now();
        for (std::uint64_t n = 0; n < (count + batch - 1) / batch; ++n) {
            test::require(!conn.process(0), "measured replay");
            pump();
        }
        const auto elapsed = ns(start);
        timed.measuring = false;
        test::require(timed.rows.size() == count && server.has_client(), "complete sustained replay and DTC client");
        test::require(!commits.empty(), "DTC frames actually queued on commit");
        rusage usage{};
        ::getrusage(RUSAGE_SELF, &usage);
        std::vector<std::uint64_t> callbacks;
        auto samples = reinterpret_cast<void (*)(std::vector<std::uint64_t>*)>(
            ::dlsym(handle, "moex_fake_ordlog_callback_samples"));
        test::require(samples, "native callback samples hook");
        samples(&callbacks);
        const double rate = static_cast<double>(count) * 1e9 / elapsed;
        std::cout << "{\"kind\":\"offline_fake_cgate_callback_decode_book_commit_dtc\",\"configured_isins\":1,"
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
                  << ",\"max_dtc_queue_bytes\":" << cfg.max_queued_bytes << ",\"max_rss_native\":" << usage.ru_maxrss
                  << ",\"row_heap_allocations\":" << allocations << "}\n";
        const bool accepted = rate >= 300000 && callbacks[callbacks.size() * 99 / 100] < 5000 &&
                              commits[commits.size() * 99 / 100] < 50000 && snapshot_ns < 2000000000ULL &&
                              allocations == 0;
        test::require(!listener.close() && !listener.destroy() && !conn.close() && !conn.destroy() && !env.close(),
                      "shutdown");
        ::dlclose(handle);
        return accepted ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
