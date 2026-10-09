#pragma once
#include "private_delta_host_regression.hpp"

#include <fstream>
#include <thread>

namespace moex::connector_host::regression {
inline void storage_recovery_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                        const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
    auto now = OrderManager::Clock::time_point(std::chrono::seconds(100));
    config.session.recovery_now = [&] { return now; };
    std::uintmax_t available = 1024 * 1024 * 1024;
    config.storage_space_probe = [&](const auto&) { return available; };
    config.journal_path = root / "capacity-recovery.ndjson";
    config.identity_state_path = root / "capacity-recovery.state";
    CgateTradingHost host(config);
    bootstrap(host);
    available = 1;
    now += std::chrono::seconds(61);
    require(!host.poll() && host.status().find("\"cancel_only\":true") != std::string::npos,
            "low capacity did not protect entry");
    host.record_operator_input("low_space_audit", "command_socket");
    host.record_local_refusal("place blocked", "low capacity", "command_socket");
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    require(!host.poll(), "low-space journal flush");
    std::ifstream log(config.journal_path);
    const std::string contents((std::istreambuf_iterator<char>(log)), {});
    require(contents.find("low_space_audit") != std::string::npos &&
                contents.find("place blocked") != std::string::npos,
            "low-space protection suppressed writable interaction journal");
    require(!host.storage_ok().empty() && host.status().find("\"cancel_only\":true") != std::string::npos,
            "storage ok ignored continuing low capacity");
    available = 1024 * 1024 * 1024;
    require(host.storage_ok().empty() && host.status().find("\"cancel_only\":false") != std::string::npos,
            "storage ok failed to restore healthy writer");
    require(!host.place({.client_order_id = "still_killed",
                         .isin_id = config.isin_ids.front(),
                         .price = "102500",
                         .quantity = 1})
                 .empty(),
            "storage recovery silently disabled kill switch");
    host.set_kill_switch(false);
    // A true writer failure stays protected until checkpoint writes succeed.
    const auto writer_boundary = config.identity_state_path.string() + ".journal";
    std::filesystem::remove(writer_boundary);
    std::filesystem::create_directory(writer_boundary);
    host.record_operator_input("before_writer_failure");
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    require(!host.poll() && host.status().find("\"cancel_only\":true") != std::string::npos,
            "writer failure did not restore protection");
    require(!host.storage_ok().empty(), "storage ok accepted an unrepaired writer");
    std::filesystem::remove(writer_boundary);
    recover_storage(host);
    require(!host.stop(), "recovered storage shutdown failed");
}
} // namespace moex::connector_host::regression
