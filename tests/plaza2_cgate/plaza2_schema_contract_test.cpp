#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "fixtures/server_schema.hpp"
#include "fixtures/server_schema_912.hpp"
#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"

#include <algorithm>
#include <array>
#include <iostream>

using namespace moex::plaza2;
using namespace cgate;
using test::require;
struct IgnoreRows final : Plaza2ListenerEventHandler {
    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent&) override {
        return {};
    }
};

// Exercise the negotiated CG_MSG_OPEN descriptor and real row-byte decoder before
// projecting the committed terms. 9.12 changes offsets as well as removing a field.
struct RefdataProjection final : Plaza2ListenerEventHandler {
    private_state::Plaza2PrivateStateProjector projector;
    Plaza2PrivateStateBridge bridge{projector};
    bool opened{}, online{}, session_row{}, uncommitted_hidden{};
    std::size_t row_size{};

    RefdataProjection() {
        const std::array streams{generated::StreamCode::kFortsRefdataRepl};
        require(!bridge.reset(streams) && !bridge.begin_run(), "reset REFDATA bridge");
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        opened |= event.kind == Plaza2ListenerEventKind::Open;
        online |= event.kind == Plaza2ListenerEventKind::Online;
        const auto result = bridge.on_plaza2_listener_event(event);
        if (event.kind == Plaza2ListenerEventKind::StreamData &&
            event.table_code == generated::TableCode::kFortsRefdataReplFutSessContents) {
            session_row = true;
            row_size = event.raw_payload.size();
            uncommitted_hidden = !projector.find_future_session_terms(1001).has_value();
            require(std::none_of(event.fields.begin(), event.fields.end(),
                                 [](const auto& field) {
                                     return field.field_code == static_cast<generated::FieldCode>(0x8453894Eu);
                                 }),
                    "unused settlement_price_open was decoded as a consumed field");
        }
        return result;
    }
};

static void require_refdata_projection(Plaza2Connection& connection, const test::fake::Control& fake, bool spectra912,
                                       generated::FieldCode unused_mutation = {}, std::string_view unused_type = {}) {
    test::fake::Scenario scenario;
    scenario.spectra912_refdata_schema = spectra912;
    scenario.mutated_schema_field = unused_mutation;
    scenario.omit_schema_field = unused_mutation != generated::FieldCode{} && unused_type.empty();
    scenario.schema_field_type = unused_type;
    fake.configure(scenario);
    RefdataProjection handler;
    Plaza2Listener listener;
    require(
        !listener.create(connection, generated::StreamCode::kFortsRefdataRepl, "p2repl://FORTS_REFDATA_REPL", &handler),
        "create versioned REFDATA listener");
    require(!listener.open("mode=snapshot+online"), "open versioned REFDATA listener");
    for (int i = 0; i < 8 && !listener.last_callback_error(); ++i)
        require(!connection.process(0), "process versioned REFDATA");
    require(!listener.last_callback_error(),
            "versioned REFDATA OPEN/decode failed: " + listener.last_callback_error().message);
    require(handler.opened && handler.online && handler.session_row && handler.uncommitted_hidden,
            "versioned REFDATA must OPEN, reach ONLINE and project only after commit");
    require(handler.row_size == (spectra912 ? test::kSpectra912FutureSessionRowSize : 540),
            "versioned REFDATA row length differs from vendor layout");
    const auto terms = handler.projector.find_future_session_terms(1001);
    const auto dec = private_state::parse_session_decimal;
    require(terms && terms->sess_id == 321 && terms->repl_id == 3101 && terms->source.repl_rev == 3 &&
                terms->settlement_price == dec("102500") && terms->raw_limit_up == dec("10000") &&
                terms->raw_limit_down == dec("10000") && terms->min_step == dec("250") &&
                terms->bounds.interval_valid && terms->bounds.lower == dec("92500") &&
                terms->bounds.upper == dec("112500"),
            "versioned REFDATA changed exact settlement/limits/grid or committed price bounds");
    const auto instruments = handler.projector.instruments();
    const auto instrument =
        std::find_if(instruments.begin(), instruments.end(), [](const auto& value) { return value.isin_id == 1001; });
    require(instrument != instruments.end() && instrument->state == 1,
            "reordered 9.12 state field was not decoded at its negotiated offset");
}

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
        const auto legacy = test::server_schema_fields(generated::TableCode::kFortsRefdataReplFutSessContents);
        const auto updated = test::spectra912_future_session_schema();
        const auto named = [](auto fields, std::string_view name) -> const test::ServerSchemaField& {
            const auto found =
                std::find_if(fields.begin(), fields.end(), [&](const auto& field) { return field.field_name == name; });
            require(found != fields.end(), "vendor fixture field absent: " + std::string(name));
            return *found;
        };
        require(legacy.size() == 51 && updated.size() == 48 &&
                    named(legacy, "settlement_price_open").native_offset == 212 &&
                    std::none_of(updated.begin(), updated.end(),
                                 [](const auto& field) { return field.field_name == "settlement_price_open"; }) &&
                    named(legacy, "settlement_price").native_offset == 303 &&
                    named(updated, "settlement_price").native_offset == 212 &&
                    named(updated, "xlot_volume").type_token == "d16.8" &&
                    named(legacy, "state").native_offset == 324 && named(updated, "state").native_offset == 512,
                "9.9/9.12 fixtures must retain their actual independent vendor layouts");
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
        // The actual 9.12 shape is intentionally tested first: the old metadata
        // rejects it at OPEN on the removed settlement_price_open field.
        require_refdata_projection(connection, fake, true);
        require_refdata_projection(connection, fake, false);
        const auto obsolete = static_cast<generated::FieldCode>(0x8453894Eu);
        require_refdata_projection(connection, fake, false, obsolete);
        require_refdata_projection(connection, fake, false, obsolete, "invalid_type");
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
                for (const bool spectra912 : {false, true}) {
                    if (spectra912 && table.table_code != generated::TableCode::kFortsRefdataReplFutSessContents)
                        continue;
                    for (const bool missing : {true, false}) {
                        test::fake::Scenario scenario;
                        scenario.spectra912_refdata_schema = spectra912;
                        scenario.mutated_schema_field = field.field_code;
                        scenario.omit_schema_field = missing;
                        scenario.schema_field_type = missing ? "" : "invalid_type";
                        fake.configure(scenario);
                        Plaza2Listener listener;
                        require(!listener.create(connection, stream, "p2repl://" + std::string(table.stream_name),
                                                 &handler),
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
