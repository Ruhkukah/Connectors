#include "plaza2_runtime_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace generated = moex::plaza2::generated;
using namespace moex::plaza2::cgate;
using moex::plaza2::test::require;

struct CapturedText {
    generated::FieldCode code;
    std::string text;
    std::vector<std::byte> raw;
};

struct Capture final : Plaza2ListenerEventHandler {
    std::vector<CapturedText> values;

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        for (const auto& field : event.fields) {
            if (field.kind != Plaza2DecodedValueKind::String)
                continue;
            require(text::valid_utf8(field.text_value), "callback text must be valid UTF-8");
            require(!field.raw_value.empty(), "callback must preserve fixed-string raw_value");
            require(std::search(event.raw_payload.begin(), event.raw_payload.end(), field.raw_value.begin(),
                                field.raw_value.end()) != event.raw_payload.end(),
                    "raw_value must occur unchanged in callback raw_payload");
            // Both runtime views expire after the callback: retain independent copies.
            values.push_back(
                {field.field_code, std::string(field.text_value), {field.raw_value.begin(), field.raw_value.end()}});
        }
        return {};
    }

    void check(generated::FieldCode code, std::string_view expected_text, std::string_view expected_raw) const {
        const auto found =
            std::find_if(values.begin(), values.end(), [&](const auto& value) { return value.code == code; });
        require(found != values.end(), "expected text field was not delivered through callback");
        require(found->text == expected_text, "callback decoded text mismatch");
        require(found->raw.size() > expected_raw.size(), "fixed raw field must include its terminator");
        std::vector<std::byte> exact(found->raw.size(), std::byte{0});
        for (std::size_t i = 0; i < expected_raw.size(); ++i)
            exact[i] = static_cast<std::byte>(static_cast<unsigned char>(expected_raw[i]));
        require(found->raw == exact, "raw_value differs from exact CP1251 bytes and zero padding");
    }
};

void run(const moex::plaza2::test::RuntimeFixturePaths& fixture, bool cp1251) {
    using enum generated::FieldCode;
    if (cp1251)
        require(::setenv("MOEX_FAKE_CP1251_TEXT", "1", 1) == 0, "set text fixture flag");
    else
        require(::unsetenv("MOEX_FAKE_CP1251_TEXT") == 0, "clear text fixture flag");
    Plaza2Settings settings;
    settings.environment = Plaza2Environment::Test;
    settings.runtime_root = fixture.root;
    settings.env_open_settings = "ini=config/t1.ini;key=00000000";
    Plaza2Env env;
    require(!env.open(settings), "open fake environment");
    Plaza2Connection connection;
    require(!connection.create(env, "p2tcp://127.0.0.1:4001;app_name=text_fixture"), "create fake connection");
    require(!connection.open({}), "open fake connection");
    Capture capture;
    Plaza2Listener refdata;
    Plaza2Listener aggr;
    require(
        !refdata.create(connection, generated::StreamCode::kFortsRefdataRepl, "p2repl://FORTS_REFDATA_REPL", &capture),
        "create REFDATA listener");
    require(!aggr.create(connection, generated::StreamCode::kFortsAggrRepl, "p2repl://FORTS_AGGR20_REPL", &capture),
            "create AGGR listener");
    require(!refdata.open("mode=snapshot+online") && !aggr.open("mode=snapshot+online"), "open listeners");
    for (int i = 0; i < 8; ++i)
        require(!connection.process(0), "process fake callbacks");
    capture.check(kFortsRefdataReplFutInstrumentsIsin, "RTS-6.26", "RTS-6.26");
    if (cp1251) {
        const auto expected = u8"Фьючерсный контракт ALRS-12.26";
        capture.check(kFortsRefdataReplFutInstrumentsName, reinterpret_cast<const char*>(expected),
                      "\xd4\xfc\xfe\xf7\xe5\xf0\xf1\xed\xfb\xe9 "
                      "\xea\xee\xed\xf2\xf0\xe0\xea\xf2 ALRS-12.26");
        capture.check(kFortsAggrReplSysEventsMessage, reinterpret_cast<const char*>(u8"Сессия"),
                      "\xd1\xe5\xf1\xf1\xe8\xff");
    } else {
        capture.check(kFortsAggrReplSysEventsMessage, "session_data_ready", "session_data_ready");
    }
}
} // namespace

int main(int argc, char** argv) {
    using namespace moex::plaza2::test;
    require(argc == 2, "expected fake CGate library path");
    const auto root = make_temp_directory("plaza2_runtime_listener_text");
    try {
        const auto fixture = materialize_runtime_fixture(
            root, argv[1], Plaza2Environment::Test, build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test"));
        run(fixture, false);
        run(fixture, true);
        run(fixture, false);
        remove_tree(root);
        return 0;
    } catch (const std::exception& error) {
        ::unsetenv("MOEX_FAKE_CP1251_TEXT");
        remove_tree(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
