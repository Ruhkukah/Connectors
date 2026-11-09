#include "full_order_log_dtc_loop.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace moex::connector_host;
namespace cg = moex::plaza2::cgate;
namespace {
void require(bool value, const char* reason) {
    if (!value)
        throw std::runtime_error(reason);
}
} // namespace
int main() {
    try {
        FullOrderLogDtcMetrics interval;
        moex::plaza2_trade::CgatePollMetrics first;
        first.sequence = 1;
        first.process_calls = 4;
        first.process_ns = 9000000; // Sum of four native calls, deliberately not their maximum.
        first.unattributed_callbacks = 3;
        first.callbacks = {{.stream_code = cg::kFullOrderLogStreamCode, .native_message_type = 100, .count = 4},
                           {.stream_code = cg::kFullOrderLogStreamCode, .native_message_type = 102, .count = 2},
                           {.stream_code = moex::plaza2::generated::StreamCode::kFortsRefdataRepl,
                            .native_message_type = 100,
                            .count = 5}};
        interval.observe(first);
        interval.observe(first);
        require(interval.callbacks == 14 && interval.full_order_log_attributed_callbacks == 6 &&
                    interval.unattributed_callbacks == 3 && interval.process_calls == 4,
                "native counts include all types/streams and unattributed exactly once per poll");
        auto second = first;
        second.sequence = 2;
        second.process_ns = 4000000;
        second.process_calls = 2;
        second.unattributed_callbacks = 1;
        second.callbacks.clear();
        interval.observe(second);
        require(interval.callbacks == 15 && interval.process_calls == 6 &&
                    interval.cg_conn_process_loop_max_ns == 9000000,
                "reported maximum is the largest one-poll native-pump sum, not the interval sum");
        require(FullOrderLogDtcMetrics::report_interval == std::chrono::minutes(1) &&
                    interval.callbacks /
                            std::chrono::duration<double>(FullOrderLogDtcMetrics::report_interval).count() ==
                        0.25,
                "callback rate uses a minute rather than the previous thirty-second interval");
        interval.reset_interval();
        interval.observe(second);
        require(interval.callbacks == 0 && interval.process_calls == 0 && interval.cg_conn_process_loop_max_ns == 0,
                "new interval cannot double-count the preceding poll or retain its maximum");
        second.sequence = 3;
        second.process_ns = 7000000;
        interval.observe(second);
        require(interval.callbacks == 1 && interval.cg_conn_process_loop_max_ns == 7000000,
                "next reporting window starts fresh while retaining source sequence deduplication");
        std::ostringstream diagnostic;
        log_full_order_log_listener_event(diagnostic,
                                          {.kind = cg::Plaza2ListenerEventKind::Close, .close_reason = 0xA006});
        diagnostic << " next_decimal=" << 42;
        const auto text = diagnostic.str();
        require(text.find("close_reason=40966 close_reason_hex=0xA006") != std::string::npos &&
                    text.find("close_reason_name=REPL:CHANNEL_DISCONNECT_ON_TIMEOUT") != std::string::npos &&
                    text.find("next_decimal=42") != std::string::npos,
                "native close reason is visible in decimal/hex with its name and formatting is restored");
        std::ostringstream ordinary;
        log_full_order_log_listener_event(ordinary, {.kind = cg::Plaza2ListenerEventKind::Close, .close_reason = 3});
        require(ordinary.str().find("close_reason=3 close_reason_hex=0x3") != std::string::npos &&
                    ordinary.str().find("CHANNEL_DISCONNECT_ON_TIMEOUT") == std::string::npos,
                "other close reasons are retained without being misclassified as 0xA006");
        std::cout << "PASS minute native-callback rates, measured process-loop maximum and close-reason diagnostics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
