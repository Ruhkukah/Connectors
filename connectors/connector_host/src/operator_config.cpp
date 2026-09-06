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
using plaza2::generated::StreamCode;
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
    auto& host = out.config.transport.host;
    host.arm_state = {.test_network_armed = flags.contains("--armed-test-network"),
                      .test_session_armed = flags.contains("--armed-test-session"),
                      .test_plaza2_armed = flags.contains("--armed-test-plaza2"),
                      .test_order_send_armed = flags.contains("--armed-test-order-send")};
    if (order && (!host.arm_state.test_network_armed || !host.arm_state.test_session_armed ||
                  !host.arm_state.test_plaza2_armed || !host.arm_state.test_order_send_armed))
        throw std::invalid_argument("order-test requires all four explicit TEST arms");
    if (!order &&
        (host.arm_state.test_order_send_armed || values.contains("--plan") || values.contains("--authorize-sha256")))
        throw std::invalid_argument("status/qualify cannot authorize or arm order submission");
    out.config.purpose = order ? HostPurpose::OrderTest : HostPurpose::Qualify;
    host.mode = order ? Plaza2TestSessionHostMode::LiveTestAuthorizedSend : Plaza2TestSessionHostMode::LiveTestPreSend;
    host.runtime.environment = cg::Plaza2Environment::Test;
    host.runtime.runtime_root = required("--runtime-root");
    host.runtime.scheme_dir = required("--scheme-dir");
    host.runtime.config_dir = required("--config-dir");
    host.runtime.library_path = get("--library-path");
    host.runtime.expected_spectra_release = get("--expected-release", "SPECTRA9.9.0");
    host.runtime.expected_scheme_sha256 = get("--scheme-sha256");
    host.runtime.env_open_settings = environment(required("--env-settings-var"));
    host.endpoint_host = "127.0.0.1";
    host.publisher_name = "moexctl_" + std::to_string(::getpid()) + "_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
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
    host.credentials = {.source = cg::Plaza2CredentialSource::Env,
                        .env_var = get("--credentials-env", "MOEX_PLAZA2_TEST_CREDENTIALS")};
    host.software_key = {.source = cg::Plaza2CredentialSource::Env,
                         .env_var = get("--software-key-env", "MOEX_PLAZA2_CGATE_SOFTWARE_KEY")};
    auto& c = out.config.order;
    c.environment = cg::Plaza2Environment::Test;
    c.profile_enabled = true;
    c.isin_id = integer<std::int32_t>(required("--isin-id"));
    c.broker_code = environment(required("--broker-code-env"));
    c.client_code = environment(required("--client-code-env"));
    c.policy = {.version = get("--policy-version", "moexctl-test-v1"),
                .sha256 = get("--policy-sha256", cg::plaza2_sha256_hex("moexctl-test-v1:qty1:distance4:age5000:zero")),
                .max_distance_ticks = 4,
                .max_aggr20_age_ms = 5000,
                .require_zero_starting_position = true};
    out.config.transport.target_isin_id = c.isin_id;
    out.config.transport.target_session_id = integer<std::int32_t>(required("--session-id"));
    out.config.transport.observation_client_code = c.broker_code + c.client_code;
    out.config.transport.require_zero_starting_position = true;
    out.config.transport.max_aggr20_age = std::chrono::milliseconds(5000);
    out.config.transport.target_max_distance_ticks = 4;
    if (order) {
        out.canonical_plan_path = required("--plan");
        out.authorized_sha256 = required("--authorize-sha256");
        c.profile_id = required("--profile-id");
        c.profile_fingerprint = required("--profile-fingerprint");
        c.run_id = required("--run-id");
        c.journal_root = required("--journal-root");
        c.base_contract_code = required("--base-contract");
        c.instrument_mask = 1;
        c.price = required("--price");
        c.comment = get("--comment");
        const auto side = required("--side");
        if (side != "buy" && side != "sell")
            throw std::invalid_argument("side must be buy or sell");
        c.side = side == "buy" ? Plaza2TradeSide::Buy : Plaza2TradeSide::Sell;
        c.quantity = 1;
        c.order_type = Plaza2TradeOrderType::Limit;
        c.ext_id = integer<std::int32_t>(required("--ext-id"));
        c.add_user_id = integer<std::uint32_t>(required("--add-user-id"));
        c.cancel_user_id = integer<std::uint32_t>(required("--cancel-user-id"));
        c.recovery_user_id = integer<std::uint32_t>(required("--recovery-user-id"));
        out.config.transport.execution_safety_receipt_path = required("--receipt-path");
    }
    out.json = flags.contains("--json");
    out.wait_ms = integer<std::uint32_t>(get("--wait-ms", "10000"));
    if (out.wait_ms > 60000)
        throw std::invalid_argument("wait-ms must not exceed 60000");
    return out;
}
} // namespace moex::connector_host
