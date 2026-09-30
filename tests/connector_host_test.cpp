#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/dtc_market_data.hpp"
#include "plaza2_runtime_test_support.hpp"
#include <cstdlib>
#include <iostream>

using namespace moex::connector_host;
namespace cg = moex::plaza2::cgate;
namespace test = moex::plaza2::test;

Plaza2HostConfig config(const test::RuntimeFixturePaths& fixture, bool read_only = false) {
    Plaza2HostConfigInputs inputs;
    inputs.read_only_market_data = read_only;
    inputs.runtime_root = fixture.root;
    inputs.library_path = fixture.library_path;
    inputs.scheme_dir = fixture.scheme_dir;
    inputs.config_dir = fixture.config_dir;
    inputs.env_open_settings = "ini=config/t1.ini;key=00000000";
    inputs.credentials_env_var = "MOEX_PLAZA2_TEST_CREDENTIALS";
    inputs.software_key_env_var = "MOEX_PLAZA2_CGATE_SOFTWARE_KEY";
    inputs.expected_spectra_release = "SPECTRA93";
    inputs.broker_code = "BRK1";
    inputs.client_code = "C01";
    inputs.isin_ids = {1001, 2002};
    inputs.router = "localhost:4102";
    auto out = build_plaza2_host_config(inputs);
    out.target_underlying_board = "RFUD";
    out.target_currency = "RUB";
    out.market_data_now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds{1700000100}}; };
    out.transport.host.process_timeout_ms = 0;
    return out;
}
void warm(ConnectorHost& host) {
    test::require(!host.start(), "host starts");
    for (int i = 0; i < 20; ++i)
        test::require(!host.poll(), "host polls");
}
int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake runtime path required");
        const auto path = test::make_temp_directory("readiness");
        const auto fixture =
            test::materialize_runtime_fixture(path, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test"));
        ::setenv("MOEX_PLAZA2_TEST_CREDENTIALS", "fake-test-only", 1);
        ::setenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY", "00000000", 1);
        ::setenv("MOEX_FAKE_CLIENT_CODE", "BRK1C01", 1);
        ::setenv("MOEX_FAKE_AGGR_SNAPSHOT_READY_ONLY", "1", 1);
        {
            Plaza2HostConfigInputs inputs;
            inputs.runtime_root = fixture.root;
            inputs.library_path = fixture.library_path;
            inputs.scheme_dir = std::filesystem::relative(fixture.scheme_dir, fixture.root);
            inputs.config_dir = std::filesystem::relative(fixture.config_dir, fixture.root);
            inputs.env_open_settings = "ini=config/t1.ini;key=00000000";
            inputs.credentials_env_var = "MOEX_PLAZA2_TEST_CREDENTIALS";
            inputs.software_key_env_var = "MOEX_PLAZA2_CGATE_SOFTWARE_KEY";
            inputs.broker_code = "BRK1";
            inputs.client_code = "C01";
            inputs.isin_ids = {1001};
            const auto cfg = build_plaza2_host_config(inputs);
            const auto& runtime = cfg.transport.host.runtime;
            test::require(runtime.scheme_dir == fixture.scheme_dir && runtime.config_dir == fixture.config_dir,
                          "relative scheme/config paths resolve against runtime root");
            test::require(cfg.transport.host.publisher_settings.find(fixture.scheme_dir.string()) !=
                                  std::string::npos &&
                              cfg.transport.host.private_streams.front().settings.find(fixture.scheme_dir.string()) !=
                                  std::string::npos,
                          "listener and publisher scheme paths use resolved runtime directory");
            ConnectorHost host(cfg);
            warm(host);
            test::require(host.order_entry_ready(1001), "relative runtime paths open all trading objects");
            test::require(!host.stop(), "relative-path host stops");
        }
        {
            const auto cfg = config(fixture);
            test::require(cfg.isin_ids.size() == 2 && cfg.transport.target_session_id == 0 &&
                              cfg.transport.host.connection_settings.starts_with("p2tcp://localhost:4102;"),
                          "router and many instruments configured without fixed session");
            test::require(cfg.transport.host.public_deals_stream.settings.empty(), "DEALS stays opt-in and off");
            ConnectorHost host(cfg);
            warm(host);
            if (!host.order_entry_ready(1001)) {
                const auto state = host.snapshot();
                std::cerr << render_snapshot(state, true) << "conn=" << state.transport_health.connection
                          << " pub=" << state.transport_health.publisher << " reply=" << state.transport_health.reply
                          << '\n';
                for (const auto& stream : state.streams)
                    std::cerr << stream.stream_name << " online=" << stream.online
                              << " snapshot=" << stream.snapshot_complete << '\n';
                for (const auto& instrument : host.qualification_snapshot().instruments)
                    std::cerr << "isin=" << instrument.isin_id << " sess=" << instrument.sess_id
                              << " member=" << instrument.current_session_member
                              << " status=" << instrument.current_status << " has=" << instrument.has_current_status
                              << " bound=" << instrument.current_status_refdata_bound << '\n';
            }
            test::require(host.order_entry_ready(1001),
                          "ONLINE private streams allow orders with existing position and working orders");
            test::require(host.market_data_snapshot().valid,
                          "snapshot event_type1 matching current session validates late join");
            const auto version = host.market_data_snapshot().source_snapshot_version;
            for (int i = 0; i < 10; ++i)
                test::require(!host.poll(), "quiet stream polls");
            test::require(host.market_data_snapshot().valid &&
                              host.market_data_snapshot().source_snapshot_version == version,
                          "quiet unchanged book stays valid without price-change freshness gate");
            test::require(!host.stop(), "host stops without serial epoch machinery");
        }
        {
            ::setenv("MOEX_FAKE_AGGR_WRONG_SESSION", "1", 1);
            ConnectorHost host(config(fixture));
            warm(host);
            test::require(!host.market_data_snapshot().valid,
                          "previous session ready event does not validate market data");
            test::require(host.order_entry_ready(1001), "AGGR readiness does not gate private order entry");
            test::require(!host.stop(), "wrong AGGR day host stops");
            ::unsetenv("MOEX_FAKE_AGGR_WRONG_SESSION");
        }
        {
            ConnectorHost host(config(fixture));
            warm(host);
            test::require(host.order_entry_ready(1001) && host.market_data_snapshot().valid,
                          "current day ready before prepublished session contents");
            ::setenv("MOEX_FAKE_PREPUBLISH_NEXT_SESSION", "1", 1);
            for (int i = 0; i < 5; ++i)
                test::require(!host.poll(), "prepublished next-day contents poll");
            ::unsetenv("MOEX_FAKE_PREPUBLISH_NEXT_SESSION");
            test::require(host.order_entry_ready(1001) && host.snapshot().session_id == 321 &&
                              host.market_data_snapshot().valid,
                          "prepublished future membership preserves current day orders and market data");
            ::setenv("MOEX_FAKE_LIVE_SESSION_SWITCH", "1", 1);
            for (int i = 0; i < 5; ++i)
                test::require(!host.poll(), "live session switch polls without reconnect");
            ::unsetenv("MOEX_FAKE_LIVE_SESSION_SWITCH");
            test::require(host.order_entry_ready(1001) && host.snapshot().session_id == 322,
                          "independent status and membership follow evening day without reconnect");
            test::require(host.market_data_snapshot().valid && host.market_data_snapshot().target_session_id == 322,
                          "current ready event validates market data after live day switch");
            test::require(!host.stop(), "live switched host stops");
        }
        {
            ::setenv("MOEX_FAKE_AGGR_EMPTY", "1", 1);
            ConnectorHost host(config(fixture));
            warm(host);
            test::require(host.order_entry_ready(1001), "no BBO does not gate order entry");
            test::require(!host.stop(), "empty book host stops");
            ::unsetenv("MOEX_FAKE_AGGR_EMPTY");
        }
        {
            ::setenv("MOEX_FAKE_STATUS_BEFORE_REFDATA", "1", 1);
            ConnectorHost host(config(fixture));
            warm(host);
            test::require(host.order_entry_ready(1001),
                          "independent ONLINE status snapshots allow either initial callback order");
            test::require(host.market_data_snapshot().valid,
                          "AGGR late join has no inherited status/refdata ordering gate");
            test::require(!host.stop(), "status-first host stops");
            ::unsetenv("MOEX_FAKE_STATUS_BEFORE_REFDATA");
        }
        {
            auto now = std::chrono::steady_clock::now();
            auto cfg = config(fixture);
            cfg.transport.host.recovery_now = [&] { return now; };
            ConnectorHost host(cfg);
            warm(host);
            test::require(host.order_entry_ready(1001), "orders ready before outage");
            ::setenv("MOEX_FAKE_CONNECTION_ERROR", "1", 1);
            test::require(!host.poll(), "outage is recoverable");
            ::unsetenv("MOEX_FAKE_CONNECTION_ERROR");
            test::require(!host.order_entry_ready(1001), "connection outage stops commands");
            ::setenv("MOEX_FAKE_SESSION_ID", "322", 1);
            now += std::chrono::seconds(2);
            for (int i = 0; i < 30; ++i) {
                test::require(!host.poll(), "reconnect polls");
                now += std::chrono::milliseconds(100);
            }
            if (!host.order_entry_ready(1001)) {
                const auto state = host.snapshot();
                std::cerr << render_snapshot(state, true);
                for (const auto& stream : state.streams)
                    std::cerr << stream.stream_name << " online=" << stream.online
                              << " snapshot=" << stream.snapshot_complete << '\n';
                for (const auto& instrument : host.qualification_snapshot().instruments)
                    std::cerr << "isin=" << instrument.isin_id << " sess=" << instrument.sess_id
                              << " member=" << instrument.current_session_member
                              << " status=" << instrument.current_status << " has=" << instrument.has_current_status
                              << " bound=" << instrument.current_status_refdata_bound << '\n';
            }
            test::require(host.order_entry_ready(1001) && host.snapshot().session_id == 322,
                          "reconnect follows new trading day and resumes commands without process restart");
            test::require(host.market_data_snapshot().valid && host.market_data_snapshot().target_session_id == 322,
                          "new current-day snapshot ready event validates market data after reconnect");
            test::require(!host.stop(), "reconnected host stops");
            ::unsetenv("MOEX_FAKE_SESSION_ID");
        }
        for (const char* flag : {"MOEX_FAKE_NONTRADABLE_SESSION", "MOEX_FAKE_NONTRADABLE_INSTRUMENT"}) {
            ::setenv(flag, "1", 1);
            ConnectorHost host(config(fixture));
            warm(host);
            test::require(!host.order_entry_ready(1001),
                          "exchange session/instrument states stop commands during clearing");
            test::require(!host.stop(), "nontradable host stops");
            ::unsetenv(flag);
        }
        {
            ConnectorHost host(config(fixture, true));
            warm(host);
            test::require(!host.has_publisher_or_reply_handles() && !host.order_entry_ready(1001),
                          "read-only DTC host owns no publisher/reply handles");
            const auto md = host.market_data_snapshot();
            test::require(md.valid && !md.order_entry_allowed,
                          "read-only DTC accepts current committed book and denies order entry");
            test::require(!host.stop(), "read-only host stops");
        }
        ::unsetenv("MOEX_FAKE_AGGR_SNAPSHOT_READY_ONLY");
        test::remove_tree(path);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
