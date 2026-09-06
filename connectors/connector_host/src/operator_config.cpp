#include "moex/connector_host/operator_config.hpp"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace moex::connector_host {
namespace {
namespace cg = plaza2::cgate;
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

std::string_view operator_help() noexcept {
    return R"(moexctl plaza2 {status|qualify|order-test} [options]
TEST only. No raw add/cancel/recover commands. Run in a dedicated writable workdir.
Required: --runtime-root PATH --scheme-dir PATH --config-dir PATH
          --env-settings-var NAME --broker-code-env NAME --client-code-env NAME
          --isin-id N --session-id N
Runtime:  --library-path PATH --expected-release TEXT --scheme-sha256 SHA
          --credentials-env NAME --software-key-env NAME
          --armed-test-network --armed-test-session --armed-test-plaza2
Output:   --json --wait-ms N (maximum 60000)
Order:    --armed-test-order-send --plan FILE --authorize-sha256 SHA
          --price DECIMAL --side {buy|sell} --base-contract CODE [--comment TEXT]
          --ext-id N --add-user-id N --cancel-user-id N --recovery-user-id N
          --run-id ID --journal-root PATH --receipt-path PATH
          --profile-id ID --profile-fingerprint SHA --policy-version ID --policy-sha256 SHA
Order quantity is exactly 1, LIMIT; zero position required, at most 4 ticks/5000ms.
qualify/status reject the send arm. Canonical plan input is byte-exact (no reformatting).
)";
}

Plaza2HostConfig build_plaza2_host_config(const Plaza2HostConfigInputs& inputs) {
    namespace cg = plaza2::cgate;
    using plaza2::generated::StreamCode;

    if (inputs.runtime_root.empty() || inputs.scheme_dir.empty() || inputs.config_dir.empty() ||
        inputs.env_open_settings.empty() || inputs.broker_code.empty() || inputs.client_code.empty() ||
        inputs.isin_id <= 0 || inputs.session_id <= 0)
        throw std::invalid_argument("incomplete ConnectorHost TEST configuration");

    Plaza2HostConfig out;
    out.purpose = inputs.purpose;
    auto& host = out.transport.host;
    host.mode = inputs.purpose == HostPurpose::OrderTest ? Plaza2TestSessionHostMode::LiveTestAuthorizedSend
                                                         : Plaza2TestSessionHostMode::LiveTestPreSend;
    host.runtime.environment = cg::Plaza2Environment::Test;
    host.runtime.runtime_root = inputs.runtime_root;
    host.runtime.library_path = inputs.library_path;
    host.runtime.scheme_dir = inputs.scheme_dir;
    host.runtime.config_dir = inputs.config_dir;
    host.runtime.env_open_settings = inputs.env_open_settings;
    host.runtime.expected_spectra_release = inputs.expected_spectra_release;
    host.runtime.expected_scheme_sha256 = inputs.expected_scheme_sha256;
    host.endpoint_host = "127.0.0.1";
    host.arm_state = inputs.arm_state;
    host.publisher_name = inputs.publisher_name.empty()
                              ? "connector_host_" + std::to_string(::getpid()) + "_" +
                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                              : inputs.publisher_name;
    host.connection_settings = "p2tcp://127.0.0.1:4101;app_name=" + host.publisher_name + ";timeout=2000";
    const auto scheme = std::filesystem::absolute(host.runtime.scheme_dir).string();
    const auto stream = [&](StreamCode code, std::string name, std::string alias) {
        return Plaza2TestTradeStreamConfig{.stream_code = code,
                                           .settings = "p2repl://" + name + ";scheme=|FILE|" + scheme +
                                                       "/forts_scheme.ini|" + alias};
    };
    host.private_streams = {stream(StreamCode::kFortsTradeRepl, "FORTS_TRADE_REPL", "Trade"),
                            stream(StreamCode::kFortsUserorderbookRepl, "FORTS_USERORDERBOOK_REPL", "OrdBook"),
                            stream(StreamCode::kFortsPosRepl, "FORTS_POS_REPL", "POS"),
                            stream(StreamCode::kFortsPartRepl, "FORTS_PART_REPL", "PART"),
                            stream(StreamCode::kFortsRefdataRepl, "FORTS_REFDATA_REPL", "REFDATA")};
    host.status_streams = {
        {.stream_code = StreamCode::kFortsSessionstateRepl, .settings = "p2repl://FORTS_SESSIONSTATE_REPL"},
        {.stream_code = StreamCode::kFortsInstrumentstateRepl, .settings = "p2repl://FORTS_INSTRUMENTSTATE_REPL"}};
    host.aggr20_stream = stream(StreamCode::kFortsAggrRepl, "FORTS_AGGR20_REPL", "Aggr");
    host.publisher_settings = "p2mq://FORTS_SRV;category=FORTS_MSG;name=" + host.publisher_name +
                              ";timeout=5000;scheme=|FILE|" + scheme + "/forts_messages.ini|message";
    host.p2mqreply_settings = "p2mqreply://;ref=" + host.publisher_name;
    host.trade_replay_from_pos_anchor = true;
    host.credentials = {.source = cg::Plaza2CredentialSource::Env, .env_var = inputs.credentials_env_var};
    host.software_key = {.source = cg::Plaza2CredentialSource::Env, .env_var = inputs.software_key_env_var};

    auto& order = out.order;
    order.environment = cg::Plaza2Environment::Test;
    order.profile_enabled = true;
    order.profile_id = inputs.profile_id;
    order.profile_fingerprint = inputs.profile_fingerprint;
    order.isin_id = static_cast<std::int32_t>(inputs.isin_id);
    order.base_contract_code = inputs.base_contract_code;
    order.instrument_mask = 1;
    order.broker_code = inputs.broker_code;
    order.client_code = inputs.client_code;
    order.side = inputs.side;
    order.order_type = Plaza2TradeOrderType::Limit;
    order.price = inputs.price;
    order.quantity = 1;
    order.ext_id = inputs.ext_id;
    order.add_user_id = inputs.add_user_id;
    order.cancel_user_id = inputs.cancel_user_id;
    order.recovery_user_id = inputs.recovery_user_id;
    order.comment = inputs.comment;
    order.run_id = inputs.run_id;
    order.journal_root = inputs.journal_root.empty() ? inputs.runtime_root / "journals" : inputs.journal_root;
    order.policy = {.version = inputs.policy_version,
                    .sha256 = inputs.policy_sha256.empty()
                                  ? cg::plaza2_sha256_hex(inputs.policy_version + ":qty1:distance4:age5000:zero")
                                  : inputs.policy_sha256,
                    .max_distance_ticks = 4,
                    .max_aggr20_age_ms = 5000,
                    .require_zero_starting_position = true};

    out.transport.target_isin_id = inputs.isin_id;
    out.transport.target_session_id = inputs.session_id;
    out.transport.observation_client_code = inputs.broker_code + inputs.client_code;
    out.transport.require_zero_starting_position = true;
    out.transport.max_aggr20_age = std::chrono::milliseconds(5000);
    out.transport.target_max_distance_ticks = 4;
    out.transport.execution_safety_receipt_path = inputs.receipt_path;
    return out;
}

