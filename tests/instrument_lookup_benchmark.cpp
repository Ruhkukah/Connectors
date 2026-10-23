#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "fake_cgate_control.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "scope_exit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
namespace test = moex::plaza2::test;
namespace fake = test::fake;
namespace cg = moex::plaza2::cgate;
namespace gen = moex::plaza2::generated;
using namespace moex::connector_host;
using enum gen::FieldCode;
using Clock = std::chrono::steady_clock;
constexpr std::int32_t universe_size = 41000;
constexpr std::array<std::int32_t, 3> targets{1, universe_size / 2, universe_size};

fake::Field integer(gen::FieldCode code, std::int64_t value) {
    return {.field_code = code, .signed_value = value};
}
fake::Field text(gen::FieldCode code, std::string value) {
    return {.field_code = code, .kind = fake::FieldKind::Text, .text = std::move(value)};
}
std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
double percentile(std::vector<std::int64_t> samples, std::size_t numerator) {
    std::sort(samples.begin(), samples.end());
    return samples[(samples.size() - 1) * numerator / 100] / 1000.0;
}
void publish_universe(const fake::Control& control) {
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsRefdataRepl});
    for (std::int32_t isin = 1; isin <= universe_size; ++isin) {
        const auto revision = 100000 + isin;
        control.enqueue({.stream_code = gen::StreamCode::kFortsRefdataRepl,
                         .table_code = gen::TableCode::kFortsRefdataReplFutInstruments,
                         .revision = revision,
                         .fields = {integer(kFortsRefdataReplFutInstrumentsReplId, 500000 + isin),
                                    integer(kFortsRefdataReplFutInstrumentsIsinId, isin),
                                    text(kFortsRefdataReplFutInstrumentsIsin, "INDEX-" + std::to_string(isin)),
                                    text(kFortsRefdataReplFutInstrumentsBaseContractCode, "RTS")}});
        control.enqueue({.stream_code = gen::StreamCode::kFortsRefdataRepl,
                         .table_code = gen::TableCode::kFortsRefdataReplFutSessContents,
                         .revision = revision,
                         .fields = {integer(kFortsRefdataReplFutSessContentsReplId, 600000 + isin),
                                    integer(kFortsRefdataReplFutSessContentsIsinId, isin),
                                    integer(kFortsRefdataReplFutSessContentsSessId, 321),
                                    text(kFortsRefdataReplFutSessContentsIsin, "INDEX-" + std::to_string(isin)),
                                    text(kFortsRefdataReplFutSessContentsBaseContractCode, "RTS"),
                                    text(kFortsRefdataReplFutSessContentsSettlementPrice, "102500"),
                                    text(kFortsRefdataReplFutSessContentsMinStep, "250"),
                                    text(kFortsRefdataReplFutSessContentsLimitUp, "10000"),
                                    text(kFortsRefdataReplFutSessContentsLimitDown, "10000")}});
    }
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsRefdataRepl});
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsInstrumentstateRepl});
    for (std::int32_t isin = 1; isin <= universe_size; ++isin)
        control.enqueue({.stream_code = gen::StreamCode::kFortsInstrumentstateRepl,
                         .table_code = gen::TableCode::kFortsInstrumentstateReplInstrumentState,
                         .revision = 200000 + isin,
                         .fields = {integer(kFortsInstrumentstateReplInstrumentStateReplId, 700000 + isin),
                                    integer(kFortsInstrumentstateReplInstrumentStateIsinId, isin),
                                    integer(kFortsInstrumentstateReplInstrumentStatePublicState, 1)}});
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsInstrumentstateRepl});
}
void run(TradingHostConfig config, const fake::Control& control) {
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = "BRK1C01",
                       .session_id = 321});
    CgateTradingHost host(config);
    ScopeExit stop([&] { (void)host.stop(); });
    test::require(!host.start(), "41k trading-path startup");
    for (int i = 0; i < 30; ++i)
        test::require(!host.poll(), "41k trading-path bootstrap");
    publish_universe(control);
    bool committed{};
    for (int i = 0; i < 20000; ++i) {
        test::require(!host.poll(), "41k trading-path publication");
        if (i % 100)
            continue;
        const auto status = host.status();
        committed = status.find("\"symbol\":\"INDEX-41000\",\"order_entry_ready\":true") != std::string::npos;
        if (committed)
            break;
    }
    test::require(committed, "41k committed universe never became tradable");
    constexpr std::size_t samples = 100;
    double first_median{}, last_median{};
    for (const auto isin : targets) {
        std::vector<std::int64_t> place_to_post, place_total;
        for (std::size_t i = 0; i < samples; ++i) {
            const auto key = "lookup-" + std::to_string(isin) + '-' + std::to_string(i);
            const auto begin = steady_ns();
            const auto error = host.place({.client_order_id = key, .isin_id = isin, .price = "103000", .quantity = 1});
            const auto end = steady_ns();
            test::require(error.empty(), "41k trading-path Add refused: " + error);
            const auto post = control.last_post_started_steady_ns();
            test::require(begin <= post && post <= end, "41k Add timing did not bracket actual native post");
            place_to_post.push_back(post - begin);
            place_total.push_back(end - begin);
        }
        std::cout << "41k trading path {\"universe\":" << universe_size << ",\"isin_id\":" << isin
                  << ",\"samples\":" << samples << ",\"place_to_post_p50_us\":" << percentile(place_to_post, 50)
                  << ",\"place_to_post_p99_us\":" << percentile(place_to_post, 99)
                  << ",\"place_total_p50_us\":" << percentile(place_total, 50) << "}\n";
        if (isin == targets.front())
            first_median = percentile(place_to_post, 50);
        if (isin == targets.back())
            last_median = percentile(place_to_post, 50);
    }
