#include "moex/plaza2_trade/plaza2_test_trade_transport.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "plaza2_trade_test_support.hpp"
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
using namespace moex::plaza2_trade;
using namespace moex::plaza2;
using test::require;
void flag(const char* key, bool on) {
    if (on)
        ::setenv(key, "1", 1);
    else
        ::unsetenv(key);
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "fake runtime path required");
        const auto root = test::make_temp_directory("cgate-recovery");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cgate::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        void* dso = ::dlopen(fixture.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(dso, "load fixture counters");
        auto reset = reinterpret_cast<void (*)()>(::dlsym(dso, "moex_fake_reset_publisher_counts"));
        reset();
        auto envs = reinterpret_cast<std::uint64_t (*)()>(::dlsym(dso, "moex_fake_environment_open_count"));
        auto conns = reinterpret_cast<std::uint64_t (*)()>(::dlsym(dso, "moex_fake_connection_new_count"));
        auto now = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
        Plaza2TestSessionHostConfig config;
        config.runtime.runtime_root = fixture.root;
        config.runtime.env_open_settings = "ini=config/t1.ini;key=00000000";
        // Hash changes are logged and do not reject compatible runtimes.
        config.runtime.expected_runtime_library_sha256 = "wrong";
        config.runtime.expected_scheme_sha256 = "wrong";
        config.connection_settings = "p2tcp://127.0.0.1:4101;app_name=recovery";
        config.publisher_settings = "p2mq://FORTS_SRV;category=FORTS_MSG;name=PUB";
        config.allow_orders = true;
        config.process_timeout_ms = 0;
        config.recovery_now = [&] { return now; };
        std::size_t opening_transitions = 0;
        config.event_log = [&](std::string_view kind, std::string_view fields) {
            if (kind == "cgate_state" && fields.find("\"object\":\"connection\"") != std::string_view::npos &&
                fields.find("\"to\":2") != std::string_view::npos)
                ++opening_transitions;
            require(fields.find("key=") == std::string_view::npos, "CGate interaction log disclosed settings");
        };
        for (auto code : {generated::StreamCode::kFortsTradeRepl, generated::StreamCode::kFortsUserorderbookRepl,
                          generated::StreamCode::kFortsPosRepl, generated::StreamCode::kFortsPartRepl,
                          generated::StreamCode::kFortsRefdataRepl}) {
            auto descriptor = generated::FindStreamByCode(code);
            config.private_streams.push_back(
                {code, "p2repl://" + std::string(descriptor->stream_name), "mode=snapshot+online"});
        }
        config.aggr20_stream = {generated::StreamCode::kFortsAggrRepl, "p2repl://FORTS_AGGR20_REPL",
                                "mode=snapshot+online"};
        flag("MOEX_FAKE_CONN_HOLD_OPENING", true);
        CgateSession session(config);
        const auto wall_start = std::chrono::steady_clock::now();
        require(!session.start(), "nonblocking start");
        require(std::chrono::steady_clock::now() - wall_start < std::chrono::milliseconds(100),
                "start blocked waiting for ACTIVE");
        for (int i = 0; i < 600; ++i) {
            now += std::chrono::seconds(1);
            require(!session.poll(), "10 minute OPENING pump failed");
        }
        require(session.started() && session.recovery_status().operation != Plaza2SessionOperation::Failed,
                "OPENING became Failed");
        require(opening_transitions == 1, "unchanged OPENING state produced duplicate state transitions");
        flag("MOEX_FAKE_CONN_HOLD_OPENING", false);
        const auto pump = [&] {
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(!session.poll(), "pump failed");
            }
        };
        pump();
        if (!session.runtime_health().private_active) {
            auto h = session.runtime_health();
            std::cerr << "callback: " << session.last_callback_error() << " conn=" << h.connection
                      << " private=" << h.private_count << "\n";
            for (std::size_t i = 0; i < h.private_count; ++i)
                std::cerr << static_cast<int>(h.private_streams[i]) << ":" << h.private_states[i] << "\n";
        }
        require(session.runtime_health().private_active, "listeners did not become ONLINE after delayed ACTIVE");
        auto command = Plaza2TradeCodec{}.encode(test_support::make_add_order());
        require(session.post_command(command, 101).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "order blocked after delayed ACTIVE");
        flag("MOEX_FAKE_CONN_TO_OPENING", true);
        pump();
        for (int i = 0; i < 300; ++i) {
            now += std::chrono::seconds(1);
            require(!session.poll(), "5 minute outage failed");
        }
        flag("MOEX_FAKE_CONN_TO_OPENING", false);
        pump();
        if (!session.runtime_health().private_active) {
            std::cerr << "resume callback: " << session.last_callback_error() << "\n";
            for (auto& health : session.private_state().stream_health())
                std::cerr << health.stream_name << " " << health.online << " " << health.snapshot_complete << "\n";
        }
        require(session.runtime_health().private_active, "private streams did not resume after ACTIVE to OPENING");
        flag("MOEX_FAKE_CONNECTION_ERROR", true);
        require(!session.poll(), "asynchronous connection ERROR terminal");
        flag("MOEX_FAKE_CONNECTION_ERROR", false);
        pump();
        require(session.runtime_health().private_active, "asynchronous connection ERROR did not recover");
        flag("MOEX_FAKE_CONN_DIRECT_CLOSED", true);
        require(!session.poll(), "direct connection CLOSED terminal");
        require(!session.runtime_health().private_active, "direct connection CLOSED retained old private ONLINE");
        require(session.post_command(command, 102).certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent,
                "direct connection CLOSED allowed command");
        flag("MOEX_FAKE_CONN_DIRECT_CLOSED", false);
        pump();
        require(session.runtime_health().private_active, "direct connection CLOSED did not rebuild snapshots");
        require(envs() == 1 && conns() == 1, "recovery recreated environment or connection");
        flag("MOEX_FAKE_CALLBACK_CORRUPTION", true);
        require(!session.poll(), "malformed private row terminal");
        flag("MOEX_FAKE_CALLBACK_CORRUPTION", false);
        pump();
        require(session.runtime_health().private_active, "private callback anomaly did not recover");
        // Idle liveness has no age or connection bootstrap deadline.
        for (int i = 0; i < 300; ++i) {
            now += std::chrono::seconds(1);
            require(!session.poll(), "idle poll failed");
        }
        require(session.runtime_health().private_active, "quiet private streams became stale");
        require(!session.stop(), "stop");
        flag("MOEX_FAKE_SCHEME_EXTRA_TABLE", true);
        flag("MOEX_FAKE_CGATE_CLEAR_DELETED_INSIDE_TRANSACTION", true);
        flag("MOEX_FAKE_TIMESTAMP_MILLISECONDS", true);
        std::size_t unknown_clears = 0;
        bool precise_time = false, moscow_time = false;
        config.listener_event_log = [&](const cgate::Plaza2ListenerEvent& event) {
            unknown_clears += event.kind == cgate::Plaza2ListenerEventKind::ClearDeleted;
            for (const auto& field : event.fields) {
                if (field.field_code == generated::FieldCode::kFortsRefdataReplSessionBegin)
                    moscow_time = field.unsigned_value == 1699989200ULL;
                if (field.kind == cgate::Plaza2DecodedValueKind::Timestamp && field.unsigned_value > 1000000000)
                    precise_time |= field.timestamp_ns == field.unsigned_value * 1000000000ULL + 321000000ULL;
            }
        };
        CgateSession additions(config);
        require(!additions.start(), "start additive schema");
        for (int i = 0; i < 10; ++i) {
            now += std::chrono::seconds(1);
            require(!additions.poll(), "additive schema rejected");
        }
        require(additions.runtime_health().private_active, "extra table field caused stream failure");
        require(unknown_clears == 0, "unknown ClearDeleted index mapped onto compacted known table plan");
        require(precise_time, "CGate timestamp fractional milliseconds lost");
        require(moscow_time, "CGate Moscow wall time was not converted to UTC");
        require(!additions.stop(), "stop additions");
        flag("MOEX_FAKE_CGATE_CLEAR_DELETED_INSIDE_TRANSACTION", false);
        flag("MOEX_FAKE_TIMESTAMP_MILLISECONDS", false);
        config.listener_event_log = {};
        flag("MOEX_FAKE_SCHEME_EXTRA_TABLE", false);
        for (const char* mutation : {"MOEX_FAKE_SCHEME_MISSING_PRICE", "MOEX_FAKE_SCHEME_RETYPE_PRICE"}) {
            flag(mutation, true);
            CgateSession incompatible(config);
            require(!incompatible.start(), "bad stream schema should keep session up");
            for (int i = 0; i < 6; ++i) {
                now += std::chrono::seconds(1);
                require(!incompatible.poll(), "incompatible listener killed session");
            }
            require(incompatible.runtime_health().private_active, "incompatible AGGR affected private streams");
            require(incompatible.last_callback_error().find("INCOMPATIBLE_SCHEME orders_aggr.price") !=
                        std::string::npos,
                    "missing clear scheme diagnostic");
            require(!incompatible.stop(), "stop incompatible");
            flag(mutation, false);
        }
        ::dlclose(dso);
        test::remove_tree(root);
        std::cout << "CGate persistent session recovery PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
