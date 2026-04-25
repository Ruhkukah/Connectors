#include "moex/plaza2_trade/plaza2_trade_test_order_runner.hpp"

#include <cstdlib>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>

namespace {

using namespace moex::plaza2_trade;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Plaza2TradeOrderEntryConfig base_config() {
    Plaza2TradeOrderEntryConfig config;
    config.profile_id = "phase5e-test";
    config.mode = Plaza2TradeOrderEntryMode::DryRun;
    config.arm_state.test_network_armed = true;
    config.arm_state.test_session_armed = true;
    config.arm_state.test_plaza2_armed = true;
    config.order_entry_armed = true;
    config.tiny_order_armed = true;
    config.private_session.runtime.environment = moex::plaza2::cgate::Plaza2Environment::Test;
    config.tiny_order.isin_id = 123456;
    config.tiny_order.broker_code = "BRK1";
    config.tiny_order.client_code = "CL1";
    config.tiny_order.side = Plaza2TradeSide::Buy;
    config.tiny_order.price = "1.00000";
    config.tiny_order.quantity = 1;
    config.tiny_order.max_quantity = 1;
    config.tiny_order.ext_id = 700001;
    config.tiny_order.client_transaction_id_prefix = "p5e-synth";
    config.tiny_order.comment = "p5e-synth";
    config.publisher.settings = "p2mq://FORTS_SRV;category=FORTS_MSG;name=phase5e";
    return config;
}

class FakeGateway final : public Plaza2TradeOrderEntryGateway {
  public:
    Plaza2TradeOrderEntryResult start_private_state() override {
        started = true;
        return ok_result("private started");
    }

    Plaza2TradeOrderEntryResult poll_private_state() override {
        polls += 1;
        if (submitted_add && polls >= 2) {
            order_id = 9000001;
        }
        if (submitted_cancel && polls >= 4) {
            cancelled = true;
        }
        return ok_result("private poll");
    }

    bool private_ready() const noexcept override {
        return started;
    }

    std::optional<std::int64_t> find_order_id(std::int32_t, std::string_view) const override {
        return order_id;
    }

    bool order_cancelled(std::int64_t) const override {
        return cancelled;
    }

    Plaza2TradeOrderEntryResult open_publisher() override {
        publisher_opened = true;
        return ok_result("publisher opened");
    }

    Plaza2TradeOrderEntryResult post_command(const Plaza2TradeEncodedCommand& command, std::uint64_t) override {
        require(publisher_opened, "publisher must be opened before post");
        if (command.command_kind == Plaza2TradeCommandKind::AddOrder) {
            submitted_add = true;
        } else if (command.command_kind == Plaza2TradeCommandKind::DelOrder) {
            submitted_cancel = true;
        } else {
            return fail_result(Plaza2TradeOrderEntryFailure::CommandSendFailed, "unexpected live command");
        }
        return ok_result("posted");
    }

    void stop() override {}

    static Plaza2TradeOrderEntryResult ok_result(std::string message) {
        Plaza2TradeOrderEntryResult result;
        result.ok = true;
        result.failure = Plaza2TradeOrderEntryFailure::None;
        result.message = std::move(message);
        return result;
    }

    static Plaza2TradeOrderEntryResult fail_result(Plaza2TradeOrderEntryFailure failure, std::string message) {
        Plaza2TradeOrderEntryResult result;
        result.ok = false;
        result.failure = failure;
        result.message = std::move(message);
        return result;
    }

    bool started{false};
    bool publisher_opened{false};
    bool submitted_add{false};
    bool submitted_cancel{false};
    bool cancelled{false};
    std::uint32_t polls{0};
    std::optional<std::int64_t> order_id;
};

void test_refuses_missing_order_entry_arm() {
    auto config = base_config();
    config.order_entry_armed = false;
    const auto result = validate_plaza2_trade_order_entry_config(config);
    require(!result.ok, "missing order-entry arm must fail");
    require(result.failure == Plaza2TradeOrderEntryFailure::MissingArmFlag, "missing arm classification");
}

void test_refuses_missing_tiny_order_arm() {
    auto config = base_config();
    config.tiny_order_armed = false;
    const auto result = validate_plaza2_trade_order_entry_config(config);
    require(!result.ok, "missing tiny-order arm must fail");
    require(result.failure == Plaza2TradeOrderEntryFailure::MissingArmFlag, "missing tiny arm classification");
}

void test_refuses_live_send_without_send_flag() {
    auto config = base_config();
    config.mode = Plaza2TradeOrderEntryMode::SendTestOrder;
    config.send_test_order = false;
    const auto result = validate_plaza2_trade_order_entry_config(config);
    require(!result.ok, "live send without send flag must fail");
    require(result.failure == Plaza2TradeOrderEntryFailure::MissingArmFlag, "send flag classification");
}

void test_dry_run_encodes_without_gateway() {
    auto config = base_config();
    Plaza2TradeTestOrderRunner runner(config, nullptr);
    const auto result = runner.run();
    require(result.ok, "dry-run must succeed without a live gateway");
    require(result.evidence.command_encoded, "dry-run must encode AddOrder");
    require(!result.evidence.command_submitted, "dry-run must not submit AddOrder");
    require(result.evidence.add_order_msgid == 474, "dry-run AddOrder msgid");
}

void test_fake_live_add_cancel_path() {
    auto config = base_config();
    config.mode = Plaza2TradeOrderEntryMode::SendTestOrder;
    config.send_test_order = true;
    Plaza2TradeTestOrderRunner runner(config, std::make_unique<FakeGateway>());
    const auto result = runner.run();
    require(result.ok, "fake live TEST add/cancel path must succeed");
    require(result.evidence.command_submitted, "AddOrder submitted");
    require(result.evidence.private_order_seen, "AddOrder confirmed");
    require(result.evidence.cancel_submitted, "DelOrder submitted");
    require(result.evidence.cancel_confirmed, "DelOrder confirmed");
}

void test_invalid_quantity_fails() {
    auto config = base_config();
    config.tiny_order.quantity = 2;
    const auto result = validate_plaza2_trade_order_entry_config(config);
    require(!result.ok, "quantity over max must fail");
    require(result.failure == Plaza2TradeOrderEntryFailure::InvalidOrderProfile, "quantity classification");
}

} // namespace

int main() {
    try {
        test_refuses_missing_order_entry_arm();
        test_refuses_missing_tiny_order_arm();
        test_refuses_live_send_without_send_flag();
        test_dry_run_encodes_without_gateway();
        test_fake_live_add_cancel_path();
        test_invalid_quantity_fails();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
