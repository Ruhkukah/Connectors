#include "moex/plaza2_trade/plaza2_order_lifecycle.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace cgate = moex::plaza2::cgate;
namespace fake = moex::plaza2::fake;
namespace generated = moex::plaza2::generated;
namespace private_state = moex::plaza2::private_state;
using namespace moex::plaza2_trade;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path make_temp_root(std::string_view scenario) {
    static std::uint64_t sequence = 0;
    auto path = std::filesystem::temp_directory_path() /
                ("moex_order_lifecycle_" + std::string(scenario) + "_" + std::to_string(++sequence));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

class FakeClock final : public OrderLifecycleClock {
  public:
    std::chrono::steady_clock::time_point now() const noexcept override {
        return now_;
    }

    void advance(std::chrono::milliseconds duration) {
        now_ += duration;
    }

  private:
    std::chrono::steady_clock::time_point now_{};
};

class ScriptTransport final : public OrderLifecycleTransport {
  public:
    explicit ScriptTransport(FakeClock& clock) : clock_(clock) {}

    cgate::Plaza2PublisherMessageResult post(const Plaza2TradeEncodedCommand& command, std::uint32_t user_id) override {
        commands.push_back(command);
        user_ids.push_back(user_id);
        if (post_results.empty()) {
            return {};
        }
        auto result = post_results.front();
        post_results.erase(post_results.begin());
        return result;
    }

    OrderLifecyclePollResult poll(std::chrono::steady_clock::time_point) override {
        clock_.advance(std::chrono::seconds(1));
        if (poll_results.empty()) {
            clock_.advance(std::chrono::minutes(5));
            return {.deadline_reached = true};
        }
        auto result = poll_results.front();
        poll_results.erase(poll_results.begin());
        return result;
    }

    OrderLifecyclePollResult reconcile() override {
        if (reconciliation_results.empty()) {
            return {};
        }
        auto result = reconciliation_results.front();
        reconciliation_results.erase(reconciliation_results.begin());
        return result;
    }

    FakeClock& clock_;
    std::vector<cgate::Plaza2PublisherMessageResult> post_results;
    std::vector<OrderLifecyclePollResult> poll_results;
    std::vector<OrderLifecyclePollResult> reconciliation_results;
    std::vector<Plaza2TradeEncodedCommand> commands;
    std::vector<std::uint32_t> user_ids;
};

cgate::Plaza2PublisherMessageResult posted() {
    return {
        .certainty = cgate::Plaza2SubmissionCertainty::Posted,
        .post_invoked = true,
    };
}

cgate::Plaza2PublisherMessageResult possibly_sent() {
    return {
        .certainty = cgate::Plaza2SubmissionCertainty::PossiblySent,
        .post_error =
            {
                .code = cgate::Plaza2ErrorCode::RuntimeCallFailed,
                .runtime_code = 131075,
                .message = "cg_pub_post: CG_ERR_TIMEOUT",
            },
        .post_invoked = true,
    };
}

OrderLifecycleConfig base_config(const std::filesystem::path& root, std::string run_id) {
    OrderLifecycleConfig config;
    config.run_id = std::move(run_id);
    config.profile_id = "test-plaza2-order-lifecycle";
    config.profile_fingerprint = std::string(64, 'a');
    config.profile_enabled = true;
    config.environment = cgate::Plaza2Environment::Test;
    config.isin_id = 1001;
    config.broker_code = "ABCD";
    config.client_code = "C01";
    config.side = Plaza2TradeSide::Buy;
    config.order_type = Plaza2TradeOrderType::Limit;
    config.price = "100.00";
    config.quantity = 1;
    config.ext_id = 7001;
    config.add_user_id = 8001;
    config.cancel_user_id = 8002;
    config.comment = "offline-test";
    config.smoke.instrument_exists = true;
    config.smoke.tradable_session = true;
    config.smoke.aggr20_two_sided = true;
    config.smoke.limits_snapshot_applicable = true;
    config.smoke.tick_size = "0.25";
    config.smoke.top_bid = "100.00";
    config.smoke.top_ask = "100.25";
    config.smoke.aggr20_age_ms = 25;
    config.smoke.max_aggr20_age_ms = 1000;
    config.limits.max_notional = "1000.00";
    config.limits.max_distance_ticks = 4;
    config.limits.max_quantity = 1;
    config.add_observation_timeout = std::chrono::seconds(3);
    config.cancel_observation_timeout = std::chrono::seconds(3);
    config.max_poll_attempts = 8;
    config.journal_root = root;
    return config;
}

void authorize_live(OrderLifecycleConfig& config) {
    config.dry_run = true;
    config.send_test_order = false;
    const auto dry_plan = build_pre_send_plan(config);
    require(dry_plan.ok, "base dry-run plan should validate");
    config.dry_run = false;
    config.send_test_order = true;
    config.authorized_plan_sha256 = dry_plan.sha256;
}

OrderReplyObservation accepted_add(std::int64_t order_id = 9001) {
    return {
        .user_id = 8001,
        .command_kind = Plaza2TradeCommandKind::AddOrder,
        .accepted = true,
        .order_id = order_id,
    };
}

OrderReplyObservation cancel_reply(bool accepted) {
    return {
        .user_id = 8002,
        .command_kind = Plaza2TradeCommandKind::DelOrder,
        .accepted = accepted,
        .code = accepted ? 0 : 14,
    };
}

OrderObservation observation(OrderLifecycleState state, std::int64_t executed = 0, std::int64_t original = 1,
                             std::int64_t remaining = 1) {
    return {
        .public_order_id = 9001,
        .private_order_id = 9001,
        .public_order_id_aliases = {9001},
        .private_order_id_aliases = {9001},
        .ext_id = 7001,
        .client_code = "C01",
        .original_quantity = original,
        .remaining_quantity = remaining,
        .executed_quantity = executed,
        .state = state,
        .from_trade_replication = true,
    };
}

OrderLifecycleResult run_script(OrderLifecycleConfig config, ScriptTransport& transport, FakeClock& clock) {
    OrderLifecycleController controller(std::move(config), transport, clock);
    return controller.run();
}

void test_dry_run_with_no_arms() {
    const auto root = make_temp_root("dry_run");
    auto config = base_config(root, "dry-run");
    FakeClock clock;
    ScriptTransport transport(clock);
    const auto result = run_script(config, transport, clock);
    require(result.ok && result.state == OrderLifecycleState::DefinitelyNotSent,
            "dry-run should finish without a submission");
    require(transport.commands.empty(), "dry-run must not invoke transport");
    require(result.journal_path.filename() == "pre_send_plan.json", "dry-run must emit canonical plan filename");
    const auto text = read_text(result.journal_path);
    require(text.find("payload_sha256") != std::string::npos, "plan should contain a payload hash");
    require(text.find(config.client_code) == std::string::npos, "plan must not write client account data");
    std::filesystem::remove_all(root);
}

void test_validation_refusals() {
    const auto root = make_temp_root("validation");
    auto disabled = base_config(root, "disabled");
    disabled.profile_enabled = false;
    require(build_pre_send_plan(disabled).failure == PreSendFailure::DisabledProfile,
            "disabled profile should fail natively");

    auto prod = base_config(root, "prod");
    prod.environment = cgate::Plaza2Environment::Prod;
    require(build_pre_send_plan(prod).failure == PreSendFailure::NonTestProfile,
            "non-TEST profile should fail natively");

    auto conflict = base_config(root, "conflict");
    conflict.send_test_order = true;
    require(build_pre_send_plan(conflict).failure == PreSendFailure::ConflictingMode,
            "conflicting dry-run/send modes should fail");

    auto quantity = base_config(root, "quantity-two");
    quantity.quantity = 2;
    quantity.limits.max_quantity = 2;
    require(build_pre_send_plan(quantity).failure == PreSendFailure::InvalidQuantity,
            "quantity two must fail even when max_quantity is two");

    auto armed_dry = base_config(root, "armed-dry");
    armed_dry.any_arm_flag = true;
    require(build_pre_send_plan(armed_dry).failure == PreSendFailure::DryRunArmed, "dry-run should reject arm flags");
    std::filesystem::remove_all(root);
}

void test_pre_post_validation_failure() {
    const auto root = make_temp_root("pre_post");
    auto config = base_config(root, "pre-post");
    config.price = "100.10";
    FakeClock clock;
    ScriptTransport transport(clock);
    const auto result = run_script(config, transport, clock);
    require(result.state == OrderLifecycleState::DefinitelyNotSent, "invalid tick should be definitely not sent");
    require(transport.commands.empty(), "pre-post validation failure must not invoke transport");
    std::filesystem::remove_all(root);
}

void test_message_allocation_failure() {
    const auto root = make_temp_root("allocation");
    auto config = base_config(root, "allocation");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    cgate::Plaza2PublisherMessageResult allocation_failure;
    allocation_failure.allocation_error = {
        .code = cgate::Plaza2ErrorCode::RuntimeCallFailed,
        .message = "cg_pub_msgnew failed",
    };
    transport.post_results = {allocation_failure};
    const auto result = run_script(config, transport, clock);
    require(result.safe_terminal && result.state == OrderLifecycleState::DefinitelyNotSent,
            "allocation failure should be definitely not sent");
    require(transport.commands.size() == 1, "allocation failure should not retry AddOrder");
    std::filesystem::remove_all(root);
}

void test_post_success_plus_free_failure() {
    const auto root = make_temp_root("free_failure");
    auto config = base_config(root, "free-failure");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    auto add = posted();
    add.free_error = {
        .code = cgate::Plaza2ErrorCode::RuntimeCallFailed,
        .message = "cg_pub_msgfree failed",
    };
    transport.post_results = {add, posted()};
    transport.poll_results = {
        {.replies = {accepted_add()}, .observations = {observation(OrderLifecycleState::Working)}},
        {.replies = {cancel_reply(true)}, .observations = {observation(OrderLifecycleState::Cancelled, 0, 1, 0)}},
    };
    const auto result = run_script(config, transport, clock);
    require(result.state == OrderLifecycleState::Cancelled,
            "message-free failure must not erase successful AddOrder post");
    require(result.add_submission.certainty == cgate::Plaza2SubmissionCertainty::Posted,
            "successful post certainty must remain posted");
    require(transport.commands.size() == 2, "working order should receive one DelOrder");
    std::int64_t del_order_id = 0;
    std::memcpy(&del_order_id, transport.commands[1].payload.data() + 4, sizeof(del_order_id));
    require(del_order_id == 9001, "DelOrder must use the accepted AddOrder reply ID");
    std::filesystem::remove_all(root);
}

void test_post_timeout_ambiguous_submission() {
    const auto root = make_temp_root("post_timeout");
    auto config = base_config(root, "post-timeout");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    transport.post_results = {possibly_sent()};
    transport.poll_results = {{.deadline_reached = true}};
    transport.reconciliation_results = {{}};
    const auto result = run_script(config, transport, clock);
    require(result.state == OrderLifecycleState::UnresolvedOrphanIncident && result.orphan_incident_written,
            "ambiguous AddOrder must produce an orphan incident when unresolved");
    require(transport.commands.size() == 1, "ambiguous AddOrder must never be retried");
    std::filesystem::remove_all(root);
}

void test_immediate_full_fill() {
    const auto root = make_temp_root("full_fill");
    auto config = base_config(root, "full-fill");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    transport.post_results = {posted()};
    transport.poll_results = {
        {.replies = {accepted_add()}, .observations = {observation(OrderLifecycleState::Filled, 1, 1, 0)}},
    };
    const auto result = run_script(config, transport, clock);
    require(result.ok && result.state == OrderLifecycleState::Filled, "immediate full fill should be terminal filled");
    require(transport.commands.size() == 1, "filled order must not be cancelled");
    std::filesystem::remove_all(root);
}

void test_partial_fill_then_remainder_cancellation() {
    const auto root = make_temp_root("partial_cancel");
    auto config = base_config(root, "partial-cancel");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    transport.post_results = {posted(), posted()};
    auto partial = observation(OrderLifecycleState::PartiallyFilled, 1, 2, 1);
    partial.from_own_trades = true;
    auto cancelled = observation(OrderLifecycleState::Cancelled, 1, 2, 0);
    cancelled.from_own_trades = true;
    transport.poll_results = {
        {.replies = {accepted_add()}, .observations = {partial}},
        {.replies = {cancel_reply(true)}, .observations = {cancelled}},
    };
    const auto result = run_script(config, transport, clock);
    require(result.state == OrderLifecycleState::Cancelled && result.observation->executed_quantity == 1,
            "partial fill followed by remainder cancellation must retain executed quantity");
    std::filesystem::remove_all(root);
}

void test_replication_timeout_then_reconciliation() {
    const auto root = make_temp_root("reconcile");
    auto config = base_config(root, "reconcile");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    transport.post_results = {posted(), posted()};
    transport.poll_results = {
        {.deadline_reached = true},
        {.replies = {cancel_reply(true)}, .observations = {observation(OrderLifecycleState::Cancelled, 0, 1, 0)}},
    };
    transport.reconciliation_results = {
        {.replies = {accepted_add()}, .observations = {observation(OrderLifecycleState::Working)}},
    };
    const auto result = run_script(config, transport, clock);
    require(result.state == OrderLifecycleState::Cancelled,
            "replication timeout should reconcile before deciding the order is unresolved");
    std::filesystem::remove_all(root);
}

void test_cancel_rejection() {
    const auto root = make_temp_root("cancel_reject");
    auto config = base_config(root, "cancel-reject");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    transport.post_results = {posted(), posted()};
    transport.poll_results = {
        {.replies = {accepted_add()}, .observations = {observation(OrderLifecycleState::Working)}},
        {.replies = {cancel_reply(false)}, .observations = {observation(OrderLifecycleState::Working)}},
    };
    transport.reconciliation_results = {{.observations = {observation(OrderLifecycleState::Working)}}};
    const auto result = run_script(config, transport, clock);
    require(result.state == OrderLifecycleState::UnresolvedOrphanIncident,
            "cancel rejection with a working order must remain an incident");
    require(result.cancel_reply.has_value() && !result.cancel_reply->accepted,
            "cancel rejection should be correlated by cancel user_id");
    std::filesystem::remove_all(root);
}

void test_cancel_timeout() {
    const auto root = make_temp_root("cancel_timeout");
    auto config = base_config(root, "cancel-timeout");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    transport.post_results = {posted(), possibly_sent()};
    transport.poll_results = {
        {.replies = {accepted_add()}, .observations = {observation(OrderLifecycleState::Working)}},
        {.deadline_reached = true},
    };
    transport.reconciliation_results = {{.observations = {observation(OrderLifecycleState::Working)}}};
    const auto result = run_script(config, transport, clock);
    require(result.state == OrderLifecycleState::UnresolvedOrphanIncident,
            "cancel timeout must not be labelled cancelled without factual evidence");
    std::filesystem::remove_all(root);
}

void test_polling_failure_after_possible_submission() {
    const auto root = make_temp_root("poll_failure");
    auto config = base_config(root, "poll-failure");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    transport.post_results = {possibly_sent()};
    transport.poll_results = {{.ok = false, .error = "offline injected poll failure"}};
    transport.reconciliation_results = {{}};
    const auto result = run_script(config, transport, clock);
    require(result.state == OrderLifecycleState::UnresolvedOrphanIncident && result.orphan_incident_written,
            "poll failure after possible submission must persist an incident");
    std::filesystem::remove_all(root);
}

void test_source_provenance_scenarios() {
    const auto run_case = [](std::string_view name, bool trade_source, bool user_source) {
        const auto root = make_temp_root(name);
        auto config = base_config(root, std::string(name));
        authorize_live(config);
        FakeClock clock;
        ScriptTransport transport(clock);
        auto filled = observation(OrderLifecycleState::Filled, 1, 1, 0);
        filled.from_trade_replication = trade_source;
        filled.from_user_orderbook = user_source;
        filled.from_own_trades = trade_source;
        transport.post_results = {posted()};
        transport.poll_results = {{.replies = {accepted_add()}, .observations = {filled}}};
        const auto result = run_script(config, transport, clock);
        require(result.state == OrderLifecycleState::Filled, "provenance scenario should finish filled");
        require(result.observation->from_trade_replication == trade_source,
                "trade replication provenance must be factual");
        require(result.observation->from_user_orderbook == user_source, "user-orderbook provenance must be factual");
        std::filesystem::remove_all(root);
    };
    run_case("trade-only", true, false);
    run_case("userbook-only", false, true);
    run_case("converged", true, true);
}

void test_duplicate_ext_id_refusal_and_orphan_journal() {
    const auto root = make_temp_root("duplicate");
    auto first = base_config(root, "unfinished-one");
    authorize_live(first);
    FakeClock first_clock;
    ScriptTransport first_transport(first_clock);
    first_transport.post_results = {possibly_sent()};
    first_transport.poll_results = {{.deadline_reached = true}};
    const auto first_result = run_script(first, first_transport, first_clock);
    require(first_result.orphan_incident_written, "first run should leave an orphan incident");
    const auto journal_text = read_text(first_result.journal_path);
    require(journal_text.find("\"orphan_incident\": true") != std::string::npos, "orphan journal should be durable");
    require(journal_text.find("\"finished\": false") != std::string::npos,
            "unresolved run should retain unfinished identifier locks");
    require(journal_text.find(first.client_code) == std::string::npos, "journal must not contain client account data");

    auto second = base_config(root, "unfinished-two");
    const auto second_plan = build_pre_send_plan(second);
    require(second_plan.failure == PreSendFailure::DuplicateIdentifier,
            "unfinished run must refuse duplicate ext/user IDs");
    std::filesystem::remove_all(root);
}

void test_public_private_id_mismatch() {
    const auto root = make_temp_root("id_mismatch");
    auto config = base_config(root, "id-mismatch");
    authorize_live(config);
    FakeClock clock;
    ScriptTransport transport(clock);
    auto mismatch = observation(OrderLifecycleState::Working);
    mismatch.private_order_id = 9002;
    mismatch.private_order_id_aliases = {9002};
    mismatch.identity_conflict = true;
    transport.post_results = {posted()};
    transport.poll_results = {{.replies = {accepted_add()}, .observations = {mismatch}}};
    const auto result = run_script(config, transport, clock);
    require(result.state == OrderLifecycleState::UnresolvedOrphanIncident,
            "public/private ID mismatch must fail closed");
    require(transport.commands.size() == 1, "ID mismatch must not trigger a guessed DelOrder");
    std::filesystem::remove_all(root);
}

void test_observation_fill_is_never_cancelled() {
    private_state::OwnOrderSnapshot order;
    order.public_order_id = 9001;
    order.private_order_id = 9001;
    order.public_order_id_aliases = {9001};
    order.private_order_id_aliases = {9001};
    order.ext_id = 7001;
    order.client_code = "C01";
    order.public_amount = 1;
    order.private_amount = 1;
    order.public_amount_rest = 0;
    order.private_amount_rest = 0;
    order.public_action = 0;
    order.private_action = 0;
    order.from_trade_repl = true;

    private_state::OwnTradeSnapshot trade;
    trade.id_deal = 77;
    trade.amount = 1;
    trade.public_order_id_buy = 9001;
    trade.private_order_id_buy = 9001;
    trade.ext_id_buy = 7001;
    const std::vector orders{order};
    const std::vector trades{trade};
    const auto observed = observe_order(7001, "C01", Plaza2TradeSide::Buy, 1, orders, trades);
    require(observed.has_value() && observed->state == OrderLifecycleState::Filled,
            "matched full fill must override a cancellation-shaped terminal row");
    require(observed->matched_own_trades.size() == 1 && observed->executed_quantity == 1,
            "observation should retain matched own-trade evidence");
}

const private_state::StreamHealthSnapshot* find_stream(std::span<const private_state::StreamHealthSnapshot> streams,
                                                       generated::StreamCode code) {
    for (const auto& stream : streams) {
        if (stream.stream_code == code) {
            return &stream;
        }
    }
    return nullptr;
}

void test_private_projection_independent_stream_commits() {
    private_state::Plaza2PrivateStateProjector projector;
    fake::ScenarioSpec scenario;
    fake::EngineState state;

    const auto project = [&](generated::StreamCode stream, generated::TableCode table,
                             std::span<const fake::FieldValueSpec> fields, std::uint64_t commit_count) {
        fake::EventSpec begin{.kind = fake::EventKind::kTransactionBegin, .stream_code = stream};
        projector.on_event(scenario, begin, state);
        fake::EventSpec row_event{
            .kind = fake::EventKind::kStreamData,
            .stream_code = stream,
            .table_code = table,
        };
        fake::RowSpec row{.stream_code = stream, .table_code = table};
        projector.on_stream_row(scenario, row_event, row, fields, state);
        state.commit_count = commit_count;
        fake::EventSpec commit{.kind = fake::EventKind::kTransactionCommit, .stream_code = stream};
        projector.on_transaction_commit(scenario, commit, state);
    };

    const std::vector trade_fields = {
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsTradeReplOrdersLogPublicOrderId,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 9001},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsTradeReplOrdersLogPrivateOrderId,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 9001},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsTradeReplOrdersLogExtId,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 7001},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsTradeReplOrdersLogClientCode,
                             .kind = fake::ValueKind::kString,
                             .text_value = "C01"},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsTradeReplOrdersLogPublicAction,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 1},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsTradeReplOrdersLogPrivateAction,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 1},
    };
    project(generated::StreamCode::kFortsTradeRepl, generated::TableCode::kFortsTradeReplOrdersLog, trade_fields, 1);
    const auto* trade_health = find_stream(projector.stream_health(), generated::StreamCode::kFortsTradeRepl);
    const auto* user_health = find_stream(projector.stream_health(), generated::StreamCode::kFortsUserorderbookRepl);
    require(trade_health != nullptr && trade_health->last_commit_sequence == 1,
            "trade row should advance only trade stream commit sequence");
    require(user_health == nullptr || user_health->last_commit_sequence == 0,
            "trade row must not advance user-orderbook commit sequence");

    const std::vector user_fields = {
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsUserorderbookReplOrdersPublicOrderId,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 9001},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsUserorderbookReplOrdersPrivateOrderId,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 9001},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsUserorderbookReplOrdersExtId,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 7001},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsUserorderbookReplOrdersClientCode,
                             .kind = fake::ValueKind::kString,
                             .text_value = "C01"},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsUserorderbookReplOrdersPublicAction,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 1},
        fake::FieldValueSpec{.field_code = generated::FieldCode::kFortsUserorderbookReplOrdersPrivateAction,
                             .kind = fake::ValueKind::kSignedInteger,
                             .signed_value = 1},
    };
    project(generated::StreamCode::kFortsUserorderbookRepl, generated::TableCode::kFortsUserorderbookReplOrders,
            user_fields, 2);
    trade_health = find_stream(projector.stream_health(), generated::StreamCode::kFortsTradeRepl);
    user_health = find_stream(projector.stream_health(), generated::StreamCode::kFortsUserorderbookRepl);
    require(trade_health != nullptr && trade_health->last_commit_sequence == 1,
            "user row must not change trade stream commit sequence");
    require(user_health != nullptr && user_health->last_commit_sequence == 2,
            "user row should advance user-orderbook commit sequence");
    require(projector.own_orders().size() == 1 && projector.own_orders()[0].from_trade_repl &&
                projector.own_orders()[0].from_user_book,
            "matching source rows should converge into one canonical order with both provenances");
}

