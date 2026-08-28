#include "moex/plaza2_trade/plaza2_test_trade_transport.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <cstdlib>
#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

namespace cgate = moex::plaza2::cgate;
namespace generated = moex::plaza2::generated;
using namespace moex::plaza2_trade;

class ScopedEnv final {
  public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const auto* previous = std::getenv(name);
        if (previous != nullptr) {
            previous_ = std::string(previous);
        }
        if (value == nullptr) {
            ::unsetenv(name);
        } else {
            ::setenv(name, value, 1);
        }
    }
    ~ScopedEnv() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

Plaza2TestTradeStreamConfig stream(generated::StreamCode code, std::string_view name, std::string_view suffix = {}) {
    std::string settings = "p2repl://" + std::string(name) + ";scheme=|FILE|scheme/forts_scheme.ini|";
    settings += suffix.empty() ? std::string(name) : std::string(suffix);
    return {.stream_code = code, .settings = std::move(settings)};
}

Plaza2TestTradeTransportConfig make_config(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    using generated::StreamCode;
    Plaza2TestTradeTransportConfig config;
    config.host.runtime.environment = cgate::Plaza2Environment::Test;
    config.host.runtime.runtime_root = fixture.root;
    config.host.runtime.expected_spectra_release = "SPECTRA93";
    config.host.runtime.env_open_settings = "ini=config/t1.ini;key=00000000";
    config.host.connection_settings = "p2tcp://127.0.0.1:4001;app_name=plaza2_test_trade_transport";
    config.host.publisher_settings = "p2mq://PUB;category=FORTS_MSG";
    config.host.private_streams = {
        stream(StreamCode::kFortsTradeRepl, "FORTS_TRADE_REPL"),
        stream(StreamCode::kFortsUserorderbookRepl, "FORTS_USERORDERBOOK_REPL"),
        stream(StreamCode::kFortsPosRepl, "FORTS_POS_REPL"),
        stream(StreamCode::kFortsPartRepl, "FORTS_PART_REPL"),
        stream(StreamCode::kFortsRefdataRepl, "FORTS_REFDATA_REPL"),
    };
    config.host.aggr20_stream = stream(StreamCode::kFortsAggrRepl, "FORTS_AGGR20_REPL", "Aggr");
    config.host.process_timeout_ms = 0;
    config.target_isin_id = 1001;
    config.target_session_id = 321;
    config.max_aggr20_age = std::chrono::seconds(5);
    config.authorized_intent = Plaza2AuthorizedOrderIntent{
        .sha256 = std::string(64, 'a'),
        .profile_id = "offline-plaza2-test",
        .profile_fingerprint = std::string(64, 'e'),
        .environment = "test",
        .add_payload_sha256 = {},
        .recovery_payload_sha256 = {},
        .isin_id = 1001,
        .base_contract_code = "RTS",
        .side = Plaza2TradeSide::Sell,
        .price = "103000",
        .quantity = 1,
        .ext_id = 79,
        .add_user_id = 701,
        .cancel_user_id = 702,
        .recovery_user_id = 703,
        .instrument_mask = 1,
        .broker_code = "BRK1",
        .client_code = "C01",
        .broker_code_sha256 = cgate::plaza2_sha256_hex("BRK1"),
        .client_code_sha256 = cgate::plaza2_sha256_hex("C01"),
        .policy_version = "offline-v1",
        .policy_sha256 = std::string(64, 'd'),
        .max_distance_ticks = 4,
        .max_aggr20_age_ms = 5000,
        .require_zero_starting_position = false,
    };
    config.execution_safety_receipt_path = fixture.root / "execution_safety.json";
    config.target_tick_size = "250";
    config.target_price = "103000";
    config.target_side = Plaza2TradeSide::Sell;
    config.target_max_distance_ticks = 4;
    config.observation_ext_id = 79;
    config.observation_client_code = "BRK1C01";
    config.observation_side = Plaza2TradeSide::Sell;
    config.observation_quantity = 1;
    return config;
}

AddOrderRequest add_request() {
    AddOrderRequest request;
    request.broker_code = "BRK1";
    request.isin_id = 1001;
    request.client_code = "C01";
    request.dir = Plaza2TradeSide::Sell;
    request.type = Plaza2TradeOrderType::Limit;
    request.amount = 1;
    request.price = "103000";
    request.comment = "offline";
    request.ext_id = 79;
    request.is_check_limit = 1;
    return request;
}

DelUserOrdersRequest recovery_request() {
    DelUserOrdersRequest request;
    request.broker_code = "BRK1";
    request.buy_sell = 2;
    request.non_system = 0;
    request.code = "C01";
    request.base_contract_code = "RTS";
    request.ext_id = 79;
    request.isin_id = 1001;
    request.instrument_mask = 1;
    return request;
}

Plaza2TestTradeTransportConfig prepared_config(const moex::plaza2::test::RuntimeFixturePaths& fixture,
                                               const Plaza2TradeEncodedCommand& add,
                                               const Plaza2TradeEncodedCommand& recovery) {
    auto config = make_config(fixture);
    config.authorized_intent->add_payload_sha256 = cgate::plaza2_sha256_hex(add.payload);
    config.authorized_intent->recovery_payload_sha256 = cgate::plaza2_sha256_hex(recovery.payload);
    config.authorized_intent->canonical_json = canonical_authorized_order_intent_json(*config.authorized_intent);
    config.authorized_intent->sha256 = authorized_order_intent_sha256(*config.authorized_intent);
    return config;
}

