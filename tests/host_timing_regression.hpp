#pragma once
#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "private_delta_host_regression.hpp"
#include <fstream>
#include <thread>

namespace moex::connector_host::regression {
inline void owner_poll_diagnostics_regression() {
    const auto check = plaza2::test::require;
    OwnerPollDiagnostics diagnostics;
    const auto began = OwnerPollDiagnostics::Clock::time_point(std::chrono::seconds(100));
    // A busy native stream previously wrote one large diagnostic for every
    // slow turn. Retain its actual worst attribution and count, not 10k rows.
    for (std::uint64_t sequence = 1; sequence <= 10000; ++sequence) {
        const auto total = sequence == 25 ? 50000000 : 2000000;
        if (diagnostics.observe(began + std::chrono::microseconds(sequence), sequence, total))
            diagnostics.capture("{\"sequence\":" + std::to_string(sequence) + ",\"total_ns\":" + std::to_string(total) +
                                ",\"events\":[{\"stream\":\"FORTS_TRADE_REPL\",\"kind\":\"commit\"}]}");
    }
    check(!diagnostics.due(began + std::chrono::milliseconds(999)) &&
              diagnostics.due(began + std::chrono::seconds(1) + std::chrono::microseconds(1)),
          "slow diagnostics did not retain the one-second window");
    const auto summary = diagnostics.take();
    check(summary.find("\"sequence\":25,") != std::string::npos &&
              summary.find("\"sampled_polls\":10000") != std::string::npos &&
              summary.find("\"first_sequence\":1,") != std::string::npos &&
              summary.find("\"last_sequence\":10000,") != std::string::npos &&
              summary.find("\"max_total_ns\":50000000,") != std::string::npos &&
              summary.find("FORTS_TRADE_REPL") != std::string::npos && diagnostics.take().empty(),
          "sampled diagnostics lost the population or actual worst native turn");
    check(diagnostics.observe(began + std::chrono::seconds(2), 10001, 3000000),
          "new diagnostic window retained the previous maximum");
    diagnostics.capture("{\"sequence\":10001,\"total_ns\":3000000,\"callbacks\":[]}");
    const auto final = diagnostics.take(); // Shutdown must flush a partial window too.
    check(final.find("\"sampled_polls\":1,") != std::string::npos &&
              final.find("\"max_total_ns\":3000000,") != std::string::npos && diagnostics.take().empty(),
          "last slow turn was lost or duplicated at shutdown");
}
inline void host_timing_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                   const std::filesystem::path& root) {
    owner_poll_diagnostics_regression();
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
    check(!host.poll(), "timed rate-released reply");
    using enum plaza2::generated::StreamCode;
    using private_delta_host_detail::own_trade;
    // A row without its native commit must never produce a credit/timing.
    control.enqueue({.kind = plaza2::test::fake::EventKind::Begin, .stream_code = kFortsTradeRepl});
    control.enqueue(own_trade(97001, 1001, "BRK1C01", 67001));
    check(!host.poll(), "timed uncommitted own deal");
    check(host.status().find("\"executed\":1") == std::string::npos, "uncommitted deal credited");
    control.enqueue({.kind = plaza2::test::fake::EventKind::Commit, .stream_code = kFortsTradeRepl});
    control.enqueue({.kind = plaza2::test::fake::EventKind::Begin, .stream_code = kFortsTradeRepl});
    control.enqueue(own_trade(97002, 1001, "BRK1C01", 67002));
    control.enqueue({.kind = plaza2::test::fake::EventKind::Commit, .stream_code = kFortsTradeRepl});
    check(!host.poll(), "timed distinct committed own deals");
    // Native replay of the same deal is not a second execution or sample.
    control.enqueue({.kind = plaza2::test::fake::EventKind::Begin, .stream_code = kFortsTradeRepl});
    control.enqueue(own_trade(97001, 1001, "BRK1C01", 67001));
    control.enqueue({.kind = plaza2::test::fake::EventKind::Commit, .stream_code = kFortsTradeRepl});
    check(!host.poll(), "timed duplicate committed own deal");
    check(!host.stop(), "timed host stop");
    std::ifstream journal(config.journal_path);
    std::string line;
    bool slow{}, immediate{}, held{};
    unsigned reply_timings{}, trade_timings{}, trade_credits{};
    while (std::getline(journal, line)) {
        if (line.find("\"event\":\"slow_owner_poll\"") != std::string::npos) {
            slow |= line.find("\"before_session_ns\":") != std::string::npos &&
                    line.find("\"journal_flush_ns\":") != std::string::npos &&
                    line.find("\"process_ns\":") != std::string::npos && line.find("\"events\":[") != std::string::npos;
            check(line.find("\"sampled_polls\":") != std::string::npos &&
                      line.find("\"max_total_ns\":") != std::string::npos,
                  "host still emitted unbounded per-turn diagnostics");
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
        if (line.find("\"event\":\"reply_timing\"") != std::string::npos) {
            ++reply_timings;
            check(line.find("\"callback_to_on_reply_ns\":") != std::string::npos &&
                      line.find("\"callback_to_on_reply_ns\":0") == std::string::npos,
                  "actual on_reply lacks positive callback latency");
        }
        if (line.find("\"event\":\"trade\"") != std::string::npos)
            ++trade_credits;
        if (line.find("\"event\":\"trade_timing\"") != std::string::npos) {
            ++trade_timings;
            check(line.find("\"commit_to_credit_ns\":") != std::string::npos &&
                      line.find("\"commit_to_credit_ns\":0") == std::string::npos,
                  "actual own execution lacks positive native commit latency");
        }
    }
    check(slow && immediate && held, "missing attributed slow poll or actual publisher timing");
    check(reply_timings == 2 && trade_timings == 2 && trade_credits == 2,
          "reply/credit timing omitted actual application or counted duplicate/uncommitted deals");
    control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
}
} // namespace moex::connector_host::regression