#if MOEX_RELEASE_PERFORMANCE_ACCEPTANCE
    test::require(last_median <= first_median * 4 + 10,
                  "41k native trading path still scales linearly with instrument position");
#endif
    const auto commands = control.commands();
    test::require(
        commands.size() == samples * targets.size() &&
            std::all_of(commands.begin(), commands.end(),
                        [](const auto& command) { return command.name == "AddOrder" && command.result == 0; }),
        "41k benchmark did not perform all actual native Adds");
    const auto status_row = [&](std::int32_t state, std::int64_t revision) {
        control.enqueue({.stream_code = gen::StreamCode::kFortsInstrumentstateRepl,
                         .table_code = gen::TableCode::kFortsInstrumentstateReplInstrumentState,
                         .revision = revision,
                         .fields = {integer(kFortsInstrumentstateReplInstrumentStateReplId, 700000 + targets.back()),
                                    integer(kFortsInstrumentstateReplInstrumentStateIsinId, targets.back()),
                                    integer(kFortsInstrumentstateReplInstrumentStatePublicState, state)}});
    };
    const auto poll = [&] {
        for (int i = 0; i < 5; ++i)
            test::require(!host.poll(), "41k lookup lifecycle poll");
    };
    const auto place_last = [&](std::string label) {
        return host.place(
            {.client_order_id = std::move(label), .isin_id = targets.back(), .price = "103000", .quantity = 1});
    };
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsInstrumentstateRepl});
    status_row(2, 300000);
    poll();
    test::require(place_last("pending-status").empty() && control.commands().size() == commands.size() + 1,
                  "pending status update replaced the committed trading readiness");
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsInstrumentstateRepl});
    poll();
    test::require(!place_last("closed-status").empty() && control.commands().size() == commands.size() + 1,
                  "committed suspended status still authorized an actual Add");
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsInstrumentstateRepl});
    status_row(1, 300001);
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsInstrumentstateRepl});
    poll();
    test::require(place_last("restored-status").empty() && control.commands().size() == commands.size() + 2,
                  "fresh committed status did not restore native admission");
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsRefdataRepl});
    control.enqueue({.kind = fake::EventKind::ClearDeleted,
                     .stream_code = gen::StreamCode::kFortsRefdataRepl,
                     .table_code = gen::TableCode::kFortsRefdataReplFutSessContents,
                     .revision = std::numeric_limits<std::int64_t>::max()});
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsRefdataRepl});
    poll();
    test::require(!place_last("retired-membership").empty() && control.commands().size() == commands.size() + 2,
                  "deleted current-session membership authorized native Add");
    test::require(!host.stop(), "41k trading-path shutdown");
    stop.release();
}
} // namespace

int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake runtime path required");
        const auto root = test::make_temp_directory("instrument-lookup");
        ScopeExit clean([&] { test::remove_tree(root); });
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        fake::Control control(fixture.library_path);
        ::setenv("MOEX_LOOKUP_TEST_CREDENTIALS", "fake-test-only", 1);
        ::setenv("MOEX_LOOKUP_TEST_KEY", "00000000", 1);
        Plaza2HostConfigInputs input;
        input.runtime_root = fixture.root;
        input.library_path = fixture.library_path;
        input.scheme_dir = fixture.scheme_dir;
        input.config_dir = fixture.config_dir;
        input.env_open_settings = "ini=config/t1.ini;key=00000000";
        input.credentials_env_var = "MOEX_LOOKUP_TEST_CREDENTIALS";
        input.software_key_env_var = "MOEX_LOOKUP_TEST_KEY";
        input.broker_code = "BRK1";
        input.client_code = "C01";
        input.isin_ids.assign(targets.begin(), targets.end());
        input.allow_orders = true;
        TradingHostConfig config;
        config.session = build_plaza2_host_config(input).transport.host;
        config.session.mode = moex::plaza2_trade::CgateSessionMode::OfflineFake;
        config.journal_path = root / "lookup.ndjson";
        config.identity_state_path = root / "lookup.state";
        config.isin_ids.assign(targets.begin(), targets.end());
        config.orders.broker_code = input.broker_code;
        config.orders.client_code = input.client_code;
        config.orders.login_from = "owner-login";
        config.orders.ext_id_range_configured = config.orders.command_rate_configured = true;
        config.orders.max_commands_per_second = 3000;
        config.orders.risk.quantity_configured = config.orders.risk.open_orders_configured = true;
        config.orders.risk.max_quantity = 2;
        config.orders.risk.max_open_orders = 400;
        config.orders.risk.max_notional_scaled = std::numeric_limits<std::int64_t>::max();
        for (const auto isin : targets) {
            config.orders.risk.max_notional_by_isin[isin] = std::numeric_limits<std::int64_t>::max();
            config.orders.risk.max_position_by_isin[isin] = 400;
        }
        run(config, control);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