void expect_case(bool condition, std::string_view message);

PreSendPlan bound_plan(const Plaza2AuthorizedOrderIntent& intent, const Plaza2TradeEncodedCommand& add,
                       const Plaza2TradeEncodedCommand& recovery) {
    return {
        .ok = true,
        .failure = PreSendFailure::None,
        .message = "test-bound plan",
        .canonical_json = intent.canonical_json,
        .sha256 = intent.sha256,
        .add_command = add,
        .exact_ext_id_recovery_command = recovery,
    };
}

void bind_test_plan(Plaza2TestTradeTransport& transport, const PreSendPlan& plan) {
    const auto binding = transport.bind_authorized_plan(plan);
    expect_case(!binding, "concrete transport test fixture must bind its exact static plan: " + binding.message);
}

Plaza2TradeEncodedCommand encoded_add(const Plaza2TradeCodec& codec) {
    return codec.encode(Plaza2TradeCommandRequest{add_request()});
}

Plaza2TradeEncodedCommand encoded_recovery(const Plaza2TradeCodec& codec) {
    return codec.encode(Plaza2TradeCommandRequest{recovery_request()});
}

bool contains_text(std::string_view value, std::string_view needle) {
    return value.find(needle) != std::string_view::npos;
}

void expect_case(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

OrderLifecycleConfig make_controller_config(const std::filesystem::path& root) {
    OrderLifecycleConfig config;
    config.run_id = "concrete-transport-controller";
    config.profile_id = "offline-plaza2-test";
    config.profile_fingerprint = std::string(64, 'e');
    config.profile_enabled = true;
    config.environment = cgate::Plaza2Environment::Test;
    config.dry_run = false;
    config.send_test_order = true;
    config.isin_id = 1001;
    config.base_contract_code = "RTS";
    config.instrument_mask = 1;
    config.broker_code = "BRK1";
    config.client_code = "C01";
    config.side = Plaza2TradeSide::Sell;
    config.order_type = Plaza2TradeOrderType::Limit;
    config.price = "103000";
    config.quantity = 1;
    config.ext_id = 79;
    config.add_user_id = 701;
    config.cancel_user_id = 702;
    config.recovery_user_id = 703;
    config.comment = "offline";
    config.smoke.instrument_exists = true;
    config.smoke.tradable_session = true;
    config.smoke.aggr20_two_sided = true;
    config.smoke.limits_snapshot_applicable = true;
    config.smoke.tick_size = "250";
    config.smoke.top_bid = "102500";
    config.smoke.top_ask = "102750";
    config.smoke.market_data_source = "fake.AGGR20";
    config.smoke.aggr20_source_sequence = 1;
    config.smoke.aggr20_source_revision = 1;
    config.smoke.aggr20_observed_at_utc = "2026-08-24T00:00:00Z";
    config.smoke.aggr20_age_ms = 0;
    config.smoke.trading_day = "20260824";
    config.smoke.session_id = "321";
    config.smoke.session_state = "trading";
    config.smoke.refdata_source = "fake.REFDATA";
    config.smoke.refdata_source_sequence = 1;
    config.smoke.refdata_source_revision = 1;
    config.smoke.limits_source = "fake.PART";
    config.smoke.limits_commit_sequence = 1;
    config.policy.version = "offline-v1";
    config.policy.sha256 = std::string(64, 'f');
    config.policy.max_distance_ticks = 4;
    config.policy.max_aggr20_age_ms = 1000;
    config.add_observation_timeout = std::chrono::seconds(2);
    config.cancel_observation_timeout = std::chrono::seconds(2);
    config.max_poll_attempts = 4;
    config.journal_root = root / "journals";
    return config;
}

OrderLifecycleResult run_concrete_controller_case(const moex::plaza2::test::RuntimeFixturePaths& fixture,
                                                  const char* order_mode, const char* reply_order_id,
                                                  bool identity_conflict, bool cancel_after_del = false,
                                                  bool mismatched_policy = false, bool* host_started = nullptr) {
    std::optional<ScopedEnv> mode;
    if (order_mode != nullptr) {
        mode.emplace("MOEX_FAKE_FULL_FILL", std::string_view(order_mode) == "full" ? "1" : nullptr);
    }
    ScopedEnv cancelled("MOEX_FAKE_CANCELLED_ORDER",
                        order_mode != nullptr && std::string_view(order_mode) == "cancel" ? "1" : nullptr);
    ScopedEnv conflict("MOEX_FAKE_IDENTITY_CONFLICT", identity_conflict ? "1" : nullptr);
    ScopedEnv reply_id("MOEX_FAKE_PUB_REPLY_ORDER_ID", reply_order_id);
    ScopedEnv client_code("MOEX_FAKE_CLIENT_CODE", "BRK1C01");
    ScopedEnv cancel_after_del_env("MOEX_FAKE_CANCEL_AFTER_DEL", cancel_after_del ? "1" : nullptr);

    const Plaza2TradeCodec codec;
    auto config = make_controller_config(fixture.root);
    config.run_id = std::string("concrete-") + (order_mode == nullptr ? "working" : order_mode) +
                    (identity_conflict ? "-conflict" : "-consistent") +
                    (reply_order_id == nullptr ? "-default" : reply_order_id);
    const auto dry = [&]() {
        auto copy = config;
        copy.dry_run = true;
        copy.send_test_order = false;
        return build_pre_send_plan(copy);
    }();
    expect_case(dry.ok, std::string("concrete controller static intent must validate: ") + dry.message);
    config.authorized_plan_sha256 = dry.sha256;
    auto transport_config = make_config(fixture);
    transport_config.authorized_intent->add_payload_sha256 = cgate::plaza2_sha256_hex(dry.add_command.payload);
    transport_config.authorized_intent->recovery_payload_sha256 =
        cgate::plaza2_sha256_hex(dry.exact_ext_id_recovery_command.payload);
    if (!mismatched_policy) {
        transport_config.authorized_intent->policy_version = config.policy.version;
        transport_config.authorized_intent->policy_sha256 = config.policy.sha256;
        transport_config.authorized_intent->max_aggr20_age_ms = config.policy.max_aggr20_age_ms;
        transport_config.authorized_intent->require_zero_starting_position =
            config.policy.require_zero_starting_position;
        transport_config.max_aggr20_age =
            std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(config.policy.max_aggr20_age_ms));
    }
    transport_config.authorized_intent->canonical_json =
        canonical_authorized_order_intent_json(*transport_config.authorized_intent);
    transport_config.authorized_intent->sha256 = authorized_order_intent_sha256(*transport_config.authorized_intent);
    if (!mismatched_policy) {
        expect_case(transport_config.authorized_intent->sha256 == dry.sha256 &&
                        transport_config.authorized_intent->canonical_json == dry.canonical_json,
                    "concrete controller must derive the exact canonical authorized intent");
    }
    transport_config.observation_quantity = 1;
    transport_config.observation_client_code = config.client_code;
    transport_config.observation_side = config.side;
    transport_config.target_price = config.price;
    transport_config.target_tick_size = config.smoke.tick_size;
    transport_config.target_side = config.side;
    transport_config.target_max_distance_ticks = config.policy.max_distance_ticks;
    transport_config.execution_safety_receipt_path = fixture.root / "execution_safety.json";
    Plaza2TestTradeTransport transport(std::move(transport_config));
    SystemOrderLifecycleClock clock;
    OrderLifecycleController controller(config, transport, clock);
    auto result = controller.run();
    if (host_started != nullptr) {
        *host_started = transport.host().started();
    }
    static_cast<void>(transport.host().stop());
    std::filesystem::remove_all(config.journal_root);
    return result;
}

