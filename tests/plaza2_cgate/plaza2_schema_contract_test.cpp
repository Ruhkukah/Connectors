#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "fixtures/server_schema.hpp"

#include <algorithm>
#include <iostream>

using namespace moex::plaza2;
using namespace cgate;
using test::require;
struct IgnoreRows final : Plaza2ListenerEventHandler {
    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent&) override {
        return {};
    }
};

static std::size_t require_captured_schema_subset() {
    const auto streams = test::captured_server_streams();
    require(streams.size() == 8, "native schema fixture must identify all eight captured product streams");
    for (const auto stream :
         {generated::StreamCode::kFortsTradeRepl, generated::StreamCode::kFortsUserorderbookRepl,
          generated::StreamCode::kFortsPosRepl, generated::StreamCode::kFortsPartRepl,
          generated::StreamCode::kFortsRefdataRepl, generated::StreamCode::kFortsAggrRepl,
          generated::StreamCode::kFortsSessionstateRepl, generated::StreamCode::kFortsInstrumentstateRepl})
        require(std::count(streams.begin(), streams.end(), stream) == 1,
                "native schema fixture missing or duplicating a captured product stream");
    const auto tables = test::captured_server_tables();
    const auto require_table = [&](generated::TableCode table) {
        const auto* descriptor = generated::FindTableByCode(table);
        require(std::count(tables.begin(), tables.end(), table) == 1,
                "required table not present in native capture: " + std::string(descriptor->stream_name) + "." +
                    std::string(descriptor->table_name));
    };
    const auto session = test::server_schema_fields(generated::TableCode::kFortsRefdataReplSession);
    require(!session.empty(), "captured CGate 9.9 REFDATA session table missing");
    for (const auto obsolete : {"inter_cl_begin", "inter_cl_end", "inter_cl_state"})
        require(std::none_of(session.begin(), session.end(),
                             [&](const auto& field) { return field.field_name == obsolete; }),
                "captured CGate 9.9 session fixture contains obsolete field: " + std::string(obsolete));
    // PART.part is required at OPEN even though its rows are not projected.
    require_table(generated::TableCode::kFortsPartReplPart);
    require(!test::server_schema_fields(generated::TableCode::kFortsPartReplPart).empty(),
            "captured CGate 9.9 required PART part table missing");
    std::size_t checked{};
    for (const auto& table : generated::TableDescriptors()) {
        if (table.stream_id == static_cast<std::uint32_t>(generated::StreamCode::kFortsDealsRepl))
            continue; // Public DEALS qualification is deferred.
        const auto required = generated::FieldsForTable(table.table_code);
        if (required.empty())
            continue; // Ignored server tables do not add required fields.
        require_table(table.table_code);
        const auto captured = test::server_schema_fields(table.table_code);
        require(!captured.empty(), "required table absent from captured CGate 9.9 schema: " +
                                       std::string(table.stream_name) + "." + std::string(table.table_name));
        for (const auto& field : required) {
            const auto label = std::string(table.stream_name) + "." + std::string(table.table_name) + "." +
                               std::string(field.field_name);
            const auto actual = std::find_if(captured.begin(), captured.end(), [&](const auto& candidate) {
                return candidate.field_name == field.field_name;
            });
            require(actual != captured.end(), "required field absent from captured CGate 9.9 schema: " + label);
            require(std::count_if(captured.begin(), captured.end(),
                                  [&](const auto& candidate) { return candidate.field_name == field.field_name; }) ==
                            1 &&
                        actual->field_code == field.field_code && actual->type_token == field.type_token,
                    "required field incompatible with captured CGate 9.9 schema: " + label);
            ++checked;
        }
    }
    return checked;
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "fake runtime path required");
        const auto captured_fields = require_captured_schema_subset();
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
        // These native tables do not participate in own regular-futures
        // state. Their server fields must not become entry requirements.
        for (const auto table :
             {generated::TableCode::kFortsTradeReplMultilegOrdersLog,
              generated::TableCode::kFortsTradeReplUserMultilegDeal,
              generated::TableCode::kFortsUserorderbookReplMultilegOrders,
              generated::TableCode::kFortsUserorderbookReplOrdersCurrentday,
              generated::TableCode::kFortsUserorderbookReplMultilegOrdersCurrentday,
              generated::TableCode::kFortsUserorderbookReplInfoCurrentday,
              generated::TableCode::kFortsPosReplPositionSa, generated::TableCode::kFortsPartReplPart,
              generated::TableCode::kFortsPartReplPartSa, generated::TableCode::kFortsRefdataReplOptSessContents,
              generated::TableCode::kFortsRefdataReplMultilegDict,
              generated::TableCode::kFortsRefdataReplInstr2matchingMap}) {
            const auto* descriptor = generated::FindTableByCode(table);
            const auto stream = static_cast<generated::StreamCode>(descriptor->stream_id);
            const auto server = test::server_schema_fields(table);
            require(server.size() > 3, "unused native schema fixture missing");
            for (const bool missing : {true, false}) {
                test::fake::Scenario scenario;
                scenario.mutated_schema_field = server[3].field_code;
                scenario.omit_schema_field = missing;
                scenario.schema_field_type = missing ? "" : "invalid_type";
                fake.configure(scenario);
                Plaza2Listener listener;
                require(
                    !listener.create(connection, stream, "p2repl://" + std::string(descriptor->stream_name), &handler),
                    "create listener with unrelated native table");
                const auto opened = listener.open("mode=snapshot+online");
                for (int i = 0; i < 8 && !listener.last_callback_error(); ++i)
                    (void)connection.process(0);
                require(!opened && !listener.last_callback_error(),
                        "unused native field incorrectly became a schema requirement: " +
                            std::string(descriptor->table_name));
                ++checked;
            }
        }
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
        std::cout << "validated " << captured_fields << " captured fields and " << checked
                  << " missing/retyped field cases\n";
        test::remove_tree(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
