#include "moex/plaza2/cgate/plaza2_ordlog.hpp"
#include "plaza2_runtime_test_support.hpp"
#include <chrono>
#include <dlfcn.h>
#include <iostream>
#include <sys/resource.h>
#include <thread>

using namespace moex::plaza2;
using namespace moex::plaza2::cgate;
using moex::plaza2::test::require;
using Clock = std::chrono::steady_clock;
using Emit = std::uint32_t (*)(std::uint32_t, std::size_t, void*, std::size_t, std::int64_t, std::uint8_t*,
                               std::size_t);
std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
struct Profile {
    const char* name;
    std::uint64_t rate, count;
    std::size_t batch, instruments, ids;
};

void run(const Profile& profile, Plaza2Connection& connection, std::string_view scheme, Emit emit) {
    Plaza2Ordlog pipe(65536, 65536);
    require(!pipe.create(connection, scheme), "benchmark listener create");
    require(!pipe.open(), "benchmark scheme qualification");
    require(emit(0x1112, 0, nullptr, 0, 0, nullptr, 0) == 0, "benchmark ONLINE");
    std::array<std::array<std::byte, 148>, 4> wire{};
    std::array<std::int64_t, 4> revisions{};
    for (std::size_t index = 0; index < 4; ++index) {
        for (const auto& field : public_wire::kTables[index].fields) {
            if (field.type == "d16.5") {
                const public_wire::Bcd16_5 price{5, 16, 0, 0, 0, 0, 0, 12, 34, 56, 70};
                std::memcpy(wire[index].data() + field.offset, price.data(), price.size());
            }
            if (field.type == "t") {
                const public_wire::Time time{2026, 9, 7, 12, 0, 0, 999};
                std::memcpy(wire[index].data() + field.offset, &time, sizeof(time));
            }
        }
    }
    std::uint64_t order_sequence = 0;
    std::uint64_t consumed = 0, optional_consumed = 0, peak_age = 0, checksum = 0;
    const auto start = Clock::now();
    for (std::uint64_t offset = 0; offset < profile.count;) {
        const auto end = std::min(profile.count, offset + profile.batch);
        pipe.poll_time(now_ns());
        require(emit(0x200, 0, nullptr, 0, 0, nullptr, 0) == 0, "benchmark transaction begin");
        for (auto i = offset; i < end; ++i) {
            // Regular and multileg dominate; heartbeat/sys_events still exercise all dispatch plans.
            const std::size_t index = i % 1024 == 0 ? 3 : i % 512 == 0 ? 2 : i % 10 == 0 ? 1 : 0;
            const auto& table = public_wire::kTables[index];
            auto* bytes = wire[index].data();
            const auto revision = ++revisions[index];
            const std::int64_t id =
                9007199254740993LL + static_cast<std::int64_t>((index < 2 ? order_sequence++ : i) % profile.ids);
            std::memcpy(bytes, &id, 8);
            std::memcpy(bytes + 8, &revision, 8);
            if (index < 2) {
                const auto instrument = static_cast<std::int32_t>(1 + i % profile.instruments);
                // Offsets are compile-time checked against the authoritative SDK in the generated bindings.
                std::memcpy(bytes + offsetof(public_wire::OrdlogOrdersLog, public_order_id), &id, 8);
                std::memcpy(bytes + offsetof(public_wire::OrdlogOrdersLog, isin_id), &instrument, 4);
                const std::uint64_t moment_ns = 1788782400000000000ULL + i * 1000;
                std::memcpy(bytes + offsetof(public_wire::OrdlogOrdersLog, moment_ns), &moment_ns, 8);
            }
            require(emit(0x120, index, bytes, table.size, revision, nullptr, 0) == 0, "benchmark wire callback");
        }
        require(emit(0x210, 0, nullptr, 0, 0, nullptr, 0) == 0, "benchmark transaction commit");
        pipe.poll_time(now_ns());
        peak_age = std::max(peak_age, pipe.metrics().oldest_age_ns);
        while (const auto* row = pipe.front()) {
            checksum ^= static_cast<std::uint64_t>(row->repl_id) + row->sequence;
            require(pipe.acknowledge(), "benchmark mandatory acknowledgment");
            ++consumed;
        }
        while (pipe.optional_acknowledge())
            ++optional_consumed;
        offset = end;
        if (profile.rate) {
            std::this_thread::sleep_until(start + std::chrono::nanoseconds(offset * 1000000000ULL / profile.rate));
        }
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const auto metrics = pipe.metrics();
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "benchmark RSS");
#ifdef __APPLE__
    const auto rss_bytes = usage.ru_maxrss;
#else
    const auto rss_bytes = usage.ru_maxrss * 1024;
#endif
    const bool valid = consumed == profile.count && optional_consumed == profile.count &&
                       metrics.records == profile.count && !metrics.dropped && !metrics.optional_dropped &&
                       !metrics.decode_failures && !metrics.revision_failures && !metrics.queued;
    std::cout << "{\"profile\":\"" << profile.name << "\",\"messages_processed\":" << consumed
              << ",\"elapsed_seconds\":" << elapsed << ",\"msg_per_second\":" << consumed / elapsed
              << ",\"target_per_second\":" << profile.rate << ",\"queue_current\":" << metrics.queued
              << ",\"queue_high_water\":" << metrics.high_water
              << ",\"oldest_queued_event_age_ns\":" << metrics.oldest_age_ns
              << ",\"peak_oldest_queued_event_age_ns\":" << peak_age << ",\"peak_process_rss_bytes\":" << rss_bytes
              << ",\"decode_failures\":" << metrics.decode_failures
              << ",\"revision_failures\":" << metrics.revision_failures << ",\"dropped_records\":" << metrics.dropped
              << ",\"optional_dropped\":" << metrics.optional_dropped << ",\"instruments\":" << profile.instruments
              << ",\"id_population\":" << profile.ids << ",\"checksum\":" << checksum
              << ",\"accounting_pass\":" << (valid ? "true" : "false")
              << ",\"l3_mutation\":false,\"certification_performance_pass\":false}\n";
    require(valid, "benchmark lossless accounting");
    // Scheduler jitter is measured; allow 2% wall-time overhead on paced engineering profiles.
    require(!profile.rate || consumed / elapsed >= profile.rate * 0.98, "benchmark offered rate not sustained");
}