void test_target_preflight_refusals(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    const Plaza2TradeCodec codec;
    const auto add = encoded_add(codec);
    const auto recovery = encoded_recovery(codec);
    const std::vector<std::pair<const char*, std::string_view>> cases = {
        {"MOEX_FAKE_AGGR_ONE_SIDED", "two-sided"},     {"MOEX_FAKE_MISSING_INSTRUMENT", "absent"},
        {"MOEX_FAKE_MISSING_SESSION", "session"},      {"MOEX_FAKE_NONTRADABLE_SESSION", "session"},
        {"MOEX_FAKE_SCHEDULED_SESSION", "session"},    {"MOEX_FAKE_SUSPENDED_SESSION", "session"},
        {"MOEX_FAKE_COMPLETED_SESSION", "session"},    {"MOEX_FAKE_MISSING_LIMITS", "limit"},
        {"MOEX_FAKE_WRONG_LIMIT_CLIENT", "limit"},     {"MOEX_FAKE_DISABLE_REPLY_LISTENER", "cg_lsn_new"},
        {"MOEX_FAKE_DISABLE_PUBLISHER", "cg_pub_new"},
    };
    for (const auto& [variable, expected] : cases) {
        ScopedEnv scenario(variable, "1");
        auto config = prepared_config(fixture, add, recovery);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto result = transport.post(add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent && !result.post_invoked &&
                        contains_text(result.validation_error.message, expected),
                    std::string("target preflight must fail closed for ") + variable + ": " +
                        result.validation_error.message);
    }

    {
        auto config = prepared_config(fixture, add, recovery);
        config.max_aggr20_age = std::chrono::milliseconds(-1);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto result = transport.post(add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent &&
                        contains_text(result.validation_error.message, "stale"),
                    "stale target AGGR20 must not authorize AddOrder");
    }
    {
        auto config = prepared_config(fixture, add, recovery);
        config.require_zero_starting_position = true;
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto result = transport.post(add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent &&
                        contains_text(result.validation_error.message, "starting position"),
                    "non-zero starting position must fail closed");
    }
    {
        ScopedEnv missing_position("MOEX_FAKE_MISSING_POSITION", "1");
        auto config = prepared_config(fixture, add, recovery);
        config.require_zero_starting_position = true;
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto result = transport.post(add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent &&
                        contains_text(result.validation_error.message, "starting position"),
                    "missing starting position must not be treated as zero");
    }
    {
        ScopedEnv zero_position("MOEX_FAKE_ZERO_POSITION", "1");
        auto config = prepared_config(fixture, add, recovery);
        config.authorized_intent->require_zero_starting_position = true;
        config.authorized_intent->canonical_json = canonical_authorized_order_intent_json(*config.authorized_intent);
        config.authorized_intent->sha256 = authorized_order_intent_sha256(*config.authorized_intent);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto result = transport.post(add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::Posted && result.post_invoked,
                    "the exact normal-client account_type=2 zero position row must pass the zero gate");
        static_cast<void>(transport.host().stop());
    }
    {
        ScopedEnv wrong_account_type("MOEX_FAKE_WRONG_POSITION_ACCOUNT_TYPE", "1");
        ScopedEnv zero_position("MOEX_FAKE_ZERO_POSITION", "1");
        auto config = prepared_config(fixture, add, recovery);
        config.require_zero_starting_position = true;
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto result = transport.post(add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent &&
                        contains_text(result.validation_error.message, "account type"),
                    "a brokerage-firm account_type=1 row must not satisfy a normal-client zero gate");
    }
    {
        const auto blocker = fixture.root / "receipt-blocker";
        moex::plaza2::test::write_text_file(blocker, "not a directory");
        auto config = prepared_config(fixture, add, recovery);
        config.execution_safety_receipt_path = blocker / "execution_safety.json";
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto result = transport.post(add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent && !result.post_invoked &&
                        contains_text(result.validation_error.message, "execution-safety"),
                    "execution-safety receipt persistence failure must prevent AddOrder");
    }
    {
        auto marketable_request = add_request();
        marketable_request.price = "102500";
        const auto marketable_add = codec.encode(Plaza2TradeCommandRequest{marketable_request});
        auto config = prepared_config(fixture, marketable_add, recovery);
        config.authorized_intent->price = marketable_request.price.value();
        config.target_price = marketable_request.price.value();
        config.authorized_intent->canonical_json = canonical_authorized_order_intent_json(*config.authorized_intent);
        config.authorized_intent->sha256 = authorized_order_intent_sha256(*config.authorized_intent);
        const auto plan = bound_plan(*config.authorized_intent, marketable_add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto result = transport.post(marketable_add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent && !result.post_invoked,
                    "marketable authorized price must be rejected before AddOrder");
    }
    {
        auto distant_request = add_request();
        distant_request.price = "104250";
        const auto distant_add = codec.encode(Plaza2TradeCommandRequest{distant_request});
        auto config = prepared_config(fixture, distant_add, recovery);
        config.authorized_intent->price = distant_request.price.value();
        config.target_price = distant_request.price.value();
        config.authorized_intent->canonical_json = canonical_authorized_order_intent_json(*config.authorized_intent);
        config.authorized_intent->sha256 = authorized_order_intent_sha256(*config.authorized_intent);
        const auto plan = bound_plan(*config.authorized_intent, distant_add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto result = transport.post(distant_add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent && !result.post_invoked,
                    "out-of-distance authorized price must be rejected before AddOrder");
    }
}

void test_authorized_payload_binding(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    const Plaza2TradeCodec codec;
    {
        const auto add = encoded_add(codec);
        const auto recovery = encoded_recovery(codec);
        auto config = prepared_config(fixture, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        const auto result = transport.post(add, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent && !result.post_invoked &&
                        !transport.host().started(),
                    "concrete transport must refuse AddOrder before an exact plan is bound");
    }
    const auto recovery = encoded_recovery(codec);
    const auto expect_not_sent = [&](Plaza2TestTradeTransportConfig config, const Plaza2TradeEncodedCommand& command,
                                     std::string_view label) {
        const auto plan = bound_plan(*config.authorized_intent, command, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        const auto binding = transport.bind_authorized_plan(plan);
        const auto result = transport.post(command, 701);
        expect_case(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent && !result.post_invoked,
                    std::string(label) + (binding ? ": " + binding.message : ""));
    };

    {
        auto request = add_request();
        request.amount = 7;
        const auto command = codec.encode(Plaza2TradeCommandRequest{request});
        auto config = prepared_config(fixture, command, recovery);
        expect_not_sent(std::move(config), command,
                        "authorized quantity-one intent must reject a seven-contract payload");
    }
    {
        auto request = add_request();
        request.dir = Plaza2TradeSide::Buy;
        const auto command = codec.encode(Plaza2TradeCommandRequest{request});
        auto config = prepared_config(fixture, command, recovery);
        expect_not_sent(std::move(config), command, "authorized sell intent must reject a buy payload");
    }
    {
        auto request = add_request();
        request.price = "103250";
        const auto command = codec.encode(Plaza2TradeCommandRequest{request});
        auto config = prepared_config(fixture, command, recovery);
        expect_not_sent(std::move(config), command, "authorized exact price must reject a different payload price");
    }
    {
        auto request = add_request();
        request.isin_id = 2002;
        const auto command = codec.encode(Plaza2TradeCommandRequest{request});
        auto config = prepared_config(fixture, command, recovery);
        expect_not_sent(std::move(config), command, "authorized target isin must reject a different payload isin");
    }
    {
        auto request = add_request();
        request.price = "104250";
        const auto command = codec.encode(Plaza2TradeCommandRequest{request});
        auto config = prepared_config(fixture, command, recovery);
        config.authorized_intent->price = request.price.value();
        config.target_price = request.price.value();
        config.target_max_distance_ticks = 1000;
        config.authorized_intent->canonical_json = canonical_authorized_order_intent_json(*config.authorized_intent);
        config.authorized_intent->sha256 = authorized_order_intent_sha256(*config.authorized_intent);
        expect_not_sent(std::move(config), command, "transport distance override must not weaken authorized policy");
    }
    {
        auto config = prepared_config(fixture, encoded_add(codec), recovery);
        config.authorized_intent->sha256 = std::string(64, 'a');
        const auto command = encoded_add(codec);
        expect_not_sent(std::move(config), command, "arbitrary intent SHA must not authorize an AddOrder");
    }
}

void test_replication_epoch_gates(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    const Plaza2TradeCodec codec;
    const auto add = encoded_add(codec);
    const auto recovery = encoded_recovery(codec);
    const std::array<std::pair<const char*, std::string_view>, 4> cases = {
        std::pair{"MOEX_FAKE_PRIVATE_CLOSE_AFTER_READY", "private close"},
        std::pair{"MOEX_FAKE_AGGR_CLOSE_AFTER_READY", "AGGR20 close"},
        std::pair{"MOEX_FAKE_PRIVATE_LIFENUM_AFTER_READY", "private LifeNum"},
        std::pair{"MOEX_FAKE_AGGR_LIFENUM_AFTER_READY", "AGGR20 LifeNum"},
    };
    for (const auto& [variable, label] : cases) {
        ScopedEnv scenario(variable, "1");
        auto config = prepared_config(fixture, add, recovery);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto first = transport.post(add, 701);
        expect_case(first.certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    std::string(label) + " fixture must initially reach the fake ready state");
        static_cast<void>(transport.poll(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
        const auto after_epoch = transport.post(add, 701);
        expect_case(after_epoch.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent &&
                        !after_epoch.post_invoked,
                    std::string(label) + " must invalidate AddOrder readiness");
        static_cast<void>(transport.host().stop());
        {
            ScopedEnv restored(variable, nullptr);
            const auto reopened = transport.post(add, 701);
            expect_case(reopened.certainty == cgate::Plaza2SubmissionCertainty::Posted && reopened.post_invoked,
                        std::string(label) + " must regain readiness only after a complete fresh snapshot");
            static_cast<void>(transport.host().stop());
        }
    }
}

void test_multi_instrument_and_terminal_controller(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    const Plaza2TradeCodec codec;
    const auto add = encoded_add(codec);
    const auto recovery = encoded_recovery(codec);
    {
        ScopedEnv multi("MOEX_FAKE_AGGR_MULTI_INSTRUMENT", "1");
        auto transport_config = prepared_config(fixture, add, recovery);
        const auto plan = bound_plan(*transport_config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(transport_config));
        bind_test_plan(transport, plan);
        const auto posted = transport.post(add, 701);
        expect_case(posted.certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    "multi-instrument target should remain postable from scoped BBO");
        const auto scoped = transport.host().aggr20_projector().snapshot_for_isin(1001);
        const auto global = transport.host().aggr20_projector().snapshot();
        expect_case(scoped.has_value() && global.top_bid.has_value() && scoped->top_bid.has_value() &&
                        global.top_bid->price_scaled != scoped->top_bid->price_scaled,
                    "global diagnostic BBO must not be reused as the target scoped BBO");
        static_cast<void>(transport.host().stop());
    }
    {
        const auto result = run_concrete_controller_case(fixture, "full", "20003", true);
        expect_case(result.state == OrderLifecycleState::UnresolvedOrphanIncident && !result.market_safe_terminal &&
                        !result.evidence_consistent && !result.ok,
                    "full fill with identity conflict must remain unresolved through concrete transport: " +
                        result.message + " / add=" + result.add_submission.validation_error.message);
    }
    {
        bool host_started = false;
        const auto result = run_concrete_controller_case(fixture, "full", "20003", false, false, true, &host_started);
        expect_case(result.state == OrderLifecycleState::DefinitelyNotSent && result.market_safe_terminal &&
                        result.add_submission.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent &&
                        !result.add_submission.post_invoked && !host_started,
                    "controller/transport policy mismatch must reject AddOrder before starting the TEST host");
    }
    {
        const auto result = run_concrete_controller_case(fixture, "cancel", "99999", false);
        expect_case(result.state == OrderLifecycleState::UnresolvedOrphanIncident && !result.market_safe_terminal &&
                        !result.evidence_consistent && !result.ok,
                    "cancelled replication contradicting AddOrder identity must remain unresolved: " + result.message);
    }
    {
        const auto result = run_concrete_controller_case(fixture, "full", "20003", false);
        expect_case(result.state == OrderLifecycleState::Filled && result.market_safe_terminal && result.ok &&
                        !result.cancel_submission.post_invoked,
                    "consistent full fill must be market-safe through concrete transport");
    }
    {
        const auto result = run_concrete_controller_case(fixture, nullptr, "20003", false, true);
        expect_case(result.state == OrderLifecycleState::Cancelled && result.cancel_submission.post_invoked &&
                        result.market_safe_terminal && result.ok,
                    "working AddOrder must use DelOrder and reach factual cancellation");
    }
    {
        ScopedEnv add_timeout("MOEX_FAKE_PUB_REPLY_TIMEOUT_ADD_ONLY", "1");
        ScopedEnv cancel_after_recovery("MOEX_FAKE_CANCEL_AFTER_RECOVERY", "1");
        const auto result = run_concrete_controller_case(fixture, "working", "20003", false);
        expect_case(result.state == OrderLifecycleState::Cancelled && result.recovery_submission.post_invoked &&
                        result.add_reply.has_value() && result.add_reply->timed_out,
                    "Add timeout with working replication must use exact-ext recovery and reach factual cancellation");
    }
    {
        auto config = prepared_config(fixture, add, recovery);
        config.max_aggr20_age = std::chrono::milliseconds(20);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto posted = transport.post(add, 701);
        expect_case(posted.certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    "cleanup freshness fixture AddOrder should post before ageing the BBO");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        DelOrderRequest cancel;
        cancel.broker_code = "BRK1";
        cancel.order_id = 20003;
        cancel.client_code = "C01";
        cancel.isin_id = 1001;
        const auto del = codec.encode(Plaza2TradeCommandRequest{cancel});
        const auto cleanup = transport.post(del, 702);
        expect_case(cleanup.certainty == cgate::Plaza2SubmissionCertainty::Posted && cleanup.post_invoked,
                    "DelOrder cleanup must remain available after AGGR20 becomes stale: " +
                        cleanup.validation_error.message + " / " + cleanup.post_error.message);
        static_cast<void>(transport.host().stop());
    }
}

void test_reply_bridge_fail_closed(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    const Plaza2TradeCodec codec;
    const auto add = encoded_add(codec);
    const auto recovery = encoded_recovery(codec);
    {
        ScopedEnv reply_first("MOEX_FAKE_REPLY_BEFORE_REPLICATION", "1");
        auto config = prepared_config(fixture, add, recovery);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto posted = transport.post(add, 701);
        expect_case(posted.certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    "reply-before-replication fixture AddOrder should be posted");
        const auto poll = transport.poll(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        expect_case(poll.ok && poll.replies.size() == 1 && poll.replies.front().accepted &&
                        poll.observations.size() == 1,
                    "reply-before-replication must preserve both evidence surfaces");
    }
    {
        ScopedEnv post_timeout("MOEX_FAKE_PUB_POST_RESULT", "timeout");
        auto config = prepared_config(fixture, add, recovery);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto ambiguous = transport.post(add, 701);
        expect_case(ambiguous.certainty == cgate::Plaza2SubmissionCertainty::PossiblySent && ambiguous.post_invoked,
                    "publisher AddOrder timeout must remain PossiblySent");
        ::setenv("MOEX_FAKE_PUB_POST_RESULT", "ok", 1);
        const auto recovery_post = transport.post_exact_ext_id_recovery(recovery, 703);
        expect_case(recovery_post.certainty == cgate::Plaza2SubmissionCertainty::Posted && recovery_post.post_invoked,
                    "publisher AddOrder timeout must leave exact-ext recovery available");
    }
    {
        ScopedEnv reply_timeout("MOEX_FAKE_PUB_REPLY_MODE", "timeout");
        auto config = prepared_config(fixture, add, recovery);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto posted = transport.post(add, 701);
        expect_case(posted.certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    "cancellation-timeout fixture AddOrder should be posted");
        const auto add_poll = transport.poll(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        expect_case(add_poll.ok && add_poll.replies.size() == 1 && add_poll.replies.front().timed_out,
                    "Add reply timeout must remain uncertain before cleanup");
        DelOrderRequest cancel;
        cancel.broker_code = "BRK1";
        cancel.order_id = 20003;
        cancel.client_code = "C01";
        cancel.isin_id = 1001;
        const auto cancel_command = codec.encode(Plaza2TradeCommandRequest{cancel});
        const auto cancel_post = transport.post(cancel_command, 702);
        expect_case(cancel_post.certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    "DelOrder cleanup should still post when its reply may time out");
        const auto cancel_poll = transport.poll(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        expect_case(cancel_poll.ok && cancel_poll.replies.size() == 1 && cancel_poll.replies.front().timed_out,
                    "cancellation timeout must remain unresolved rather than becoming cancellation");
    }
    const std::vector<std::pair<const char*, std::string_view>> cases = {
        {"MOEX_FAKE_PUB_REPLY_FAMILY", "reply message family"},
        {"MOEX_FAKE_PUB_REPLY_MALFORMED", "malformed"},
        {"MOEX_FAKE_PUB_DUPLICATE_REPLY", "contradictory"},
    };
    for (const auto& [variable, expected] : cases) {
        ScopedEnv scenario(variable, variable == std::string_view("MOEX_FAKE_PUB_REPLY_FAMILY") ? "wrong" : "1");
        auto config = prepared_config(fixture, add, recovery);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto posted = transport.post(add, 701);
        expect_case(posted.certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    "reply failure fixture AddOrder should still be posted");
        const auto poll = transport.poll(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        expect_case(!poll.ok && contains_text(poll.error, expected),
                    std::string("reply bridge must fail closed for ") + variable + ": " + poll.error);
    }
    {
        ScopedEnv timeout("MOEX_FAKE_PUB_REPLY_TIMEOUT_ADD_ONLY", "1");
        auto config = prepared_config(fixture, add, recovery);
        const auto plan = bound_plan(*config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(config));
        bind_test_plan(transport, plan);
        const auto posted = transport.post(add, 701);
        expect_case(posted.certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    "timeout fixture AddOrder should be posted");
        const auto poll = transport.poll(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        expect_case(poll.ok && poll.replies.size() == 1 && poll.replies.front().timed_out &&
                        !poll.replies.front().accepted && !poll.replies.front().order_id.has_value(),
                    "P2MQ timeout must remain an operation-correlated uncertainty");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "expected fake runtime library path\n";
            return 1;
        }
        using moex::plaza2::test::build_vendor_like_runtime_scheme;
        using moex::plaza2::test::make_temp_directory;
        using moex::plaza2::test::materialize_runtime_fixture;
        using moex::plaza2::test::remove_tree;
        using moex::plaza2::test::require;

        const auto root = make_temp_directory("plaza2_trade_transport_scenarios");
        const auto fixture =
            materialize_runtime_fixture(root, std::filesystem::path(argv[1]), cgate::Plaza2Environment::Test,
                                        build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test"));
        ::setenv("MOEX_FAKE_CGATE_REQUIRE_ABSOLUTE_SCHEME", "1", 1);
        ::setenv("MOEX_FAKE_CLIENT_CODE", "BRK1C01", 1);
        ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);

        const Plaza2TradeCodec codec;
        const auto add = codec.encode(Plaza2TradeCommandRequest{add_request()});
        require(add.validation.ok() && add.payload.size() == 112,
                "AddOrder fixture must use reviewed payload size (got " + std::to_string(add.payload.size()) + ", " +
                    add.validation.message + ")");
        auto transport_config = make_config(fixture);
        transport_config.authorized_intent->add_payload_sha256 = cgate::plaza2_sha256_hex(add.payload);
        const auto recovery = codec.encode(Plaza2TradeCommandRequest{recovery_request()});
        require(recovery.validation.ok() && recovery.payload.size() == 49,
                "exact-ext recovery fixture must use reviewed payload size");
        transport_config.authorized_intent->recovery_payload_sha256 = cgate::plaza2_sha256_hex(recovery.payload);
        transport_config.authorized_intent->canonical_json =
            canonical_authorized_order_intent_json(*transport_config.authorized_intent);
        transport_config.authorized_intent->sha256 =
            authorized_order_intent_sha256(*transport_config.authorized_intent);
        const auto plan = bound_plan(*transport_config.authorized_intent, add, recovery);
        Plaza2TestTradeTransport transport(std::move(transport_config));
        bind_test_plan(transport, plan);
        const auto posted = transport.post(add, 701);
        require(posted.certainty == cgate::Plaza2SubmissionCertainty::Posted && posted.post_invoked,
                "fake TEST transport must preserve posted certainty: " + posted.validation_error.message + " / " +
                    posted.post_error.message);
        require(std::filesystem::exists(root / "execution_safety.json"),
                "AddOrder must persist execution-safety receipt before posting");
        require(transport.last_execution_safety_receipt().has_value() &&
                    transport.last_execution_safety_receipt()->passive_non_marketable &&
                    transport.last_execution_safety_receipt()->bbo_distance_allowed &&
                    transport.last_execution_safety_receipt()->aggr_online &&
                    transport.last_execution_safety_receipt()->aggr_snapshot_complete &&
                    transport.last_execution_safety_receipt()->authorized_max_aggr20_age_ms == 5000 &&
                    !transport.last_execution_safety_receipt()->require_zero_starting_position,
                "execution-safety receipt must record passive and BBO-distance checks");

        const auto first_poll = transport.poll(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        require(first_poll.ok, "first fake CGate poll should succeed");
        require(first_poll.replies.size() == 1 && first_poll.replies.front().user_id == 701 &&
                    first_poll.replies.front().accepted && first_poll.replies.front().order_id == 20003,
                "reply listener must decode the exact AddOrder user_id and order id");
        require(first_poll.observations.size() == 1 && first_poll.observations.front().ext_id == 79,
                "committed private replication must produce the own-order observation (orders=" +
                    std::to_string(transport.host().private_state().own_orders().size()) +
                    ", callback=" + transport.host().last_callback_error() +
                    ", streams=" + std::to_string(transport.host().private_state().stream_health().size()) +
                    ", commits=" + std::to_string(transport.host().private_state().connector_health().commit_count) +
                    ")");
        const auto scoped_bbo = transport.host().aggr20_projector().snapshot_for_isin(1001);
        require(scoped_bbo.has_value() && scoped_bbo->top_bid.has_value() && scoped_bbo->top_ask.has_value(),
                "AGGR20 target snapshot must be two-sided");
        require(scoped_bbo->committed_at != cgate::Plaza2Aggr20BookProjector::Clock::time_point{},
                "AGGR20 target snapshot must carry local commit time");

        ::setenv("MOEX_FAKE_PUB_POST_RESULT", "timeout", 1);
        const auto ambiguous = transport.post_exact_ext_id_recovery(recovery, 703);
        require(ambiguous.certainty == cgate::Plaza2SubmissionCertainty::PossiblySent && ambiguous.post_invoked,
                "publisher timeout must remain PossiblySent and must not be retried");
        ::unsetenv("MOEX_FAKE_PUB_POST_RESULT");

        const auto second_poll = transport.poll(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        require(second_poll.ok, "timeout recovery poll should remain usable for cleanup");
        require(second_poll.replies.empty(), "a publisher timeout must not fabricate a recovery reply");

        const auto absent = transport.host().aggr20_projector().snapshot_for_isin(999999);
        require(!absent.has_value(), "missing target instrument must remain absent");
        require(transport.host().stop().code == cgate::Plaza2ErrorCode::None, "TEST host must stop cleanly");

        {
            ScopedEnv no_active_order("MOEX_FAKE_MISSING_ORDER", "1");
            auto live_config = prepared_config(fixture, add, recovery);
            live_config.host.mode = Plaza2TestSessionHostMode::LiveTestPreSend;
            live_config.host.endpoint_host = "127.0.0.1";
            live_config.host.arm_state.test_plaza2_armed = true;
            live_config.host.publisher_name = "PRE_SEND_TEST";
            live_config.host.publisher_settings = "p2mq://FORTS_SRV;category=FORTS_MSG;name=PRE_SEND_TEST;timeout=5000";
            live_config.host.p2mqreply_settings = "p2mqreply://;ref=PRE_SEND_TEST";
            Plaza2TestTradeTransport live_transport(std::move(live_config));
            bind_test_plan(live_transport, plan);
            const auto disabled = live_transport.post(add, 701);
            require(disabled.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent &&
                        !disabled.post_invoked &&
                        disabled.validation_error.code == cgate::Plaza2ErrorCode::SendDisabledPreSendPhase &&
                        disabled.validation_error.message == "SEND_DISABLED_PRE_SEND_PHASE",
                    "LiveTestPreSend must stop below cg_pub_msgnew/cg_pub_post with a typed result");
            require(live_transport.host().publisher_open() && live_transport.host().p2mqreply_open() &&
                        live_transport.last_execution_safety_receipt().has_value() &&
                        live_transport.last_execution_safety_receipt()->userorderbook_periodic_snapshot_consistent,
                    "LiveTestPreSend must complete the exact pre-send receipt before the disabled post barrier");
            require(live_transport.host().stop().code == cgate::Plaza2ErrorCode::None,
                    "LiveTestPreSend host must stop cleanly");
        }

        {
            ScopedEnv no_active_order("MOEX_FAKE_MISSING_ORDER", "1");
            ScopedEnv missing_position("MOEX_FAKE_MISSING_POSITION", "1");
            ScopedEnv flat_trade_replay("MOEX_FAKE_FLAT_TRADE_REPLAY", "1");
            auto flat_config = prepared_config(fixture, add, recovery);
            flat_config.host.mode = Plaza2TestSessionHostMode::LiveTestPreSend;
            flat_config.host.endpoint_host = "127.0.0.1";
            flat_config.host.arm_state.test_plaza2_armed = true;
            flat_config.host.publisher_name = "FLAT_REPLAY_TEST";
            flat_config.host.publisher_settings =
                "p2mq://FORTS_SRV;category=FORTS_MSG;name=FLAT_REPLAY_TEST;timeout=5000";
            flat_config.host.p2mqreply_settings = "p2mqreply://;ref=FLAT_REPLAY_TEST";
            flat_config.host.trade_replay_from_pos_anchor = true;
            flat_config.authorized_intent->require_zero_starting_position = true;
            flat_config.authorized_intent->canonical_json =
                canonical_authorized_order_intent_json(*flat_config.authorized_intent);
            flat_config.authorized_intent->sha256 = authorized_order_intent_sha256(*flat_config.authorized_intent);
            const auto flat_plan = bound_plan(*flat_config.authorized_intent, add, recovery);
            Plaza2TestTradeTransport flat_transport(std::move(flat_config));
            bind_test_plan(flat_transport, flat_plan);
            const auto disabled = flat_transport.post(add, 701);
            require(disabled.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent &&
                        disabled.validation_error.code == cgate::Plaza2ErrorCode::SendDisabledPreSendPhase,
                    "POS plus anchored empty TRADE replay must reach the disabled pre-send barrier");
            const auto& receipt = flat_transport.last_execution_safety_receipt();
            require(receipt.has_value() &&
                        receipt->position_evidence_class == PositionEvidenceClass::FlatByPosSnapshotAndTradeReplay &&
                        receipt->zero_starting_position_proven && receipt->position_snapshot_complete &&
                        receipt->trade_replay_complete && receipt->position_trades_rev == 44 &&
                        receipt->position_trades_lifenum == 7 && receipt->position_server_time == 1700000001 &&
                        receipt->participant_user_deal_count == 0 &&
                        receipt->participant_user_multileg_deal_count == 0 && receipt->reconstructed_target_xpos == 0 &&
                        receipt->active_own_order_count == 0,
                    "flatness receipt must bind POS anchor, empty participant replay, reconstructed xpos, and order "
                    "census");
            require(flat_transport.host().stop().code == cgate::Plaza2ErrorCode::None,
                    "flatness pre-send host must stop cleanly");
        }

        test_target_preflight_refusals(fixture);
        test_authorized_payload_binding(fixture);
        test_replication_epoch_gates(fixture);
        test_reply_bridge_fail_closed(fixture);
        test_multi_instrument_and_terminal_controller(fixture);

        ::unsetenv("MOEX_FAKE_CGATE_REQUIRE_ABSOLUTE_SCHEME");
        ::unsetenv("MOEX_FAKE_PUB_REPLY_ORDER_ID");
        remove_tree(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