OperatorRequest parse_operator_arguments(std::span<const std::string_view> args) {
    OperatorRequest out;
    if (args.size() == 1 && args[0] == "--help") {
        out.help = true;
        return out;
    }
    if (args.size() < 2 || args[0] != "plaza2" ||
        (args[1] != "status" && args[1] != "qualify" && args[1] != "order-test"))
        throw std::invalid_argument("expected plaza2 status, qualify, or order-test; see --help");
    out.command = args[1];
    const bool order = out.command == "order-test";
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
    const std::set<std::string_view> flag_names{"--json", "--armed-test-network", "--armed-test-session",
                                                "--armed-test-plaza2", "--armed-test-order-send"};
    const std::set<std::string_view> value_names{"--runtime-root",
                                                 "--scheme-dir",
                                                 "--config-dir",
                                                 "--library-path",
                                                 "--expected-release",
                                                 "--scheme-sha256",
                                                 "--env-settings-var",
                                                 "--broker-code-env",
                                                 "--client-code-env",
                                                 "--isin-id",
                                                 "--session-id",
                                                 "--credentials-env",
                                                 "--software-key-env",
                                                 "--wait-ms",
                                                 "--plan",
                                                 "--authorize-sha256",
                                                 "--price",
                                                 "--comment",
                                                 "--side",
                                                 "--base-contract",
                                                 "--ext-id",
                                                 "--add-user-id",
                                                 "--cancel-user-id",
                                                 "--recovery-user-id",
                                                 "--run-id",
                                                 "--journal-root",
                                                 "--receipt-path",
                                                 "--profile-id",
                                                 "--profile-fingerprint",
                                                 "--policy-version",
                                                 "--policy-sha256"};
    for (std::size_t i = 2; i < args.size(); ++i) {
        const auto key = args[i];
        if (flag_names.contains(key)) {
            if (!flags.emplace(key).second)
                throw std::invalid_argument("duplicate flag");
        } else if (value_names.contains(key)) {
            if (++i == args.size() || !values.emplace(std::string(key), args[i]).second)
                throw std::invalid_argument("missing value or duplicate option");
        } else
            throw std::invalid_argument("unknown option; see --help");
    }
    const auto get = [&](const char* key, std::string fallback = {}) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    };
    const auto required = [&](const char* key) {
        auto value = get(key);
        if (value.empty())
            throw std::invalid_argument(std::string("required option: ") + key);
        return value;
    };
    Plaza2HostConfigInputs inputs;
    inputs.purpose = order ? HostPurpose::OrderTest : HostPurpose::Qualify;
    inputs.runtime_root = required("--runtime-root");
    inputs.scheme_dir = required("--scheme-dir");
    inputs.config_dir = required("--config-dir");
    inputs.library_path = get("--library-path");
    inputs.expected_spectra_release = get("--expected-release", "SPECTRA9.9.0");
    inputs.expected_scheme_sha256 = get("--scheme-sha256");
    inputs.env_open_settings = environment(required("--env-settings-var"));
    inputs.credentials_env_var = get("--credentials-env", "MOEX_PLAZA2_TEST_CREDENTIALS");
    inputs.software_key_env_var = get("--software-key-env", "MOEX_PLAZA2_CGATE_SOFTWARE_KEY");
    inputs.broker_code = environment(required("--broker-code-env"));
    inputs.client_code = environment(required("--client-code-env"));
    inputs.isin_id = integer<std::int64_t>(required("--isin-id"));
    inputs.session_id = integer<std::int32_t>(required("--session-id"));
    inputs.arm_state = {.test_network_armed = flags.contains("--armed-test-network"),
                        .test_session_armed = flags.contains("--armed-test-session"),
                        .test_plaza2_armed = flags.contains("--armed-test-plaza2"),
                        .test_order_send_armed = flags.contains("--armed-test-order-send")};
    inputs.policy_version = get("--policy-version", "moexctl-test-v1");
    inputs.policy_sha256 =
        get("--policy-sha256", cg::plaza2_sha256_hex(inputs.policy_version + ":qty1:distance4:age5000:zero"));
    if (order) {
        if (!inputs.arm_state.test_network_armed || !inputs.arm_state.test_session_armed ||
            !inputs.arm_state.test_plaza2_armed || !inputs.arm_state.test_order_send_armed)
            throw std::invalid_argument("order-test requires all four explicit TEST arms");
        out.canonical_plan_path = required("--plan");
        out.authorized_sha256 = required("--authorize-sha256");
        inputs.profile_id = required("--profile-id");
        inputs.profile_fingerprint = required("--profile-fingerprint");
        inputs.run_id = required("--run-id");
        inputs.journal_root = required("--journal-root");
        inputs.base_contract_code = required("--base-contract");
        inputs.price = required("--price");
        inputs.comment = get("--comment");
        const auto side = required("--side");
        if (side != "buy" && side != "sell")
            throw std::invalid_argument("side must be buy or sell");
        inputs.side = side == "buy" ? Plaza2TradeSide::Buy : Plaza2TradeSide::Sell;
        inputs.ext_id = integer<std::int32_t>(required("--ext-id"));
        inputs.add_user_id = integer<std::uint32_t>(required("--add-user-id"));
        inputs.cancel_user_id = integer<std::uint32_t>(required("--cancel-user-id"));
        inputs.recovery_user_id = integer<std::uint32_t>(required("--recovery-user-id"));
        inputs.receipt_path = required("--receipt-path");
    } else if (inputs.arm_state.test_order_send_armed || values.contains("--plan") ||
               values.contains("--authorize-sha256"))
        throw std::invalid_argument("status/qualify cannot authorize or arm order submission");
    out.config = build_plaza2_host_config(inputs);
    out.json = flags.contains("--json");
    out.wait_ms = integer<std::uint32_t>(get("--wait-ms", "10000"));
    if (out.wait_ms > 60000)
        throw std::invalid_argument("wait-ms must not exceed 60000");
    return out;
}
} // namespace moex::connector_host