void test_timeout_translation_and_sha256() {
    const auto process = cgate::translate_plaza2_result("cg_conn_process", 131075);
    const auto post = cgate::translate_plaza2_result("cg_pub_post", 131075);
    require(!process, "CG_ERR_TIMEOUT should be benign for cg_conn_process");
    require(post && post.code == cgate::Plaza2ErrorCode::RuntimeCallFailed,
            "CG_ERR_TIMEOUT must not be generic publisher success");
    require(cgate::plaza2_sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "canonical plan hash must use correct SHA-256");
}

} // namespace

int main() {
    try {
        test_dry_run_with_no_arms();
        test_validation_refusals();
        test_pre_post_validation_failure();
        test_message_allocation_failure();
        test_post_success_plus_free_failure();
        test_post_timeout_ambiguous_submission();
        test_immediate_full_fill();
        test_partial_fill_then_remainder_cancellation();
        test_replication_timeout_then_reconciliation();
        test_cancel_rejection();
        test_cancel_timeout();
        test_polling_failure_after_possible_submission();
        test_source_provenance_scenarios();
        test_duplicate_ext_id_refusal_and_orphan_journal();
        test_public_private_id_mismatch();
        test_observation_fill_is_never_cancelled();
        test_private_projection_independent_stream_commits();
        test_timeout_translation_and_sha256();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
