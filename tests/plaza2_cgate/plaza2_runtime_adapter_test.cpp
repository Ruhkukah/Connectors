#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct ReplyCapture final : moex::plaza2::cgate::Plaza2ListenerEventHandler {
    struct Captured {
        moex::plaza2::cgate::Plaza2ListenerEventKind kind;
        std::int32_t message_id;
        std::string message_name;
        std::uint32_t user_id;
        std::vector<std::byte> payload;
    };

    moex::plaza2::cgate::Plaza2Error
    on_plaza2_listener_event(const moex::plaza2::cgate::Plaza2ListenerEvent& event) override {
        if ((event.kind == moex::plaza2::cgate::Plaza2ListenerEventKind::StreamData ||
             event.kind == moex::plaza2::cgate::Plaza2ListenerEventKind::Timeout) &&
            event.stream_code == moex::plaza2::cgate::kNoStreamCode) {
            events.push_back({
                .kind = event.kind,
                .message_id = event.message_id,
                .message_name = std::string(event.message_name),
                .user_id = event.user_id,
                .payload = std::vector<std::byte>(event.raw_payload.begin(), event.raw_payload.end()),
            });
        }
        return {};
    }

    std::vector<Captured> events;
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open reviewed ABI lock: " + path.string());
    }
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "expected fake runtime library path\n";
            return 1;
        }

        using namespace moex::plaza2::cgate;
        using namespace moex::plaza2::test;

        const auto fake_library = std::filesystem::path(argv[1]);
        const auto fixture_root = make_temp_directory("plaza2_runtime_adapter_test");
        const auto cleanup = [&]() { remove_tree(fixture_root); };

        const auto scheme_text = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture =
            materialize_runtime_fixture(fixture_root, fake_library, Plaza2Environment::Test, scheme_text);

        Plaza2Settings settings;
        settings.environment = Plaza2Environment::Test;
        settings.runtime_root = fixture.root;
        settings.env_open_settings = "ini=config/t1.ini;key=00000000";

        Plaza2Env env;
        require(!env.open(settings), "environment open should succeed");

        Plaza2Connection connection;
        const auto app_name = make_plaza2_application_name("Connectors", "phase3c", 7);
        require(app_name == "connectors_phase3c_7", "application name should be deterministic and sanitized");
        require(!connection.create(env, "p2tcp://127.0.0.1:4001;app_name=" + app_name),
                "connection create should succeed");
        require(!connection.open({}), "connection open should succeed");

        std::uint32_t connection_state = 0;
        require(!connection.state(connection_state) && connection_state == 3, "fake connection should become active");

        std::uint32_t process_code = 0;
        require(!connection.process(0, &process_code), "timeout process should not be treated as error");
        require(process_code == 131075, "fake runtime should report CG_ERR_TIMEOUT from process");

        Plaza2Listener listener;
        require(!listener.create(connection, "p2repl://FORTS_TRADE_REPL;scheme=|FILE|scheme/forts_scheme.ini|TRADES"),
                "listener create should succeed");

        const auto abi_lock = read_text(std::filesystem::path(MOEX_SOURCE_ROOT) / "spec-lock" / "test" / "plaza2" /
                                        "cgate99" / "abi_x86_64.json");
        require(abi_lock.find("\"CG_STATE_CLOSED\": 0") != std::string::npos &&
                    abi_lock.find("\"CG_STATE_ERROR\": 1") != std::string::npos &&
                    abi_lock.find("\"CG_STATE_OPENING\": 2") != std::string::npos &&
                    abi_lock.find("\"CG_STATE_ACTIVE\": 3") != std::string::npos,
                "reviewed CGate 9.9 ABI state lock should retain the expected raw values");

        std::uint32_t listener_state = 99;
        require(!listener.state(listener_state) && listener_state == 0,
                "new fake listener should expose CG_STATE_CLOSED=0");

        ::setenv("MOEX_FAKE_LSN_OPENING_STATE", "1", 1);
        require(!listener.open({}), "listener open should succeed");
        ::unsetenv("MOEX_FAKE_LSN_OPENING_STATE");
        require(!listener.state(listener_state) && listener_state == 2,
                "opening fake listener should expose CG_STATE_OPENING=2");
        require(!listener.close(), "opening listener close should succeed");

        ::setenv("MOEX_FAKE_LSN_ERROR_STATE", "1", 1);
        require(!listener.open({}), "forced-error listener open should return its modeled state");
        ::unsetenv("MOEX_FAKE_LSN_ERROR_STATE");
        require(!listener.state(listener_state) && listener_state == 1,
                "forced-error fake listener should expose CG_STATE_ERROR=1");
        require(!listener.close(), "forced-error listener close should succeed");

        require(!listener.open({}), "listener active open should succeed");

        require(!listener.state(listener_state) && listener_state == 3,
                "active fake listener should expose CG_STATE_ACTIVE=3");

        const auto publisher_timeout = translate_plaza2_result("cg_pub_post", 131075);
        require(publisher_timeout && publisher_timeout.code == Plaza2ErrorCode::RuntimeCallFailed,
                "CG_ERR_TIMEOUT should be an error for publisher operations");

        Plaza2Publisher publisher;
        require(!publisher.create(connection, "p2mq://PUB;category=FORTS_MSG"), "fake publisher create should succeed");
        require(!publisher.open({}), "fake publisher open should succeed");
        ReplyCapture reply_capture;
        Plaza2Listener reply_listener;
        require(!reply_listener.create(connection, kNoStreamCode, "p2mqreply://;ref=PUB", &reply_capture),
                "untyped reply listener create should succeed");
        require(!reply_listener.open({}), "untyped reply listener open should succeed");
        const std::array<std::byte, 128> add_payload{};
        const std::array<std::byte, 28> del_payload{};

        ::setenv("MOEX_FAKE_PUB_MSGNEW_RESULT", "internal", 1);
        const auto allocation_failure = publisher.post_by_message_name("AddOrder", add_payload, 701, true);
        require(allocation_failure.certainty == Plaza2SubmissionCertainty::DefinitelyNotSent &&
                    allocation_failure.allocation_error && !allocation_failure.post_invoked,
                "message allocation failure should be definitely not sent");
        ::unsetenv("MOEX_FAKE_PUB_MSGNEW_RESULT");

        ::setenv("MOEX_FAKE_PUB_POST_RESULT", "timeout", 1);
        const auto ambiguous = publisher.post_by_message_name("AddOrder", add_payload, 702, true);
        require(ambiguous.certainty == Plaza2SubmissionCertainty::PossiblySent && ambiguous.post_error,
                "publisher timeout should preserve possible-submission certainty");
        ::unsetenv("MOEX_FAKE_PUB_POST_RESULT");

        ::setenv("MOEX_FAKE_PUB_MSGFREE_RESULT", "internal", 1);
        const auto posted_free_failure = publisher.post_by_message_name("AddOrder", add_payload, 703, true);
        require(posted_free_failure.certainty == Plaza2SubmissionCertainty::Posted && posted_free_failure.free_error &&
                    !posted_free_failure.post_error,
                "message-free failure must not erase a successful post");
        ::unsetenv("MOEX_FAKE_PUB_MSGFREE_RESULT");

        ::setenv("MOEX_FAKE_PUB_REPLY_MODE", "timeout", 1);
        const auto add_timeout = publisher.post_by_message_name("AddOrder", add_payload, 704, true);
        const auto del_timeout = publisher.post_by_message_name("DelOrder", del_payload, 705, true);
        require(add_timeout.certainty == Plaza2SubmissionCertainty::Posted &&
                    del_timeout.certainty == Plaza2SubmissionCertainty::Posted,
                "timeout fixtures should model successfully posted commands awaiting replies");
        ::unsetenv("MOEX_FAKE_PUB_REPLY_MODE");

        std::uint32_t reply_process_code = 0;
        const auto reply_process_error = connection.process(0, &reply_process_code);
        require(!reply_process_error,
                "posted reply should be delivered through cg_conn_process: " + reply_process_error.message + " (" +
                    std::to_string(reply_process_code) + ")");
        require(reply_capture.events.size() == 3, "ordinary DATA and both P2MQ timeout events should be delivered");
        require(reply_capture.events[0].kind == Plaza2ListenerEventKind::StreamData &&
                    reply_capture.events[0].user_id == 703 && reply_capture.events[0].message_id == 179 &&
                    reply_capture.events[0].message_name == "AddOrderReply" &&
                    reply_capture.events[0].payload.size() == 268,
                "reply listener must expose the exact publisher user_id and raw reply payload");
        require(reply_capture.events[1].kind == Plaza2ListenerEventKind::Timeout &&
                    reply_capture.events[1].user_id == 704 && reply_capture.events[1].message_id == 0 &&
                    reply_capture.events[1].message_name.empty() && reply_capture.events[1].payload.empty(),
                "CG_MSG_P2MQ_TIMEOUT must retain only the originating AddOrder user_id");
        require(reply_capture.events[2].kind == Plaza2ListenerEventKind::Timeout &&
                    reply_capture.events[2].user_id == 705,
                "CG_MSG_P2MQ_TIMEOUT must retain the originating DelOrder user_id");

        const auto reopen_error = connection.open({});
        require(reopen_error && reopen_error.code == Plaza2ErrorCode::AdapterState,
                "reopening an active connection should translate to adapter-state error");

        require(!publisher.close(), "publisher close should succeed");
        require(!publisher.destroy(), "publisher destroy should succeed");
        require(!reply_listener.close(), "reply listener close should succeed");
        require(!reply_listener.destroy(), "reply listener destroy should succeed");

        require(!listener.close(), "listener close should succeed");
        require(!listener.destroy(), "listener destroy should succeed");
        require(!connection.close(), "connection close should succeed");
        require(!connection.destroy(), "connection destroy should succeed");
        require(!env.close(), "environment close should succeed");

        cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
