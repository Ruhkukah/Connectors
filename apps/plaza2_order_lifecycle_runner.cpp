#include "moex/plaza2_trade/plaza2_order_lifecycle.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using moex::plaza2::cgate::Plaza2Environment;
using moex::plaza2_trade::OrderLifecycleConfig;
using moex::plaza2_trade::Plaza2TradeSide;

template <typename T> bool parse_integer(std::string_view text, T& value) {
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_bool(std::string_view text, bool& value) {
    if (text == "true") {
        value = true;
        return true;
    }
    if (text == "false") {
        value = false;
        return true;
    }
    return false;
}

struct Arguments {
    OrderLifecycleConfig config;
    std::filesystem::path output_directory;
};

bool parse_arguments(int argc, char** argv, Arguments& arguments, std::string& error) {
    auto require_value = [&](int& index, std::string_view option) -> std::string_view {
        if (index + 1 >= argc) {
            error = std::string(option) + " requires a value";
            return {};
        }
        return argv[++index];
    };

    bool mode_seen = false;
    for (int index = 1; index < argc && error.empty(); ++index) {
        const std::string_view option(argv[index]);
        if (option == "--dry-run") {
            arguments.config.dry_run = true;
            if (mode_seen && arguments.config.send_test_order) {
                arguments.config.dry_run = true;
            }
            mode_seen = true;
        } else if (option == "--send-test-order") {
            arguments.config.send_test_order = true;
            if (!mode_seen) {
                arguments.config.dry_run = false;
            }
            mode_seen = true;
        } else if (option == "--profile-id") {
            arguments.config.profile_id = require_value(index, option);
        } else if (option == "--profile-fingerprint") {
            arguments.config.profile_fingerprint = require_value(index, option);
        } else if (option == "--profile-enabled") {
            const auto value = require_value(index, option);
            if (!parse_bool(value, arguments.config.profile_enabled)) {
                error = "--profile-enabled must be true or false";
            }
        } else if (option == "--environment") {
            const auto value = require_value(index, option);
            if (value == "test") {
                arguments.config.environment = Plaza2Environment::Test;
            } else if (value == "prod") {
                arguments.config.environment = Plaza2Environment::Prod;
            } else {
                error = "--environment must be test or prod";
            }
        } else if (option == "--run-id") {
            arguments.config.run_id = require_value(index, option);
        } else if (option == "--output-dir") {
            arguments.output_directory = require_value(index, option);
        } else if (option == "--journal-root") {
            arguments.config.journal_root = require_value(index, option);
        } else if (option == "--isin-id") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.isin_id)) {
                error = "invalid --isin-id";
            }
        } else if (option == "--base-contract-code") {
            arguments.config.base_contract_code = require_value(index, option);
        } else if (option == "--instrument-mask") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.instrument_mask)) {
                error = "invalid --instrument-mask";
            }
        } else if (option == "--broker-code") {
            arguments.config.broker_code = require_value(index, option);
        } else if (option == "--client-code") {
            arguments.config.client_code = require_value(index, option);
        } else if (option == "--side") {
            const auto value = require_value(index, option);
            if (value == "buy") {
                arguments.config.side = Plaza2TradeSide::Buy;
            } else if (value == "sell") {
                arguments.config.side = Plaza2TradeSide::Sell;
            } else {
                error = "--side must be buy or sell";
            }
        } else if (option == "--price") {
            arguments.config.price = require_value(index, option);
        } else if (option == "--quantity") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.quantity)) {
                error = "invalid --quantity";
            }
        } else if (option == "--ext-id") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.ext_id)) {
                error = "invalid --ext-id";
            }
        } else if (option == "--add-user-id") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.add_user_id)) {
                error = "invalid --add-user-id";
            }
        } else if (option == "--cancel-user-id") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.cancel_user_id)) {
                error = "invalid --cancel-user-id";
            }
        } else if (option == "--recovery-user-id") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.recovery_user_id)) {
                error = "invalid --recovery-user-id";
            }
        } else if (option == "--comment") {
            arguments.config.comment = require_value(index, option);
        } else if (option == "--tick-size") {
            arguments.config.smoke.tick_size = require_value(index, option);
        } else if (option == "--top-bid") {
            arguments.config.smoke.top_bid = require_value(index, option);
        } else if (option == "--top-ask") {
            arguments.config.smoke.top_ask = require_value(index, option);
        } else if (option == "--market-data-source") {
            arguments.config.smoke.market_data_source = require_value(index, option);
        } else if (option == "--aggr20-source-sequence") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.smoke.aggr20_source_sequence)) {
                error = "invalid --aggr20-source-sequence";
            }
        } else if (option == "--aggr20-source-revision") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.smoke.aggr20_source_revision)) {
                error = "invalid --aggr20-source-revision";
            }
        } else if (option == "--aggr20-observed-at-utc") {
            arguments.config.smoke.aggr20_observed_at_utc = require_value(index, option);
        } else if (option == "--aggr20-age-ms") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.smoke.aggr20_age_ms)) {
                error = "invalid --aggr20-age-ms";
            }
        } else if (option == "--max-aggr20-age-ms") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.smoke.max_aggr20_age_ms)) {
                error = "invalid --max-aggr20-age-ms";
            }
        } else if (option == "--trading-day") {
            arguments.config.smoke.trading_day = require_value(index, option);
        } else if (option == "--session-id") {
            arguments.config.smoke.session_id = require_value(index, option);
        } else if (option == "--session-state") {
            arguments.config.smoke.session_state = require_value(index, option);
        } else if (option == "--refdata-source") {
            arguments.config.smoke.refdata_source = require_value(index, option);
        } else if (option == "--refdata-source-sequence") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.smoke.refdata_source_sequence)) {
                error = "invalid --refdata-source-sequence";
            }
        } else if (option == "--refdata-source-revision") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.smoke.refdata_source_revision)) {
                error = "invalid --refdata-source-revision";
            }
        } else if (option == "--limits-source") {
            arguments.config.smoke.limits_source = require_value(index, option);
        } else if (option == "--limits-commit-sequence") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.smoke.limits_commit_sequence)) {
                error = "invalid --limits-commit-sequence";
            }
        } else if (option == "--smoke-policy-version") {
            arguments.config.policy.version = require_value(index, option);
        } else if (option == "--smoke-policy-sha256") {
            arguments.config.policy.sha256 = require_value(index, option);
        } else if (option == "--max-distance-ticks") {
            const auto value = require_value(index, option);
            if (!parse_integer(value, arguments.config.policy.max_distance_ticks)) {
                error = "invalid --max-distance-ticks";
            }
        } else if (option == "--plan-sha256") {
            arguments.config.authorized_plan_sha256 = require_value(index, option);
        } else if (option == "--instrument-exists") {
            arguments.config.smoke.instrument_exists = true;
        } else if (option == "--tradable-session") {
            arguments.config.smoke.tradable_session = true;
        } else if (option == "--aggr20-two-sided") {
            arguments.config.smoke.aggr20_two_sided = true;
        } else if (option == "--limits-snapshot-applicable") {
            arguments.config.smoke.limits_snapshot_applicable = true;
        } else if (option.starts_with("--armed-")) {
            arguments.config.any_arm_flag = true;
        } else {
            error = "unknown argument: " + std::string(option);
        }
    }
    if (!mode_seen) {
        arguments.config.dry_run = true;
    }
    if (arguments.output_directory.empty()) {
        error = "--output-dir is required";
    }
    return error.empty();
}

} // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    std::string error;
    if (!parse_arguments(argc, argv, arguments, error)) {
        std::cerr << error << '\n';
        return 2;
    }

    const auto plan = moex::plaza2_trade::build_pre_send_plan(arguments.config);
    if (!plan.ok) {
        std::cerr << moex::plaza2_trade::pre_send_failure_name(plan.failure) << ": " << plan.message << '\n';
        return 2;
    }
    if (arguments.config.send_test_order) {
        std::cerr
            << "live TEST transport is intentionally unavailable in this offline increment; plan hash validated\n";
        return 3;
    }
    if (!moex::plaza2_trade::write_pre_send_plan(arguments.output_directory, plan, error)) {
        std::cerr << error << '\n';
        return 2;
    }
    std::cout << "pre_send_plan_sha256=" << plan.sha256 << '\n';
    return 0;
}
