#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"

#include <iostream>

using namespace moex::plaza2;
using namespace cgate;
using test::require;
struct IgnoreRows final : Plaza2ListenerEventHandler {
    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent&) override {
        return {};
    }
};

int main(int argc, char** argv) {
    try {
        require(argc == 2, "fake runtime path required");
        const auto root = test::make_temp_directory("schema-contract");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA 9.9.0", "9.9", "T1"));
        test::fake::Control fake(fixture.library_path);
        Plaza2Settings settings;
        settings.runtime_root = fixture.root;
        settings.env_open_settings = "ini=config/t1.ini;key=00000000";
        Plaza2Env env;
        require(!env.open(settings), "open environment");
        Plaza2Connection connection;
        require(!connection.create(env, "p2tcp://127.0.0.1:4101;app_name=schema_contract"), "create connection");
        require(!connection.open({}), "open connection");
        IgnoreRows handler;
        std::size_t checked{};
        // The retained descriptors are the decoder's consumed field contract, not the full server scheme.
        // Exercise every one: this also catches new consumed fields omitted from validation later.
        for (const auto& table : generated::TableDescriptors()) {
            const auto stream = static_cast<generated::StreamCode>(table.stream_id);
            if (stream == generated::StreamCode::kFortsDealsRepl)
                continue; // Public DEALS qualification is deferred.
            for (const auto& field : generated::FieldsForTable(table.table_code)) {
                for (const bool missing : {true, false}) {
                    test::fake::Scenario scenario;
                    scenario.mutated_schema_field = field.field_code;
                    scenario.omit_schema_field = missing;
                    scenario.schema_field_type = missing ? "" : "invalid_type";
                    fake.configure(scenario);
                    Plaza2Listener listener;
                    require(
                        !listener.create(connection, stream, "p2repl://" + std::string(table.stream_name), &handler),
                        "create listener");
                    const auto opened = listener.open("mode=snapshot+online");
                    for (int i = 0; i < 8 && !listener.last_callback_error(); ++i)
                        (void)connection.process(0);
                    const auto& error = listener.last_callback_error();
                    const auto label = std::string(table.table_name) + "." + std::string(field.field_name);
                    require((opened.code == Plaza2ErrorCode::IncompatibleScheme ||
                             error.code == Plaza2ErrorCode::IncompatibleScheme) &&
                                (opened.message + error.message).find(label) != std::string::npos,
                            (missing ? "missing consumed field accepted: " : "retyped consumed field accepted: ") +
                                label);
                    ++checked;
                }
            }
        }
        fake.configure({});
        std::cout << "validated " << checked << " missing/retyped field cases\n";
        test::remove_tree(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
