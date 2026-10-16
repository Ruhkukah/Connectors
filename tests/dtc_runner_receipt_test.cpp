#define main moex_connector_host_dtc_runner_application_main
#include "../apps/moex_connector_host_dtc_runner.cpp"
#undef main

namespace {
using namespace moex::connector_host::dtc;

class ReceiptSource final : public DtcMarketDataSource {
  public:
    DtcMarketDataSnapshot value;

    DtcMarketDataSnapshot snapshot() const override {
        return value;
    }
    DtcReadOnlyCapabilities capabilities() const noexcept override {
        return {.market_data = false,
                .market_depth = true,
                .security_definitions = true,
                .accounts = false,
                .positions = false,
                .orders = false,
                .order_entry = false};
    }
};

void runner_instance_regression() {
    using namespace moex::connector_host;
    ::setenv("MOEX_DTC_TEST_ENV", "ini=unused.ini;key=unused", 1);
    const std::vector<std::string> base{
        "dtc-test", "plaza2",       "qualify", "--runtime-root",     "/unused",           "--scheme-dir",
        "scheme",   "--config-dir", "config",  "--env-settings-var", "MOEX_DTC_TEST_ENV", "--isin-id",
        "42"};
    const auto runner_config = [&](std::vector<std::string> args) {
        std::vector<char*> argv;
        for (auto& arg : args)
            argv.push_back(arg.data());
        auto options = parse_options(static_cast<int>(argv.size()), argv.data());
        options.host_arguments.push_back("--read-only-market-data");
        return parse_operator_arguments(options.host_arguments).config.transport.host;
    };
    const auto check = [](bool passed, const char* message) {
        if (!passed)
            throw std::runtime_error(message);
    };
    auto common_args = std::vector<std::string_view>(base.begin() + 1, base.end());
    common_args.push_back("--read-only-market-data");
    const auto common = parse_operator_arguments(common_args).config.transport.host;
    const auto runner = runner_config(base);
    check(common.connection_settings.find(";app_name=moex_connector;") != std::string::npos,
          "common ConnectorHost default instance changed");
    check(runner.connection_settings.find(";app_name=moex_connector_dtc;") != std::string::npos &&
              runner.connection_settings != common.connection_settings,
          "DTC runner shares the default CGate application instance");
    check(runner.read_only_market_data && !runner.allow_orders && runner.publisher_name.empty() &&
              runner.publisher_settings.empty() && runner.p2mqreply_settings.empty(),
          "DTC instance selection opened a trading surface");
    for (const auto* explicit_id : {"dtc-custom", "moex_connector"}) {
        auto args = base;
        args.insert(args.end(), {"--instance-id", explicit_id});
        const auto configured = runner_config(std::move(args));
        check(configured.connection_settings.find(";app_name=" + std::string(explicit_id) + ";") != std::string::npos,
              "DTC runner replaced an explicit instance ID");
    }
}
} // namespace

int main() {
    using namespace moex::connector_host;
    runner_instance_regression();
    ReceiptSource source;
    const auto malformed = std::string("\xd0\x90", 2) + R"( "quoted" \)" + std::string("\0\x01\x1f\xff", 4);
    source.value.symbol = malformed;
    source.value.underlying_board = malformed;
    source.value.currency = malformed;
    source.value.min_step = malformed;
    source.value.description = malformed;
    source.value.contract_size = malformed;
    source.value.currency_value_per_increment = malformed;
    source.value.future_vcb_base_contract_code = malformed;
    source.value.session_id = 321;
    source.value.isin_id = 1001;

    DtcReadOnlyServerConfig server_config;
    server_config.source_mode = DtcSourceMode::LiveTest;
    DtcReadOnlyServer server(source, server_config);
    Options options;
    ConnectorHostSnapshot host_snapshot;
    host_snapshot.runtime_compatibility = "Compatible";
    host_snapshot.runtime_scheme_sha256 = std::string(64, 'a');
    print_startup_receipt(options, server, source.value, std::string(64, 'b'), source.capabilities(), host_snapshot);
    return 0;
}
