#include "moex/plaza2_trade/plaza2_trade_test_order_runner.hpp"

#include <algorithm>
#include <utility>

namespace moex::plaza2_trade {

namespace {

Plaza2TradeOrderEntryResult fail(Plaza2TradeOrderEntryFailure failure, std::string message) {
    Plaza2TradeOrderEntryResult result;
    result.ok = false;
    result.failure = failure;
    result.message = std::move(message);
    result.evidence.failure_classification = std::string(plaza2_trade_order_entry_failure_name(failure));
    result.evidence.diagnostic = result.message;
    return result;
}

Plaza2TradeOrderEntryResult ok(std::string message) {
    Plaza2TradeOrderEntryResult result;
    result.ok = true;
    result.failure = Plaza2TradeOrderEntryFailure::None;
    result.message = std::move(message);
    result.evidence.failure_classification = "none";
    return result;
}

bool terminal_order_status(const ::moex::plaza2::private_state::OwnOrderSnapshot& order) {
    return order.public_amount_rest == 0 || order.private_amount_rest == 0 || order.public_action == 2 ||
           order.private_action == 2;
}

} // namespace

std::string_view plaza2_trade_order_entry_failure_name(Plaza2TradeOrderEntryFailure failure) noexcept {
    switch (failure) {
    case Plaza2TradeOrderEntryFailure::None:
        return "none";
    case Plaza2TradeOrderEntryFailure::MissingArmFlag:
        return "missing_arm_flag";
    case Plaza2TradeOrderEntryFailure::ProductionProfileRejected:
        return "production_profile_rejected";
    case Plaza2TradeOrderEntryFailure::InvalidOrderProfile:
        return "invalid_order_profile";
    case Plaza2TradeOrderEntryFailure::RuntimeProbeFailed:
        return "runtime_probe_failed";
    case Plaza2TradeOrderEntryFailure::SchemeDriftIncompatible:
        return "scheme_drift_incompatible";
    case Plaza2TradeOrderEntryFailure::PrivateStateNotReady:
        return "private_state_not_ready";
    case Plaza2TradeOrderEntryFailure::Aggr20NotReady:
        return "aggr20_not_ready";
    case Plaza2TradeOrderEntryFailure::CommandValidationFailed:
        return "command_validation_failed";
    case Plaza2TradeOrderEntryFailure::PublisherOpenFailed:
        return "publisher_open_failed";
    case Plaza2TradeOrderEntryFailure::CommandSendFailed:
        return "command_send_failed";
    case Plaza2TradeOrderEntryFailure::ReplyRejected:
        return "reply_rejected";
    case Plaza2TradeOrderEntryFailure::ReplyTimeout:
        return "reply_timeout";
    case Plaza2TradeOrderEntryFailure::ReplicationConfirmationTimeout:
        return "replication_confirmation_timeout";
    case Plaza2TradeOrderEntryFailure::CancelSendFailed:
        return "cancel_send_failed";
    case Plaza2TradeOrderEntryFailure::CancelReplyRejected:
        return "cancel_reply_rejected";
    case Plaza2TradeOrderEntryFailure::CancelConfirmationTimeout:
        return "cancel_confirmation_timeout";
    case Plaza2TradeOrderEntryFailure::Unknown:
        return "unknown";
    }
    return "unknown";
}

Plaza2TradeOrderEntryResult validate_plaza2_trade_order_entry_config(const Plaza2TradeOrderEntryConfig& config) {
    if (config.profile_id.empty()) {
        return fail(Plaza2TradeOrderEntryFailure::InvalidOrderProfile,
                    "profile_id must be set for PLAZA II TEST order entry");
    }
    if (config.production_profile || config.private_session.runtime.environment != cgate::Plaza2Environment::Test) {
        return fail(Plaza2TradeOrderEntryFailure::ProductionProfileRejected,
                    "Phase 5E PLAZA II order entry is TEST-only");
    }
    if (!config.arm_state.test_network_armed || !config.arm_state.test_session_armed ||
        !config.arm_state.test_plaza2_armed || !config.order_entry_armed || !config.tiny_order_armed) {
        return fail(Plaza2TradeOrderEntryFailure::MissingArmFlag,
                    "TEST order entry requires all network/session/plaza2/order-entry/tiny-order arms");
    }
    if (config.mode == Plaza2TradeOrderEntryMode::SendTestOrder && !config.send_test_order) {
        return fail(Plaza2TradeOrderEntryFailure::MissingArmFlag,
                    "live TEST order submission additionally requires --send-test-order");
    }
    if (config.tiny_order.quantity <= 0 || config.tiny_order.quantity > config.tiny_order.max_quantity ||
        config.tiny_order.max_quantity <= 0) {
        return fail(Plaza2TradeOrderEntryFailure::InvalidOrderProfile,
                    "tiny TEST order quantity must be positive and no larger than max_quantity");
    }
    if (config.tiny_order.isin_id <= 0 || config.tiny_order.broker_code.empty() ||
        config.tiny_order.client_code.empty() || config.tiny_order.price.empty() || config.tiny_order.ext_id <= 0 ||
        config.tiny_order.client_transaction_id_prefix.empty()) {
        return fail(Plaza2TradeOrderEntryFailure::InvalidOrderProfile,
                    "tiny TEST order must provide isin_id, broker_code, client_code, price, ext_id, and id prefix");
    }
    if (config.publisher.settings.empty() && config.mode == Plaza2TradeOrderEntryMode::SendTestOrder) {
        return fail(Plaza2TradeOrderEntryFailure::InvalidOrderProfile,
                    "live TEST order submission requires publisher settings");
    }
    return ok("PLAZA II TEST order-entry config validated");
}

AddOrderRequest make_plaza2_trade_add_order_request(const Plaza2TradeTinyOrderConfig& order) {
    AddOrderRequest request;
    request.broker_code = order.broker_code;
    request.isin_id = order.isin_id;
    request.client_code = order.client_code;
    request.dir = order.side;
    request.type = order.order_type;
    request.amount = order.quantity;
    request.price = order.price;
    request.comment = order.comment.empty() ? order.client_transaction_id_prefix : order.comment;
    request.ext_id = order.ext_id;
    request.is_check_limit = 1;
    return request;
}

DelOrderRequest make_plaza2_trade_del_order_request(const Plaza2TradeTinyOrderConfig& order, std::int64_t order_id) {
    DelOrderRequest request;
    request.broker_code = order.broker_code;
    request.order_id = order_id;
    request.client_code = order.client_code;
    request.isin_id = order.isin_id;
    return request;
}

Plaza2TradeLiveOrderEntryGateway::Plaza2TradeLiveOrderEntryGateway(Plaza2TradeOrderEntryConfig config)
    : config_(std::move(config)), private_runner_(config_.private_session) {}

Plaza2TradeLiveOrderEntryGateway::~Plaza2TradeLiveOrderEntryGateway() {
    stop();
}

Plaza2TradeOrderEntryResult Plaza2TradeLiveOrderEntryGateway::start_private_state() {
    const auto result = private_runner_.start();
    if (!result.ok) {
        const auto& health = private_runner_.health_snapshot();
        if (!health.runtime_probe_ok) {
            return fail(Plaza2TradeOrderEntryFailure::RuntimeProbeFailed, result.message);
        }
        if (!health.scheme_drift_ok) {
            return fail(Plaza2TradeOrderEntryFailure::SchemeDriftIncompatible, result.message);
        }
        return fail(Plaza2TradeOrderEntryFailure::PrivateStateNotReady, result.message);
    }
    return ok(result.message);
}

Plaza2TradeOrderEntryResult Plaza2TradeLiveOrderEntryGateway::poll_private_state() {
    const auto result = private_runner_.poll_once();
    if (!result.ok) {
        return fail(Plaza2TradeOrderEntryFailure::PrivateStateNotReady, result.message);
    }
    return ok(result.message);
}

bool Plaza2TradeLiveOrderEntryGateway::private_ready() const noexcept {
    return private_runner_.health_snapshot().ready;
}

std::optional<std::int64_t> Plaza2TradeLiveOrderEntryGateway::find_order_id(std::int32_t ext_id,
                                                                            std::string_view client_code) const {
    const auto orders = private_runner_.projector().own_orders();
    const auto found = std::find_if(orders.begin(), orders.end(), [&](const auto& order) {
        return order.ext_id == ext_id && order.client_code == client_code && !terminal_order_status(order);
    });
    if (found == orders.end()) {
        return std::nullopt;
    }
    return found->public_order_id != 0 ? found->public_order_id : found->private_order_id;
}

bool Plaza2TradeLiveOrderEntryGateway::order_cancelled(std::int64_t order_id) const {
    const auto orders = private_runner_.projector().own_orders();
    return std::any_of(orders.begin(), orders.end(), [&](const auto& order) {
        const bool same_order = order.public_order_id == order_id || order.private_order_id == order_id;
        return same_order && terminal_order_status(order);
    });
}

Plaza2TradeOrderEntryResult Plaza2TradeLiveOrderEntryGateway::open_publisher() {
    const auto result = private_runner_.open_publisher(config_.publisher);
    if (!result.ok) {
        return fail(Plaza2TradeOrderEntryFailure::PublisherOpenFailed, result.message);
    }
    return ok(result.message);
}

Plaza2TradeOrderEntryResult Plaza2TradeLiveOrderEntryGateway::post_command(const Plaza2TradeEncodedCommand& command,
                                                                           std::uint64_t user_id) {
    const auto result = private_runner_.post_publisher_message({
        .message_name = command.command_name,
        .payload = command.payload,
        .user_id = user_id,
        .need_reply = true,
    });
    if (!result.ok) {
        return fail(Plaza2TradeOrderEntryFailure::CommandSendFailed, result.message);
    }
    return ok(result.message);
}

void Plaza2TradeLiveOrderEntryGateway::stop() {
    static_cast<void>(private_runner_.stop());
}

Plaza2TradeTestOrderRunner::Plaza2TradeTestOrderRunner(Plaza2TradeOrderEntryConfig config,
                                                       std::unique_ptr<Plaza2TradeOrderEntryGateway> gateway)
    : config_(std::move(config)), gateway_(std::move(gateway)) {}

Plaza2TradeTestOrderRunner::~Plaza2TradeTestOrderRunner() {
    if (gateway_) {
        gateway_->stop();
    }
}

Plaza2TradeOrderEntryResult Plaza2TradeTestOrderRunner::run() {
    auto validation = validate_plaza2_trade_order_entry_config(config_);
    if (!validation.ok) {
        return validation;
    }

    auto add = codec_.encode(Plaza2TradeCommandRequest{make_plaza2_trade_add_order_request(config_.tiny_order)});
    if (!add.validation.ok()) {
        return fail(Plaza2TradeOrderEntryFailure::CommandValidationFailed, add.validation.message);
    }

    Plaza2TradeOrderEntryEvidence evidence;
    evidence.dry_run = config_.mode == Plaza2TradeOrderEntryMode::DryRun;
    evidence.command_encoded = true;
    evidence.add_order_msgid = add.msgid;
    evidence.add_user_id = static_cast<std::uint64_t>(config_.tiny_order.ext_id);

    if (config_.mode == Plaza2TradeOrderEntryMode::DryRun) {
        auto result = ok("dry-run encoded AddOrder and stopped before live publisher submission");
        result.evidence = evidence;
        return result;
    }
    if (!gateway_) {
        return fail(Plaza2TradeOrderEntryFailure::Unknown, "live TEST order runner requires a gateway");
    }

    if (auto started = gateway_->start_private_state(); !started.ok) {
        return started;
    }
    for (std::uint32_t poll = 0; poll < config_.max_polls && !gateway_->private_ready(); ++poll) {
        if (auto polled = gateway_->poll_private_state(); !polled.ok) {
            return polled;
        }
    }
    if (!gateway_->private_ready()) {
        auto result = fail(Plaza2TradeOrderEntryFailure::PrivateStateNotReady,
                           "private-state streams did not reach ready before TEST order submission");
        result.evidence = evidence;
        return result;
    }
    if (auto opened = gateway_->open_publisher(); !opened.ok) {
        opened.evidence = evidence;
        return opened;
    }
    if (auto posted = gateway_->post_command(add, evidence.add_user_id); !posted.ok) {
        posted.evidence = evidence;
        return posted;
    }
    evidence.command_submitted = true;

    std::optional<std::int64_t> order_id;
    for (std::uint32_t poll = 0; poll < config_.max_polls; ++poll) {
        if (auto polled = gateway_->poll_private_state(); !polled.ok) {
            polled.evidence = evidence;
            return polled;
        }
        order_id = gateway_->find_order_id(config_.tiny_order.ext_id, config_.tiny_order.client_code);
        if (order_id.has_value()) {
            break;
        }
    }
    if (!order_id.has_value()) {
        auto result = fail(Plaza2TradeOrderEntryFailure::ReplicationConfirmationTimeout,
                           "AddOrder was submitted but private replication did not confirm it");
        result.evidence = evidence;
        return result;
    }
    evidence.private_order_seen = true;
    evidence.user_orderbook_seen = true;
    evidence.confirmed_order_id = *order_id;

    auto del =
        codec_.encode(Plaza2TradeCommandRequest{make_plaza2_trade_del_order_request(config_.tiny_order, *order_id)});
    if (!del.validation.ok()) {
        auto result = fail(Plaza2TradeOrderEntryFailure::CommandValidationFailed, del.validation.message);
        result.evidence = evidence;
        return result;
    }
    evidence.del_order_msgid = del.msgid;
    evidence.cancel_user_id = evidence.add_user_id + 1U;
    if (auto posted = gateway_->post_command(del, evidence.cancel_user_id); !posted.ok) {
        posted.failure = Plaza2TradeOrderEntryFailure::CancelSendFailed;
        posted.evidence = evidence;
        posted.evidence.failure_classification =
            std::string(plaza2_trade_order_entry_failure_name(Plaza2TradeOrderEntryFailure::CancelSendFailed));
        return posted;
    }
    evidence.cancel_submitted = true;

    for (std::uint32_t poll = 0; poll < config_.max_polls; ++poll) {
        if (auto polled = gateway_->poll_private_state(); !polled.ok) {
            polled.evidence = evidence;
            return polled;
        }
        if (gateway_->order_cancelled(*order_id)) {
            evidence.cancel_confirmed = true;
            break;
        }
    }
    if (!evidence.cancel_confirmed) {
        auto result = fail(Plaza2TradeOrderEntryFailure::CancelConfirmationTimeout,
                           "DelOrder was submitted but private replication did not confirm cancellation");
        result.evidence = evidence;
        return result;
    }

    auto result = ok("TEST AddOrder and DelOrder were submitted and confirmed through private replication");
    result.evidence = evidence;
    return result;
}

} // namespace moex::plaza2_trade
