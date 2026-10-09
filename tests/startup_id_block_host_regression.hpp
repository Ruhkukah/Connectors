#pragma once
#include "moex/connector_host/trading_host.hpp"
#include "fake_cgate_control.hpp"
#include "plaza2_runtime_test_support.hpp"
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace moex::connector_host::regression {
inline void startup_id_block_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                             const std::filesystem::path& root) {
    const auto check = plaza2::test::require;
    struct Checkpoint {
        std::uint64_t ext{}, user{};
    };
    const auto checkpoint = [&](const auto& path) {
        std::ifstream input(path);
        std::string magic;
        std::uint64_t device{}, inode{}, offset{}, boundary{}, checksum{};
        Checkpoint value;
        check(bool(input >> magic >> device >> inode >> offset >> value.ext >> value.user >> boundary >> checksum) &&
                  magic == "MOEXJ2",
              "read startup ID checkpoint");
        return value;
    };
    const auto text = [](const auto& path) {
        std::ifstream input(path);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    for (const auto beginning : {10'000'000, 1001}) {
        control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
        config.orders.ext_id_begin = beginning;
        config.orders.ext_id_end = beginning + 2000;
        config.orders.next_ext_id = beginning;
        // 1000 is still the last ID in the initial exclusive [1,1001) block.
        config.orders.next_user_id = beginning == 1001 ? 1001 : 1000;
        config.journal_path = root / ("startup-ID-" + std::to_string(beginning) + ".ndjson");
        config.identity_state_path = root / ("startup-ID-" + std::to_string(beginning) + ".state");
        CgateTradingHost host(config);
        const auto reserved = checkpoint(config.identity_state_path);
        check(reserved.ext > static_cast<std::uint64_t>(beginning),
              "startup checkpoint does not cover the configured first ext_id");
        check(reserved.user > config.orders.next_user_id && (beginning == 1001 || reserved.user == 1001),
              "startup did not cover its first user_id or extended the last-reserved-ID boundary");
        check(!host.start(), "startup ID warm host start");
        for (int i = 0; i < 30; ++i)
            check(!host.poll(), "startup ID warm bootstrap");
        const auto saved = text(config.identity_state_path);
        struct stat before {
        }, after{};
        check(::stat(config.identity_state_path.c_str(), &before) == 0, "stat warm ID checkpoint");
        const auto posted = control.commands().size();
        check(host.place({.client_order_id = "startup-ID-first", .isin_id = 1001, .price = "103000", .quantity = 1})
                  .empty(),
              "configured first Add refused");
        check(control.commands().size() == posted + 1, "configured first Add did not reach native publisher");
        check(::stat(config.identity_state_path.c_str(), &after) == 0 && before.st_ino == after.st_ino &&
                  text(config.identity_state_path) == saved,
              "first Add rewrote its durable ID-block checkpoint");
        check(!host.stop(), "startup ID warm host stop");
    }
    control.configure({});
    config.journal_path = root / "startup-ID-exhausted.ndjson";
    config.identity_state_path = root / "startup-ID-exhausted.state";
    config.orders.next_user_id = UINT32_MAX;
    const auto processes = control.process_count();
    bool refused{};
    try {
        CgateTradingHost exhausted(config);
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused && control.process_count() == processes, "exhausted startup ID arithmetic reached native processing");
}
} // namespace moex::connector_host::regression
