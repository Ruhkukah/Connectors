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
} // namespace

int main() {
    using namespace moex::connector_host;
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
