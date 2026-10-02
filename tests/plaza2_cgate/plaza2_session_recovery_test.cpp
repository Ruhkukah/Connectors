#include "moex/plaza2_trade/cgate_session.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "plaza2_trade_test_support.hpp"
#include "fixtures/cgate99_messages.hpp"
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
using namespace moex::plaza2_trade;
using namespace moex::plaza2;
using test::require;
int main(int argc, char** argv) {
    try {
        require(argc == 2, "fake runtime path required");
        const auto root = test::make_temp_directory("cgate-recovery");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cgate::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        moex::plaza2::test::fake::Control fake(fixture.library_path);
        const auto flag = [&](moex::plaza2::test::fake::Option key, bool on) { fake.set(key, on ? "1" : ""); };
        void* dso = ::dlopen(fixture.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(dso, "load fixture counters");
        auto reset = reinterpret_cast<void (*)()>(::dlsym(dso, "moex_fake_reset_publisher_counts"));
        reset();
        auto envs = reinterpret_cast<std::uint64_t (*)()>(::dlsym(dso, "moex_fake_environment_open_count"));
        auto conns = reinterpret_cast<std::uint64_t (*)()>(::dlsym(dso, "moex_fake_connection_new_count"));
        auto now = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
        CgateSessionConfig config;
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
        for (const bool read_only : {false, true}) {
            auto live_config = config;
            live_config.mode = CgateSessionMode::Live;
            live_config.read_only_market_data = read_only;
            if (read_only) {
                live_config.allow_orders = false;
                live_config.publisher_settings.clear();
            }
            test::write_text_file(fixture.config_dir / "t1.ini", "[cgate]\n# log=p2:p2syslog\n[p2syslog]\n");
            CgateSession invalid_logging(live_config);
            require(invalid_logging.start().code == cgate::Plaza2ErrorCode::InvalidConfiguration,
                    "Live session accepted disabled native logging");
            require(envs() == 0, "Live logging validation ran after opening native environment");
            test::write_text_file(fixture.config_dir / "t1.ini", "[cgate]\nlog=p2:p2syslog\n[p2syslog]\n");
            CgateSession valid_logging(live_config);
            require(!valid_logging.start(), "Live session rejected valid native logging");
            require(!valid_logging.stop(), "Live logging session stop");
            reset();
        }
        flag(moex::plaza2::test::fake::Option::ConnHoldOpening, true);
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
        flag(moex::plaza2::test::fake::Option::ConnHoldOpening, false);
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
        auto request = test_support::make_add_order();
        request.isin_id = 1001;
        auto command = Plaza2TradeCodec{}.encode(request);
        require(session.post_command(command, 101).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "order blocked after delayed ACTIVE");
        flag(moex::plaza2::test::fake::Option::ConnToOpening, true);
        pump();
        for (int i = 0; i < 300; ++i) {
            now += std::chrono::seconds(1);
            require(!session.poll(), "5 minute outage failed");
        }
        flag(moex::plaza2::test::fake::Option::ConnToOpening, false);
        pump();
        if (!session.runtime_health().private_active) {
            std::cerr << "resume callback: " << session.last_callback_error() << "\n";
            for (auto& health : session.private_state().stream_health())
                std::cerr << health.stream_name << " " << health.online << " " << health.snapshot_complete << "\n";
        }
        require(session.runtime_health().private_active, "private streams did not resume after ACTIVE to OPENING");
        flag(moex::plaza2::test::fake::Option::ConnectionError, true);
        require(!session.poll(), "asynchronous connection ERROR terminal");
        flag(moex::plaza2::test::fake::Option::ConnectionError, false);
        pump();
        require(session.runtime_health().private_active, "asynchronous connection ERROR did not recover");
        flag(moex::plaza2::test::fake::Option::ConnDirectClosed, true);
        require(!session.poll(), "direct connection CLOSED terminal");
        require(!session.runtime_health().private_active, "direct connection CLOSED retained old private ONLINE");
        require(session.post_command(command, 102).certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent,
                "direct connection CLOSED allowed command");
        flag(moex::plaza2::test::fake::Option::ConnDirectClosed, false);
        pump();
        require(session.runtime_health().private_active, "direct connection CLOSED did not rebuild snapshots");
        require(envs() == 1 && conns() == 1, "recovery recreated environment or connection");
        flag(moex::plaza2::test::fake::Option::CallbackCorruption, true);
        require(!session.poll(), "malformed private row terminal");
        flag(moex::plaza2::test::fake::Option::CallbackCorruption, false);
        pump();
        require(session.runtime_health().private_active, "private callback anomaly did not recover");
        // Idle liveness has no age or connection bootstrap deadline.
        for (int i = 0; i < 300; ++i) {
            now += std::chrono::seconds(1);
            require(!session.poll(), "idle poll failed");
        }
        require(session.runtime_health().private_active, "quiet private streams became stale");
        // An always-readable runtime must yield to the owning command/kill path.
        fake.configure({.continuous_input = true});
        const auto calls = fake.process_count();
        const auto busy_start = std::chrono::steady_clock::now();
        require(!session.poll(), "continuous flow poll failed");
        require(std::chrono::steady_clock::now() - busy_start < std::chrono::milliseconds(100),
                "continuous flow starved owner loop");
        require(fake.process_count() - calls <= 100, "continuous flow exceeded drain call budget");
        auto cancel = Plaza2TradeCodec{}.encode(test_support::make_del_order());
        require(session.post_command(cancel, 103).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "continuous flow starved risk-reduction command");
        fake.configure({});

        require(!session.stop(), "stop");
        flag(moex::plaza2::test::fake::Option::SchemeExtraTable, true);
        flag(moex::plaza2::test::fake::Option::CgateClearDeletedInsideTransaction, true);
        flag(moex::plaza2::test::fake::Option::TimestampMilliseconds, true);
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
        flag(moex::plaza2::test::fake::Option::CgateClearDeletedInsideTransaction, false);
        flag(moex::plaza2::test::fake::Option::TimestampMilliseconds, false);
        config.listener_event_log = {};
        flag(moex::plaza2::test::fake::Option::SchemeExtraTable, false);
        for (auto mutation : {moex::plaza2::test::fake::Option::SchemeMissingPrice,
                              moex::plaza2::test::fake::Option::SchemeRetypePrice}) {
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
            const auto initial_opens = fake.opens(generated::StreamCode::kFortsAggrRepl);
            for (int i = 0; i < 600; ++i) {
                now += std::chrono::seconds(1);
                require(!incompatible.poll(), "latched scheme pump");
            }
            require(fake.opens(generated::StreamCode::kFortsAggrRepl) == initial_opens,
                    "incompatible scheme reopened without new connection generation");
            flag(moex::plaza2::test::fake::Option::ConnectionError, true);
            require(!incompatible.poll(), "latched listener reconnect");
            flag(moex::plaza2::test::fake::Option::ConnectionError, false);
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(!incompatible.poll(), "latched listener reconnect pump");
            }
            require(fake.opens(generated::StreamCode::kFortsAggrRepl) == initial_opens + 1,
                    "connection restart did not recheck latched scheme exactly once");
            require(!incompatible.stop(), "stop incompatible");
            flag(mutation, false);
        }
        test::fake::Scenario unrecognized_schema;
        unrecognized_schema.unrecognized_schema_stream = generated::StreamCode::kFortsAggrRepl;
        fake.configure(unrecognized_schema);
        CgateSession missing_tables(config);
        require(!missing_tables.start(), "unknown AGGR tables session start");
        for (int i = 0; i < 6; ++i) {
            now += std::chrono::seconds(1);
            require(!missing_tables.poll(), "unknown AGGR tables bootstrap");
        }
        require(missing_tables.runtime_health().private_active, "unknown AGGR tables affected private streams");
        const auto missing_table_opens = fake.opens(generated::StreamCode::kFortsAggrRepl);
        for (int i = 0; i < 600; ++i) {
            now += std::chrono::seconds(1);
            require(!missing_tables.poll(), "unknown AGGR tables pump");
        }
        require(fake.opens(generated::StreamCode::kFortsAggrRepl) == missing_table_opens,
                "unrecognized required tables reopened repeatedly");
        require(!missing_tables.stop(), "unknown AGGR tables stop");
        fake.configure({});
        // A terminal create-time configuration error is never retried.
        for (bool listener : {true, false}) {
            test::fake::Scenario invalid_configuration;
            (listener ? invalid_configuration.listener_create_result : invalid_configuration.publisher_create_result) =
                131073;
            fake.configure(invalid_configuration);
            CgateSession invalid_session(config);
            require(!invalid_session.start(), "configured create errors arise on poll");
            require(static_cast<bool>(invalid_session.poll()), "create INVALIDARGUMENT retried as recoverable");
            require(invalid_session.recovery_status().operation == Plaza2SessionOperation::Failed,
                    "create INVALIDARGUMENT did not latch Failed");
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(static_cast<bool>(invalid_session.poll()), "terminal create error was cleared");
            }
            require(!invalid_session.stop(), "invalid session stop");
            fake.configure({});
        }
        config.publisher_messages_per_second = 1;
        CgateSession limited(config);
        require(!limited.start(), "standalone rate session start");
        for (int i = 0; i < 10; ++i) {
            now += std::chrono::seconds(1);
            require(!limited.poll(), "standalone rate warmup");
        }
        require(limited.post_command(command, 201).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "standalone first post");
        require(limited.post_command(command, 202).certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent,
                "standalone rate protection missing");
        require(!limited.stop(), "standalone stop");
        for (const auto owner : {PublisherRateOwner::Session, PublisherRateOwner::External}) {
            test::fake::Scenario scenario;
            scenario.suppress_auto_replies = true;
            fake.configure(scenario);
            auto late_config = config;
            late_config.publisher_rate_owner = owner;
            late_config.reply_timeout_ms = 10;
            CgateSession late(late_config);
            require(!late.start(), "late reply tracking session start");
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(!late.poll(), "late reply tracking warmup");
            }
            auto move = test_support::make_move_order();
            move.isin_id = 1001;
            move.regime = 3;
            move.order_id2 = 0;
            move.amount2 = 0;
            move.price1 = "103000";
            const auto encoded = Plaza2TradeCodec{}.encode(move);
            require(encoded.validation.ok(), "late Move fixture codec validation");
            require(late.post_command(encoded, 401).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    "late Move fixture post");
            now += std::chrono::milliseconds(20);
            require(!late.poll(), "late Move tracking expiry");
            require(late.take_reply_events().empty(), "tracking expiry manufactured a native timeout");
            official_cgate99::FORTS_MSG176 reply{};
            reply.order_id1 = 64001;
            std::vector<std::byte> payload(sizeof(reply));
            std::memcpy(payload.data(), &reply, payload.size());
            fake.enqueue({.kind = test::fake::EventKind::Reply, .message_id = 176, .user_id = 401, .payload = payload});
            require(!late.poll(), "expired correlation late176 pump");
            const auto replies = late.take_reply_events();
            if (owner == PublisherRateOwner::External) {
                require(replies.size() == 1 && replies.front().user_id == 401 && replies.front().message_id == 176 &&
                            replies.front().raw_payload == payload,
                        "external owner lost176 after session correlation expired");
                fake.enqueue({.kind = test::fake::EventKind::Reply,
                              .message_id = 99,
                              .user_id = 499,
                              .payload = {std::byte{1}, std::byte{2}, std::byte{3}}});
                require(!late.poll(), "unknown malformed99 pump");
                const auto malformed = late.take_reply_events();
                require(malformed.empty() && late.runtime_health().reply == 1,
                        "unknown malformed99 bypassed existing flood validation");
            } else
                require(replies.empty(), "standalone session forwarded an unknown late reply");
            require(!late.stop(), "late reply tracking session stop");
        }
        fake.configure({});
        config.publisher_rate_owner = PublisherRateOwner::External;
        CgateSession external(config);
        require(!external.start(), "external rate session start");
        for (int i = 0; i < 10; ++i) {
            now += std::chrono::seconds(1);
            require(!external.poll(), "external rate warmup");
        }
        fake.set(test::fake::Option::PubReplyFamily, "99");
        require(external.post_command(command, 301).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "external first post");
        require(external.post_command(command, 302).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "duplicate transport limiter rejected queue-admitted command");
        require(!external.poll(), "external flood reply pump");
        require(external.take_reply_events().size() == 2, "99 replies were not forwarded to external rate owner");
        require(external.publisher_rate_metrics().penalty_until_ms == 0,
                "external rate owner received duplicate transport penalty");
        fake.set(test::fake::Option::PubReplyMalformed);
        require(external.post_command(command, 303).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "transport blocked externally-owned 99 recovery queue");
        require(!external.poll(), "malformed99 pump");
        const auto malformed = external.take_reply_events();
        require(malformed.size() == 1 && malformed.front().user_id == 303 && malformed.front().raw_payload.size() == 3,
                "malformed99 lost terminal correlation event");
        fake.clear(test::fake::Option::PubReplyMalformed);
        fake.clear(test::fake::Option::PubReplyFamily);
        for (int i = 0; i < 3; ++i) {
            now += std::chrono::seconds(1);
            require(!external.poll(), "malformed99 reply listener recovery");
        }
        require(external.post_command(command, 303).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "malformed99 correlation leaked until timeout");
        // Committed halted instrument blocks Add/Move while cancel remains available.
        fake.enqueue(
            {.kind = test::fake::EventKind::Begin, .stream_code = generated::StreamCode::kFortsInstrumentstateRepl});
        fake.enqueue(
            {.kind = test::fake::EventKind::Row,
             .stream_code = generated::StreamCode::kFortsInstrumentstateRepl,
             .table_code = generated::TableCode::kFortsInstrumentstateReplInstrumentState,
             .revision = 100,
             .fields = {{.field_code = generated::FieldCode::kFortsInstrumentstateReplInstrumentStateIsinId,
                         .signed_value = 1001},
                        {.field_code = generated::FieldCode::kFortsInstrumentstateReplInstrumentStatePublicState,
                         .signed_value = 0}}});
        fake.enqueue(
            {.kind = test::fake::EventKind::Commit, .stream_code = generated::StreamCode::kFortsInstrumentstateRepl});
        require(!external.poll(), "committed instrument halt pump");
        require(external.post_command(command, 304).certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent,
                "Add ignored committed instrument halt");
        require(external.post_command(cancel, 305).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "halted instrument blocked risk reduction");
        require(!external.stop(), "external stop");
        ::dlclose(dso);
        test::remove_tree(root);
        std::cout << "CGate persistent session recovery PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
