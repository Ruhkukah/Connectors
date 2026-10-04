#include "moex/connector_host/operator_config.hpp"
#include <charconv>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>

namespace moex::connector_host {
namespace {
using namespace plaza2_trade;
template <class T> T integer(std::string_view value) {
    T out{};
    auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || out <= 0)
        throw std::invalid_argument("expected a positive integer");
    return out;
}
std::string environment(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (!value || !*value)
        throw std::invalid_argument("required environment variable is missing");
    return value;
}
} // namespace
std::pair<std::string, std::uint16_t> router_address(std::string_view router) {
    if (router.find_first_of("; /\\\r\n") != std::string_view::npos)
        throw std::invalid_argument("router must be host:port");
    const auto colon = router.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == router.size())
        throw std::invalid_argument("router must be host:port");
    return {std::string(router.substr(0, colon)), integer<std::uint16_t>(router.substr(colon + 1))};
}
std::string_view operator_help() noexcept {
    return R"(moexctl plaza2 {status|qualify|run} [options]
Required: --runtime-root PATH --scheme-dir PATH --config-dir PATH
          --env-settings-var NAME --isin-id N [--isin-id N ...]
Trading:  --broker-code-env NAME --client-code-env NAME --allow-orders
Runtime:  --router HOST:PORT --environment {test|prod} --instance-id NAME
          --library-path PATH --expected-release TEXT --credentials-env NAME
          --software-key-env NAME --local-pass-env NAME --max-commands-per-second N
Output:   --json --wait-ms N
Read-only DTC: --read-only-market-data [--public-deals]
The current session is followed from committed REFDATA and SESSIONSTATE.
Public DEALS is opt-in and excluded from the first certificate.
)";
}
Plaza2HostConfig build_plaza2_host_config(const Plaza2HostConfigInputs& inputs) {
    namespace cg = plaza2::cgate;
    using plaza2::generated::StreamCode;
    if (inputs.public_deals && !inputs.read_only_market_data)
        throw std::invalid_argument("public DEALS requires read-only market data mode");
    if (inputs.read_only_market_data && inputs.allow_orders)
        throw std::invalid_argument("read-only market data cannot allow orders");
    auto ids = inputs.isin_ids;
    if (ids.empty() && inputs.isin_id > 0)
        ids.push_back(inputs.isin_id);
    if (inputs.runtime_root.empty() || inputs.scheme_dir.empty() || inputs.config_dir.empty() ||
        inputs.env_open_settings.empty() || ids.empty() ||
        (!inputs.read_only_market_data && (inputs.broker_code.empty() || inputs.client_code.empty())))
        throw std::invalid_argument("incomplete ConnectorHost configuration");
    for (const auto id : ids)
        if (id <= 0 || id > std::numeric_limits<std::int32_t>::max())
            throw std::invalid_argument("ISIN id must be a positive int32");
    if (!inputs.publisher_messages_per_second)
        throw std::invalid_argument("command rate must be positive");
    const auto [endpoint, port] = router_address(inputs.router);
    if (inputs.publisher_name.empty() || inputs.publisher_name.find_first_of(";\r\n") != std::string::npos)
        throw std::invalid_argument("instance id is invalid");
    Plaza2HostConfig out;
    out.purpose = inputs.purpose;
    out.isin_ids = std::move(ids);
    out.read_only_market_data = inputs.read_only_market_data;
    out.transport.target_isin_id = out.isin_ids.front();
    auto& host = out.transport.host;
    host.read_only_market_data = inputs.read_only_market_data;
    host.allow_orders = inputs.allow_orders;
    host.mode = CgateSessionMode::Live;
    host.runtime.environment = inputs.environment;
    host.runtime.runtime_root = std::filesystem::absolute(inputs.runtime_root).lexically_normal();
    host.runtime.library_path = inputs.library_path;
    const auto runtime_path = [&](const std::filesystem::path& path) {
        return (path.is_absolute() ? path : host.runtime.runtime_root / path).lexically_normal();
    };
    host.runtime.scheme_dir = runtime_path(inputs.scheme_dir);
    host.runtime.config_dir = runtime_path(inputs.config_dir);
    host.runtime.env_open_settings = inputs.env_open_settings;
    host.runtime.expected_spectra_release = inputs.expected_spectra_release;
    host.runtime.expected_scheme_sha256 = inputs.expected_scheme_sha256;
    host.publisher_messages_per_second = inputs.publisher_messages_per_second;
    host.market_data_isin_ids = inputs.isin_ids;
    host.publisher_name = inputs.read_only_market_data ? std::string{} : inputs.publisher_name;
    host.connection_settings =
        "p2tcp://" + endpoint + ":" + std::to_string(port) + ";app_name=" + inputs.publisher_name + ";timeout=2000";
    if (!inputs.local_pass_env_var.empty()) {
        const auto password = environment(inputs.local_pass_env_var);
        if (password.find_first_of(";\r\n") != std::string::npos)
            throw std::invalid_argument("local router password contains a settings delimiter");
        host.connection_settings += ";local_pass=" + password;
    }
    const auto scheme = host.runtime.scheme_dir.string();
    const auto stream = [&](StreamCode code, std::string name) {
        return CgateStreamConfig{
            .stream_code = code, .settings = "p2repl://" + name, .open_settings = "mode=snapshot+online"};
    };
    if (inputs.read_only_market_data)
        host.private_streams = {stream(StreamCode::kFortsRefdataRepl, "FORTS_REFDATA_REPL")};
    else
        host.private_streams = {stream(StreamCode::kFortsTradeRepl, "FORTS_TRADE_REPL"),
                                stream(StreamCode::kFortsUserorderbookRepl, "FORTS_USERORDERBOOK_REPL"),
                                stream(StreamCode::kFortsPosRepl, "FORTS_POS_REPL"),
                                stream(StreamCode::kFortsPartRepl, "FORTS_PART_REPL"),
                                stream(StreamCode::kFortsRefdataRepl, "FORTS_REFDATA_REPL")};
    host.status_streams = {stream(StreamCode::kFortsSessionstateRepl, "FORTS_SESSIONSTATE_REPL"),
                           stream(StreamCode::kFortsInstrumentstateRepl, "FORTS_INSTRUMENTSTATE_REPL")};
    host.aggr20_stream = stream(StreamCode::kFortsAggrRepl, "FORTS_AGGR20_REPL");
    if (inputs.public_deals)
        host.public_deals_stream = stream(StreamCode::kFortsDealsRepl, "FORTS_DEALS_REPL");
    if (!inputs.read_only_market_data) {
        host.publisher_settings = "p2mq://FORTS_SRV;category=FORTS_MSG;name=" + host.publisher_name +
                                  ";timeout=60000;scheme=|FILE|" + scheme + "/forts_messages.ini|message";
        host.p2mqreply_settings = "p2mqreply://;ref=" + host.publisher_name;
        host.trade_replay_from_pos_anchor = true;
    }
    if (!inputs.credentials_env_var.empty())
        host.credentials = {.source = cg::Plaza2CredentialSource::Env, .env_var = inputs.credentials_env_var};
    host.software_key = {.source = cg::Plaza2CredentialSource::Env, .env_var = inputs.software_key_env_var};
    out.order.broker_code = inputs.broker_code;
    out.order.client_code = inputs.client_code;
    out.order.isin_id = static_cast<std::int32_t>(out.transport.target_isin_id);
    return out;
}
OperatorRequest parse_operator_arguments(std::span<const std::string_view> args) {
    OperatorRequest out;
    if (args.size() == 1 && args[0] == "--help") {
        out.help = true;
        return out;
    }
    if (args.size() < 2 || args[0] != "plaza2" || (args[1] != "status" && args[1] != "qualify" && args[1] != "run"))
        throw std::invalid_argument("expected plaza2 status, qualify or run; see --help");
    out.command = args[1];
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
    const std::set<std::string_view> flag_names{"--json", "--allow-orders", "--read-only-market-data",
                                                "--public-deals"};
    const std::set<std::string_view> value_names{"--runtime-root",
                                                 "--scheme-dir",
                                                 "--config-dir",
                                                 "--library-path",
                                                 "--expected-release",
                                                 "--env-settings-var",
                                                 "--broker-code-env",
                                                 "--client-code-env",
                                                 "--isin-id",
                                                 "--router",
                                                 "--environment",
                                                 "--instance-id",
                                                 "--credentials-env",
                                                 "--software-key-env",
                                                 "--local-pass-env",
                                                 "--wait-ms",
                                                 "--max-commands-per-second"};
    Plaza2HostConfigInputs inputs;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const auto key = args[i];
        if (flag_names.contains(key)) {
            if (!flags.emplace(key).second)
                throw std::invalid_argument("duplicate flag");
        } else if (value_names.contains(key)) {
            if (++i == args.size())
                throw std::invalid_argument("missing option value");
            if (key == "--isin-id")
                inputs.isin_ids.push_back(integer<std::int64_t>(args[i]));
            else if (!values.emplace(std::string(key), args[i]).second)
                throw std::invalid_argument("duplicate option");
        } else
            throw std::invalid_argument("unknown option; see --help");
    }
    const auto get = [&](const char* key, std::string fallback = {}) {
        const auto found = values.find(key);
        return found == values.end() ? fallback : found->second;
    };
    const auto required = [&](const char* key) {
        auto value = get(key);
        if (value.empty())
            throw std::invalid_argument(std::string("required option: ") + key);
        return value;
    };
    inputs.purpose = out.command == "run" ? HostPurpose::Trade : HostPurpose::Qualify;
    inputs.allow_orders = flags.contains("--allow-orders");
    inputs.read_only_market_data = flags.contains("--read-only-market-data");
    inputs.public_deals = flags.contains("--public-deals");
    inputs.runtime_root = required("--runtime-root");
    inputs.scheme_dir = required("--scheme-dir");
    inputs.config_dir = required("--config-dir");
    inputs.library_path = get("--library-path");
    inputs.expected_spectra_release = get("--expected-release", "SPECTRA9.9.0");
    inputs.env_open_settings = environment(required("--env-settings-var"));
    inputs.credentials_env_var = get("--credentials-env");
    inputs.local_pass_env_var = get("--local-pass-env");
    inputs.software_key_env_var = get("--software-key-env", "MOEX_PLAZA2_CGATE_SOFTWARE_KEY");
    if (!inputs.read_only_market_data) {
        inputs.broker_code = environment(required("--broker-code-env"));
        inputs.client_code = environment(required("--client-code-env"));
    }
    inputs.router = get("--router", "127.0.0.1:4101");
    inputs.publisher_name = get("--instance-id", "moex_connector");
    const auto env = get("--environment", "test");
    if (env != "test" && env != "prod")
        throw std::invalid_argument("environment must be test or prod");
    inputs.environment =
        env == "test" ? plaza2::cgate::Plaza2Environment::Test : plaza2::cgate::Plaza2Environment::Prod;
    inputs.publisher_messages_per_second = integer<std::uint32_t>(get("--max-commands-per-second", "30"));
    out.config = build_plaza2_host_config(inputs);
    out.json = flags.contains("--json");
    out.wait_ms = integer<std::uint32_t>(get("--wait-ms", "10000"));
    return out;
}
} // namespace moex::connector_host
