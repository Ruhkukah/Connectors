#include "adapters/alorengine_capi/moex_c_api.h"
#include "moex/plaza2_twime_reconciler/plaza2_twime_integrated_test_runner.hpp"

#include "local_tcp_server.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "plaza2_twime_integrated_test_support.hpp"
#include "plaza2_twime_reconciler_test_support.hpp"
#include "twime_live_session_test_support.hpp"
#include "twime_trade_test_support.hpp"

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "expected fake runtime library path\n";
            return 1;
        }

        namespace plaza_test = moex::plaza2::test;
        namespace integrated_test = moex::plaza2_twime_reconciler::test;
        using namespace moex::plaza2_twime_reconciler;
        using moex::plaza2_twime_reconciler::test_support::price_to_mantissa;
        using moex::plaza2_twime_reconciler::test_support::set_field;
        using moex::twime_sbe::TwimeFieldValue;
        using moex::twime_trade::test::decode_bytes;
        using moex::twime_trade::test::encode_bytes;
        using moex::twime_trade::test::make_request;

        const auto fake_library = std::filesystem::path(argv[1]);
        const auto fixture_root = plaza_test::make_temp_directory("plaza2_twime_integrated_runner_test");
        const auto cleanup = [&]() { plaza_test::remove_tree(fixture_root); };

        const auto scheme_text = plaza_test::build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture = plaza_test::materialize_runtime_fixture(
            fixture_root, fake_library, moex::plaza2::cgate::Plaza2Environment::Test, scheme_text);

        moex::test::LocalTcpServer server;
        moex::twime_trade::test::ScopedEnvVar twime_env("MOEX_TWIME_TEST_CREDENTIALS", "TWIME-INTEGRATED-SECRET");
        moex::twime_trade::test::ScopedEnvVar plaza_env("MOEX_PLAZA2_TEST_CREDENTIALS", "PLAZA-INTEGRATED-SECRET");

        auto config = integrated_test::make_integrated_config(fixture, server.port(), "phase4c_integrated_runner");
        moex::twime_trade::test::ManualRunnerClock runner_clock;

        std::exception_ptr server_error;
        std::jthread server_thread([&] {
            try {
                integrated_test::require(server.wait_for_client(std::chrono::milliseconds(3000)),
                                         "expected integrated runner TWIME client connection");

                const auto establish = server.receive_up_to(1024, std::chrono::milliseconds(3000));
                const auto establish_decoded = decode_bytes(establish);
                integrated_test::require(establish_decoded.metadata != nullptr &&
                                             establish_decoded.metadata->name == "Establish",
                                         "integrated runner must send Establish");

                auto ack = make_request("EstablishmentAck");
                set_field(ack, "NextSeqNo", TwimeFieldValue::unsigned_integer(11));
                server.send_bytes(encode_bytes(ack));

                auto accepted = make_request("NewOrderSingleResponse");
                set_field(accepted, "ClOrdID", TwimeFieldValue::unsigned_integer(501));
                set_field(accepted, "OrderID", TwimeFieldValue::signed_integer(20003));
                set_field(accepted, "TradingSessionID", TwimeFieldValue::signed_integer(321));
                set_field(accepted, "SecurityID", TwimeFieldValue::signed_integer(1001));
                set_field(accepted, "OrderQty", TwimeFieldValue::unsigned_integer(7));
                set_field(accepted, "Price", TwimeFieldValue::decimal(price_to_mantissa("102500")));
                set_field(accepted, "Side", TwimeFieldValue::enum_name("Sell"));
                server.send_bytes(encode_bytes(accepted));

                auto execution = make_request("ExecutionSingleReport");
                set_field(execution, "ClOrdID", TwimeFieldValue::unsigned_integer(501));
                set_field(execution, "OrderID", TwimeFieldValue::signed_integer(20003));
                set_field(execution, "TrdMatchID", TwimeFieldValue::signed_integer(9001));
                set_field(execution, "TradingSessionID", TwimeFieldValue::signed_integer(321));
                set_field(execution, "SecurityID", TwimeFieldValue::signed_integer(1001));
                set_field(execution, "LastPx", TwimeFieldValue::decimal(price_to_mantissa("102500")));
                set_field(execution, "LastQty", TwimeFieldValue::unsigned_integer(2));
                set_field(execution, "OrderQty", TwimeFieldValue::unsigned_integer(7));
                set_field(execution, "Side", TwimeFieldValue::enum_name("Sell"));
                server.send_bytes(encode_bytes(execution));

                for (;;) {
                    const auto inbound = server.receive_up_to(1024, std::chrono::milliseconds(3000));
                    if (inbound.empty()) {
                        continue;
                    }
                    const auto decoded = decode_bytes(inbound);
                    if (decoded.metadata != nullptr && decoded.metadata->name == "Terminate") {
                        server.send_bytes(encode_bytes(make_request("Terminate")));
                        server.close_client();
                        return;
                    }
                }
            } catch (...) {
                server_error = std::current_exception();
            }
        });

        Plaza2TwimeIntegratedTestRunner runner(std::move(config));
        runner.set_time_source([&runner_clock] { return runner_clock(); });

        const auto start = runner.start();
        integrated_test::require(start.ok, "integrated runner start must succeed with all arms and fake fixtures");

        integrated_test::pump_runner_until(runner, runner_clock, [](const Plaza2TwimeIntegratedTestRunner& candidate) {
            return candidate.health_snapshot().readiness.ready;
        });

        const auto& health = runner.health_snapshot();
        integrated_test::require(health.state == Plaza2TwimeIntegratedRunnerState::Ready,
                                 "integrated runner must reach ready state");
        integrated_test::require(health.twime_validation_ok, "TWIME validation must succeed");
        integrated_test::require(health.plaza_validation_ok, "PLAZA validation must succeed");
        integrated_test::require(health.plaza_runtime_probe_ok, "PLAZA runtime probe must succeed");
        integrated_test::require(health.plaza_scheme_drift_ok, "PLAZA scheme drift validation must succeed");
        integrated_test::require(health.reconciler_updating,
                                 "reconciler should be updating once both sources are attached");
        integrated_test::require(health.abi_handle_valid, "integrated runner must create a Phase 4B ABI handle");
        integrated_test::require(health.abi_snapshot_attached,
                                 "integrated runner must attach snapshots into the ABI handle");
        integrated_test::require(health.readiness.twime_session_established, "TWIME readiness must be visible");
        integrated_test::require(health.readiness.plaza_runtime_probe_ok, "PLAZA probe readiness must be visible");
        integrated_test::require(health.readiness.plaza_scheme_drift_ok, "PLAZA drift readiness must be visible");
        integrated_test::require(health.readiness.plaza_streams_open, "PLAZA stream-open readiness must be visible");
        integrated_test::require(health.readiness.plaza_streams_online,
                                 "PLAZA stream-online readiness must be visible");
        integrated_test::require(health.readiness.plaza_streams_snapshot_complete,
                                 "PLAZA snapshot-complete readiness must be visible");
        integrated_test::require(health.readiness.reconciler_attached, "reconciler attachment must be visible");
        integrated_test::require(health.readiness.abi_snapshot_attached, "ABI attachment must be visible");
        integrated_test::require(health.readiness.ready, "overall readiness must become true");
        integrated_test::require(health.readiness.blocker.empty(), "ready runner must not retain a readiness blocker");
        integrated_test::require(health.evidence.startup_report_ready, "startup evidence flag must be set");
        integrated_test::require(health.evidence.readiness_summary_ready, "readiness evidence flag must be set");
        integrated_test::require(health.reconciled_order_count >= 1, "reconciled orders must be available");
        integrated_test::require(health.reconciled_trade_count >= 1, "reconciled trades must be available");
        integrated_test::require(health.reconciler.total_diverged_orders >= 1,
                                 "diverged order count must be reflected");
        integrated_test::require(health.reconciler.total_matched_trades >= 1, "matched trade count must be reflected");

        auto* handle = runner.abi_handle();
        integrated_test::require(handle != nullptr, "integrated runner must expose the attached ABI handle");

        MoexPlaza2PrivateConnectorHealth private_health{};
        integrated_test::require(moex_get_plaza2_private_connector_health(handle, &private_health) == MOEX_RESULT_OK,
                                 "private connector health ABI export must succeed");
        integrated_test::require(private_health.online == 1U, "private connector health must report online");

        uint32_t private_order_count = 0;
        integrated_test::require(moex_get_plaza2_own_order_count(handle, &private_order_count) == MOEX_RESULT_OK,
                                 "private own-order count ABI export must succeed");
        integrated_test::require(private_order_count >= 1U, "private own-order snapshot must be attached");

        uint32_t reconciled_order_count = 0;
        integrated_test::require(moex_get_plaza2_reconciled_order_count(handle, &reconciled_order_count) ==
                                     MOEX_RESULT_OK,
                                 "reconciled order count ABI export must succeed");
        integrated_test::require(reconciled_order_count >= 1U, "reconciled orders must be exported through the ABI");

        std::vector<MoexPlaza2ReconciledOrderItem> reconciled_orders(reconciled_order_count);
        uint32_t written = 0;
        integrated_test::require(moex_copy_plaza2_reconciled_order_items(handle, reconciled_orders.data(),
                                                                         reconciled_order_count,
                                                                         &written) == MOEX_RESULT_OK,
                                 "reconciled order copy-out must succeed");
        integrated_test::require(written == reconciled_order_count, "reconciled order copy-out must write all items");
        bool found_diverged_order = false;
        for (const auto& item : reconciled_orders) {
            if (item.twime_order_id == 20003 && item.plaza_private_order_id == 20003) {
                found_diverged_order = true;
                integrated_test::require(item.status == MOEX_PLAZA2_ORDER_STATUS_DIVERGED,
                                         "diverged integrated order must preserve Diverged status");
            }
        }
        integrated_test::require(found_diverged_order,
                                 "expected diverged integrated order is missing from the ABI snapshot");

        uint32_t reconciled_trade_count = 0;
        integrated_test::require(moex_get_plaza2_reconciled_trade_count(handle, &reconciled_trade_count) ==
                                     MOEX_RESULT_OK,
                                 "reconciled trade count ABI export must succeed");
        integrated_test::require(reconciled_trade_count >= 1U, "reconciled trades must be exported through the ABI");

        std::vector<MoexPlaza2ReconciledTradeItem> reconciled_trades(reconciled_trade_count);
        integrated_test::require(moex_copy_plaza2_reconciled_trade_items(handle, reconciled_trades.data(),
                                                                         reconciled_trade_count,
                                                                         &written) == MOEX_RESULT_OK,
                                 "reconciled trade copy-out must succeed");
        integrated_test::require(written == reconciled_trade_count, "reconciled trade copy-out must write all items");
        bool found_matched_trade = false;
        for (const auto& item : reconciled_trades) {
            if (item.twime_trade_id == 9001 && item.plaza_trade_id == 9001) {
                found_matched_trade = true;
                integrated_test::require(item.status == MOEX_PLAZA2_TRADE_STATUS_MATCHED,
                                         "matched integrated trade must export Matched status");
            }
        }
        integrated_test::require(found_matched_trade,
                                 "expected matched integrated trade is missing from the ABI snapshot");

        const auto& logs = runner.operator_log_lines();
        integrated_test::require(!logs.empty(), "integrated runner must accumulate operator logs");
        integrated_test::require(integrated_test::find_log_index(logs, "[TWIME]") < logs.size(),
                                 "TWIME log prefix must be preserved");
        integrated_test::require(integrated_test::find_log_index(logs, "[PLAZA]") < logs.size(),
                                 "PLAZA log prefix must be preserved");
        integrated_test::require(integrated_test::find_log_index(logs, "[REDACTED_ENDPOINT]") < logs.size(),
                                 "integrated runner must redact child endpoint lines");
        integrated_test::require(integrated_test::find_log_index(logs, "[REDACTED]") < logs.size(),
                                 "integrated runner must redact child credential lines");
        for (const auto& line : logs) {
            integrated_test::require(line.find("TWIME-INTEGRATED-SECRET") == std::string::npos,
                                     "integrated operator logs must not leak raw TWIME credentials");
            integrated_test::require(line.find("PLAZA-INTEGRATED-SECRET") == std::string::npos,
                                     "integrated operator logs must not leak raw PLAZA credentials");
            integrated_test::require(line.find("127.0.0.1:") == std::string::npos,
                                     "integrated operator logs must not leak the raw TWIME endpoint");
            integrated_test::require(line.find("198.51.100.10:4001") == std::string::npos,
                                     "integrated operator logs must not leak the raw PLAZA endpoint");
        }

        const auto stop = runner.stop();
        integrated_test::require(stop.ok, "integrated runner stop must succeed");
        integrated_test::require(runner.health_snapshot().evidence.final_summary_ready,
                                 "final evidence flag must be set on stop");

        if (server_thread.joinable()) {
            server_thread.join();
        }
        if (server_error) {
            std::rethrow_exception(server_error);
        }

        cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