int main(int argc, char** argv) {
    try {
        using namespace moex::plaza2::test;
        require(argc == 2 || argc == 3, "usage: plaza2_ordlog_benchmark <fake-runtime-library> [--smoke]");
        const bool smoke = argc == 3 && std::string_view(argv[2]) == "--smoke";
        const auto root = make_temp_directory("ordlog_benchmark");
        const auto fixture =
            materialize_runtime_fixture(root, argv[1], Plaza2Environment::Test,
                                        build_vendor_like_runtime_scheme("SPECTRA99", "990.1.6.42752", "test"));
        {
            Plaza2Settings settings;
            settings.environment = Plaza2Environment::Test;
            settings.runtime_root = fixture.root;
            settings.env_open_settings = "ini=config/t1.ini;key=00000000";
            Plaza2Env env;
            require(!env.open(settings), "fake benchmark environment");
            Plaza2Connection connection;
            require(!connection.create(env, "p2tcp://127.0.0.1:4001;app_name=ordlog_benchmark"),
                    "fake benchmark connection");
            require(!connection.open({}), "fake benchmark open");
            auto* module = dlopen(fixture.library_path.c_str(), RTLD_NOW);
            require(module != nullptr, "fake benchmark module");
            const auto emit = reinterpret_cast<Emit>(dlsym(module, "moex_fake_ordlog_emit"));
            require(emit != nullptr, "fake benchmark driver");
            const std::array profiles{
                Profile{"sustained_100k", 100000, 1000000, 1000, 64, 100000},
                Profile{"sustained_200k", 200000, 2000000, 1000, 64, 100000},
                Profile{"burst", 0, 2000000, 32768, 64, 100000},
                Profile{"multi_instrument", 200000, 2000000, 1000, 4096, 100000},
                Profile{"large_id_population", 200000, 2000000, 1000, 4096, 1000000},
            };
            for (auto profile : profiles) {
                if (smoke) {
                    profile.count = 10000;
                    profile.rate = 0;
                }
                run(profile, connection, fixture.scheme_path.string(), emit);
            }
            dlclose(module);
        }
        remove_tree(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
