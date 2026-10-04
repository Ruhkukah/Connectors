#include "moex/plaza2_trade/cgate_session.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include <array>
#include <iostream>
#include <limits>
using namespace moex::plaza2;
using namespace moex::plaza2_trade;
using test::require;
namespace f = test::fake;
int main(int argc, char** argv) {
    try {
        require(argc == 2, "fake runtime path required");
        const auto root = test::make_temp_directory("full-order-log-session");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cgate::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        f::Control fake(fixture.library_path);
        auto now = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
        const std::array<std::int32_t, 1> ids{1001};
        cgate::Plaza2FullOrderLog book(ids, 100, 100);
        CgateSessionConfig config;
        config.runtime.runtime_root = fixture.root;
        config.runtime.env_open_settings = "ini=config/t1.ini;key=00000000";
        config.connection_settings = "p2tcp://127.0.0.1:4101;app_name=full-order-log-test";
        config.read_only_market_data = true;
        config.process_timeout_ms = 0;
        config.recovery_now = [&] { return now; };
        config.private_streams = {
            {generated::StreamCode::kFortsRefdataRepl, "p2repl://FORTS_REFDATA_REPL", "mode=snapshot+online"}};
        config.market_data_isin_ids = {1001};
        config.full_order_log_stream = {cgate::kFullOrderLogStreamCode,
                                        "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL", ""};
        config.full_order_log_handler = &book;
        auto scenario = f::Scenario{};
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogRefdata)] = "1";
        scenario.options[static_cast<std::size_t>(f::Option::DisablePublisher)] = "1";
        scenario.options[static_cast<std::size_t>(f::Option::DisableReplyListener)] = "1";
        fake.configure(scenario);
        CgateSession session(config);
        require(!session.start(), "full order log start");
        const auto pump = [&](int n = 3) {
            for (int i = 0; i < n; ++i) {
                const auto e = session.poll(false);
                require(!e, "full order log poll: " + e.message + " callback: " + session.last_callback_error());
            }
        };
        pump();
        require(session.full_order_log_matching_id() == 3, "REFDATA matching join must resolve matching 3");
        require(book.valid() && book.order_count() == 2, "composite snapshot must publish on first online commit");
        require(book.levels(1001, true).at(10250000000LL) == 7 && book.levels(1001, false).at(10260000000LL) == 9,
                "composite snapshot must retain exact d16.5 levels");
        require(session.runtime_health().full_order_log == 3 && !session.publisher_open() && !session.p2mqreply_open(),
                "full log must run without order-entry transports");
        require(session.connection_app_name() == "full-order-log-test",
                "market data process must retain own app identity");
        const auto enqueue = [&](f::EventKind kind, std::int64_t rev = 0,
                                 generated::TableCode table = cgate::kNoTableCode, std::uint64_t value = 0) {
            fake.enqueue({.kind = kind,
                          .stream_code = cgate::kFullOrderLogStreamCode,
                          .table_code = table,
                          .revision = rev,
                          .value = value});
        };
        enqueue(f::EventKind::Begin);
        enqueue(f::EventKind::ClearDeleted, 22, generated::TableCode::kFortsUserorderbookReplOrders);
        enqueue(f::EventKind::Commit);
        pump();
        require(book.order_count() == 1 && book.levels(1001, true).empty(),
                "ClearDeleted must erase only rows below revision");
        enqueue(f::EventKind::Begin);
        enqueue(f::EventKind::ClearDeleted, std::numeric_limits<std::int64_t>::max(),
                generated::TableCode::kFortsUserorderbookReplOrders);
        enqueue(f::EventKind::Commit);
        pump();
        require(book.valid() && book.order_count() == 0, "ClearDeleted MAX must erase all rows");
        auto opens = fake.opens(cgate::kFullOrderLogStreamCode);
        enqueue(f::EventKind::ListenerError);
        pump(2);
        require(!book.valid(), "ERROR must fence the old book");
        now += std::chrono::milliseconds(999);
        pump(1);
        require(fake.opens(cgate::kFullOrderLogStreamCode) == opens, "composite reopen must wait one second");
        now += std::chrono::milliseconds(1);
        pump();
        require(fake.opens(cgate::kFullOrderLogStreamCode) == opens + 1 && book.valid() && book.order_count() == 2,
                "ERROR must reopen a fresh composite snapshot");
        require(fake.listener_open_settings(cgate::kFullOrderLogStreamCode).find("replstate") == std::string::npos,
                "p2ordbook recovery must never pass p2repl replay keys");
        // A committed single-partition remap fences the book and rebuilds it.
        fake.enqueue({.kind = f::EventKind::Begin, .stream_code = generated::StreamCode::kFortsRefdataRepl});
        fake.enqueue(
            {.kind = f::EventKind::Row,
             .stream_code = generated::StreamCode::kFortsRefdataRepl,
             .table_code = generated::TableCode::kFortsRefdataReplInstr2matchingMap,
             .revision = 40,
             .fields = {
                 {.field_code = generated::FieldCode::kFortsRefdataReplInstr2matchingMapReplId, .signed_value = 3},
                 {.field_code = generated::FieldCode::kFortsRefdataReplInstr2matchingMapReplRev, .signed_value = 40},
                 {.field_code = generated::FieldCode::kFortsRefdataReplInstr2matchingMapBaseContractId,
                  .signed_value = 500},
                 {.field_code = generated::FieldCode::kFortsRefdataReplInstr2matchingMapMatchingId,
                  .signed_value = 4}}});
        fake.enqueue({.kind = f::EventKind::Commit, .stream_code = generated::StreamCode::kFortsRefdataRepl});
        pump();
        require(session.full_order_log_matching_id() == 4 && !book.valid(),
                "matching remap must fence the old public book");
        now += std::chrono::seconds(1);
        pump();
        require(book.valid(), "single matching remap must rebuild a fresh composite snapshot");
        fake.enqueue({.kind = f::EventKind::ListenerError, .stream_code = generated::StreamCode::kFortsRefdataRepl});
        pump(2);
        require(!book.valid() && !session.full_order_log_matching_id(),
                "REFDATA authority loss must fence full log immediately");
        now += std::chrono::seconds(1);
        pump();
        require(book.valid() && session.full_order_log_matching_id() == 3,
                "fresh REFDATA must restore matching before composite publication");
        fake.enqueue(
            {.kind = f::EventKind::LifeNum, .stream_code = generated::StreamCode::kFortsRefdataRepl, .value = 7});
        pump();
        require(book.valid() && session.full_order_log_matching_id() == 3,
                "unchanged REFDATA LifeNum must preserve matching");
        enqueue(f::EventKind::LifeNum, 0, cgate::kNoTableCode, 8);
        pump();
        require(!book.valid() && book.order_count() == 0, "LifeNum change must clear public book");
        require(!session.stop(), "full log stop");
        // A bad anonymous table contract is a startup refusal with its missing field named.
        scenario.mutated_schema_field = generated::FieldCode::kFortsUserorderbookReplOrdersPublicAmountRest;
        scenario.omit_schema_field = true;
        fake.configure(scenario);
        CgateSession incompatible(config);
        require(!incompatible.start(), "schema test start");
        cgate::Plaza2Error schema_error;
        for (int i = 0; i < 5 && !schema_error; ++i)
            schema_error = incompatible.poll(false);
        require(schema_error.code == cgate::Plaza2ErrorCode::IncompatibleScheme &&
                    schema_error.message.find("public_amount_rest") != std::string::npos && !book.valid(),
                "missing anonymous snapshot field must fail closed by name");
        require(!incompatible.stop(), "schema test stop");
        scenario.mutated_schema_field = {};
        scenario.omit_schema_field = false;
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogMultiMatching)] = "1";
        fake.configure(scenario);
        config.market_data_isin_ids = {1001, 2002};
        opens = fake.opens(cgate::kFullOrderLogStreamCode);
        CgateSession partitioned(config);
        require(!partitioned.start(), "multiple matching start");
        cgate::Plaza2Error partition_error;
        for (int i = 0; i < 5 && !partition_error; ++i)
            partition_error = partitioned.poll(false);
        require(partition_error && partition_error.message.find("more than one matching ID") != std::string::npos,
                "configured instruments across matching IDs must be refused");
        require(fake.opens(cgate::kFullOrderLogStreamCode) == opens,
                "multiple matching refusal must precede composite open");
        require(!partitioned.stop(), "multiple matching stop");
        config.market_data_isin_ids = {1001};
        config.full_order_log_stream.open_settings = "replstate=unsafe";
        CgateSession invalid(config);
        require(invalid.start().code == cgate::Plaza2ErrorCode::InvalidConfiguration,
                "composite cannot accept replstate");
        require(!invalid.stop(), "invalid config stop");
        test::remove_tree(root);
        std::cout << "full order log session recovery/schema/matching passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
