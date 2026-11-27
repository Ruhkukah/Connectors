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
#include <map>
#include <bit>
#include <cerrno>
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
struct ClientDepth {
    using Key = std::pair<unsigned, unsigned>;
    using Level = std::pair<float, float>;
    std::map<Key, Level> levels, staged;
    std::uint64_t epoch{}, batch{}, version{}, update_batches{}, snapshots{}, rejected{}, gaps{};
    std::uint64_t active_batch{}, active_epoch{}, active_version{}, active_type{}, batch_rows{};
    std::vector<std::uint64_t> delivered_versions;
    std::uint64_t content_hash{1469598103934665603ULL};
    static std::uint64_t integer(std::span<const std::uint8_t> bytes, std::size_t& pos) {
        std::uint64_t out{};
        for (unsigned shift = 0; shift < 64 && pos < bytes.size(); shift += 7) {
            const auto b = bytes[pos++];
            out |= static_cast<std::uint64_t>(b & 127) << shift;
            if (!(b & 128))
                return out;
        }
        throw std::runtime_error("invalid DTC protobuf integer");
    }
    void receive(unsigned type, std::span<const std::uint8_t> bytes) {
        std::array<std::uint64_t, 32> numbers{};
        std::array<float, 32> reals{};
        for (std::size_t pos = 0; pos < bytes.size();) {
            const auto key = integer(bytes, pos), field = key >> 3, wire = key & 7;
            if (wire == 0) {
                const auto value = integer(bytes, pos);
                if (field < numbers.size())
                    numbers[field] = value;
            } else if (wire == 5) {
                test::require(pos + 4 <= bytes.size(), "DTC fixed32 truncated");
                if (field < reals.size())
                    std::memcpy(&reals[field], bytes.data() + pos, 4);
                pos += 4;
            } else if (wire == 1) {
                test::require(pos + 8 <= bytes.size(), "DTC fixed64 truncated");
                pos += 8;
            } else if (wire == 2) {
                const auto size = integer(bytes, pos);
                test::require(size <= bytes.size() - pos, "DTC string truncated");
                pos += size;
            } else
                throw std::runtime_error("unsupported DTC protobuf wire type");
        }
        rejected += type == 5 || type == 103 || type == 121 || (type == 116 && numbers[2] == 1);
        if (type != 140 && type != 145)
            return;
        const bool snapshot = type == 145;
        const auto next_epoch = numbers[snapshot ? 11 : 13], next_batch = numbers[snapshot ? 12 : 14];
        const auto next_version = numbers[snapshot ? 13 : 15], marker = numbers[snapshot ? 7 : 8];
        const auto side = numbers[snapshot ? 6 : 5], level = numbers[snapshot ? 5 : 9];
        test::require((side == 1 || side == 2) && level > 0, "DTC level identity invalid");
        if (!batch_rows) {
            test::require(marker == 1 || marker == 3, "DTC batch missing first marker");
            staged = snapshot ? std::map<Key, Level>{} : levels;
            active_batch = next_batch;
            active_epoch = next_epoch;
            active_version = next_version;
            active_type = type;
        } else
            test::require(marker == 1 || marker == 2, "DTC batch marker invalid");
        test::require(active_batch == next_batch && active_epoch == next_epoch && active_version == next_version &&
                          active_type == type,
                      "DTC batch fields changed before commit");
        ++batch_rows;
        const Key key{static_cast<unsigned>(side), static_cast<unsigned>(level)};
        if (!snapshot && numbers[6] == 2)
            staged.erase(key);
        else {
            test::require(snapshot || numbers[6] == 1, "DTC update action invalid");
            staged[key] = {reals[snapshot ? 2 : 3], reals[snapshot ? 3 : 4]};
        }
        // Hash value/position/operation, excluding wall-clock source timestamps.
        for (const auto value : {static_cast<std::uint64_t>(type), side, level, numbers[6],
                                 static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(reals[snapshot ? 2 : 3])),
                                 static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(reals[snapshot ? 3 : 4]))}) {
            content_hash ^= value;
            content_hash *= 1099511628211ULL;
        }
        if (marker == 1) {
            gaps += batch && (next_epoch != epoch || next_batch != batch + 1);
            levels.swap(staged);
            epoch = next_epoch;
            batch = next_batch;
            version = next_version;
            if (snapshot)
                ++snapshots;
            else {
                ++update_batches;
                delivered_versions.push_back(version);
            }
            batch_rows = 0;
        }
    }
    bool equals(const dtc::DtcMarketDataSnapshot& snapshot) const {
        std::map<Key, Level> expected;
        std::array<unsigned, 3> positions{};
        for (const auto& row : snapshot.levels) {
            const auto side = static_cast<unsigned>(row.side);
            expected[{side, ++positions[side]}] = {static_cast<float>(static_cast<double>(row.price_scaled) / 100000.0),
                                                   static_cast<float>(row.volume)};
        }
        return levels == expected;
    }
};
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
        test::require(argc == 2 || argc == 3, "benchmark requires fake CGate library [depth]");
        const std::size_t depth = argc == 3 ? std::stoul(argv[2]) : 20;
        test::require(depth > 0 && depth <= 20000, "benchmark depth bound");
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
                                           .settings =
                                               cg::full_order_log_listener_settings(transport.runtime.scheme_dir)};
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
        std::uint64_t measured_queue_peak{}, measured_commits{}, expected_update_rows{}, expected_version{};
        std::vector<std::uint64_t> expected_versions;
        expected_versions.reserve(40000);
        loop.on_queued_commit = [&] {
            if (timed.measuring) {
                ++measured_commits;
                measured_queue_peak = std::max<std::uint64_t>(measured_queue_peak, server.queued_bytes());
                const auto version = source.status_snapshot().source_snapshot_version;
                if (version != expected_version && !source.depth_changes().empty()) {
                    expected_update_rows += source.depth_changes().size();
                    expected_versions.push_back(version);
                }
                expected_version = version;
            }
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
        source.configure_depth_limit(depth);
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
        std::uint64_t wire_bytes{}, initial_rows{}, update_rows{}, owner_pump_calls{}, client_apply_fences{};
        ClientDepth client;
        std::array<std::uint64_t, 65536> messages{};
        Bytes pending;
        pending.reserve(131072);
        auto read_client = [&] {
            std::array<std::uint8_t, 65536> bytes{};
            for (;;) {
                const auto received = ::recv(fd, bytes.data(), bytes.size(), 0);
                test::require(received != 0, "DTC peer closed before complete delivery");
                if (received < 0) {
                    test::require(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR,
                                  "DTC client receive failed");
                    break;
                }
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
                    if (type != 7)
                        client.receive(type, std::span(pending).subspan(offset + 4, size - 4));
                    offset += size;
                }
                pending.erase(pending.begin(), pending.begin() + offset);
            }
        };
        auto socket_pump = [&] {
            server.poll();
            read_client();
            test::require(server.has_client() && !client.rejected && server.last_error().empty(),
                          "DTC client was rejected/disconnected: " + server.last_error());
        };
        auto pump = [&] {
            loop.poll();
            ++owner_pump_calls;
            read_client();
        };
        auto drain = [&](bool initial = false) {
            const auto until = Clock::now() + std::chrono::seconds(3);
            while (server.queued_bytes() || !pending.empty() || client.batch_rows ||
                   (initial
                        ? client.snapshots != 1
                        : update_rows != expected_update_rows || client.update_batches != expected_versions.size())) {
                socket_pump();
                test::require(Clock::now() < until, "DTC client did not fully apply queued commits");
                std::this_thread::yield();
            }
            test::require(!client.rejected && !client.gaps && server.has_client() && server.last_error().empty(),
                          "DTC rejected, sequence gap or disconnect");
            if (!initial) {
                ++client_apply_fences;
                test::require(client.delivered_versions == expected_versions,
                              "DTC client acknowledged a different commit/version sequence");
            }
            // DTC has no per-depth-batch wire ACK. This heartbeat keeps the peer live;
            // the client application fence above is the delivery acknowledgement.
            send_frame(fd, 3, {});
            socket_pump();
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
        num(p, 5, depth);
        send_frame(fd, 102, p);
        await_message(145);
        drain(true);
        test::require(client.equals(source.snapshot()), "initial client book equals committed book");
        test::require(server.has_client() && initial_rows && !client.rejected,
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
        expected_version = source.status_snapshot().source_snapshot_version;
        timed.measuring = true;
        const auto start = Clock::now();
        while (replay_pending()) {
            pump();
            drain();
        }
        drain();
        const auto elapsed = ns(start);
        timed.measuring = false;
        set_allocation_window(nullptr);
        test::require(book.metrics().rows_total - metrics_before.rows_total == count &&
                          book.metrics().rows_filtered - metrics_before.rows_filtered == count * 9 / 10,
                      "exact input and filter accounting");
        test::require(timed.rows.size() == count && server.has_client(), "complete sustained replay and DTC client");
        test::require(update_rows == expected_update_rows && !client.rejected && !client.gaps &&
                          client.update_batches == expected_versions.size() && !server.queued_bytes() &&
                          pending.empty() && !client.batch_rows && client.equals(source.snapshot()),
                      "all depth batches delivered, applied and final book equals committed source");
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
                  << ",\"configured_dtc_queue_cap_bytes\":4194304,\"measured_dtc_queue_peak_bytes\":"
                  << measured_queue_peak << ",\"subscribed_depth\":" << depth
                  << ",\"input_commits\":" << measured_commits << ",\"expected_update_rows\":" << expected_update_rows
                  << ",\"update_batches\":" << client.update_batches
                  << ",\"client_applied_batches\":" << client.delivered_versions.size()
                  << ",\"expected_update_batches\":" << expected_versions.size()
                  << ",\"last_dtc_batch_sequence\":" << client.batch << ",\"stream_epoch\":" << client.epoch
                  << ",\"last_delivered_source_version\":" << client.version
                  << ",\"final_source_version\":" << source.status_snapshot().source_snapshot_version
                  << ",\"elapsed_ns_including_delivery\":" << elapsed
                  << ",\"client_apply_fences\":" << client_apply_fences << ",\"client_rejects\":" << client.rejected
                  << ",\"sequence_gaps\":" << client.gaps << ",\"resnapshots\":" << client.snapshots - 1
                  << ",\"final_book_equal\":true,\"wire_content_hash_without_time\":" << client.content_hash
                  << ",\"metadata_refreshes\":" << loop.metadata_refreshes - refreshes_before
                  << ",\"owner_polls\":" << loop.polls - polls_before
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
