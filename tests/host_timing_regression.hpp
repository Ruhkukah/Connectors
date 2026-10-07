#pragma once
#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include <fstream>
#include <thread>

namespace moex::connector_host::regression {
inline void host_timing_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                   const std::filesystem::path& root) {
    const auto check = plaza2::test::require;
    control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
    config.measure_timings = true;
    config.session.process_timeout_ms = 0;
    auto now = OrderManager::Clock::time_point(std::chrono::seconds(100));
    config.session.recovery_now = [&] { return now; };
    config.orders.max_commands_per_second = 1;
    config.journal_path = root / "host-timings.ndjson";
    config.identity_state_path = root / "host-timings.state";
    bool slow_storage{};
    config.storage_space_probe = [&](const auto&) {
        if (slow_storage)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return std::uintmax_t(1024 * 1024 * 1024);
    };
    CgateTradingHost host(config);
    check(!host.start(), "timed host start");
    for (int i = 0; i < 30; ++i)
        check(!host.poll(), "timed bootstrap");
    now += std::chrono::minutes(2);
    slow_storage = true;
    check(!host.poll(), "timed storage poll");
    slow_storage = false;
    control.set(plaza2::test::fake::Option::PubReplyOrderId, "67001");
    check(host.place({.client_order_id = "timed-immediate", .isin_id = 1001, .price = "103000", .quantity = 1}).empty(),
          "timed immediate Add refused");
    check(host.place({.client_order_id = "timed-held", .isin_id = 1001, .price = "103000", .quantity = 1}).empty(),
          "timed rate-held Add refused");
    check(!host.poll(), "timed immediate reply");
    now += std::chrono::seconds(1);
    control.set(plaza2::test::fake::Option::PubReplyOrderId, "67002");
    check(!host.poll(), "timed rate-released Add");
    check(!host.stop(), "timed host stop");
    std::ifstream journal(config.journal_path);
    std::string line;
    bool slow{}, immediate{}, held{};
    while (std::getline(journal, line)) {
        if (line.find("\"event\":\"slow_owner_poll\"") != std::string::npos) {
            slow |= line.find("\"before_session_ns\":") != std::string::npos &&
                    line.find("\"journal_flush_ns\":") != std::string::npos &&
                    line.find("\"process_ns\":") != std::string::npos && line.find("\"events\":[") != std::string::npos;
        }
        if (line.find("\"event\":\"command_timing\"") != std::string::npos) {
            check(line.find("\"post_started_utc_ns\":0") == std::string::npos,
                  "post timing fabricated for a non-invoked boundary");
            if (line.find("timed-immediate") != std::string::npos)
                immediate = line.find("\"immediate_place_to_post_ns\":") != std::string::npos;
            if (line.find("\"user_id\":2") != std::string::npos) {
                held = true;
                check(line.find("immediate_place_to_post_ns") == std::string::npos,
                      "rate-held Add falsely labelled immediate place latency");
            }
        }
    }
    check(slow && immediate && held, "missing attributed slow poll or actual publisher timing");
    control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
}
} // namespace moex::connector_host::regression
