#include "moex/plaza2_trade/cgate_session.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_oi.hpp"
#include "moex/plaza2/cgate/plaza2_replication_tables.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include <algorithm>
#include <array>
#include <vector>
#include <iostream>
#include <limits>
#include <fstream>
#include <iterator>
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
        config.runtime.scheme_dir = fixture.scheme_dir;
        config.runtime.env_open_settings = "ini=config/t1.ini;key=00000000";
        config.connection_settings = "p2tcp://127.0.0.1:4101;app_name=full-order-log-test";
        config.read_only_market_data = true;
        config.full_order_log_partitioned = true;
        config.process_timeout_ms = 0;
        config.recovery_now = [&] { return now; };
        config.private_streams = {
            {generated::StreamCode::kFortsRefdataRepl,
             cgate::required_replication_listener_settings(generated::StreamCode::kFortsRefdataRepl),
             "mode=snapshot+online"}};
        config.market_data_isin_ids = {1001};
        const auto canonical_url = cgate::full_order_log_listener_settings(fixture.scheme_dir);
        const auto match3_url = cgate::full_order_log_listener_settings(fixture.scheme_dir, 3);
        config.full_order_log_stream = {cgate::kFullOrderLogStreamCode, canonical_url, ""};
        config.full_order_log_handler = &book;
        std::vector<std::string> operation_logs;
        std::vector<std::uint32_t> close_reasons;
        std::vector<std::string> refdata_tables, full_log_tables;
        config.event_log = [&](std::string_view kind, std::string_view detail) {
            operation_logs.push_back(std::string(kind) + " " + std::string(detail));
        };
        config.listener_event_log = [&](const cgate::Plaza2ListenerEvent& event) {
            if (event.stream_code == generated::StreamCode::kFortsRefdataRepl &&
                event.kind == cgate::Plaza2ListenerEventKind::Open) {
                refdata_tables.clear();
                for (const auto& table : event.raw_tables)
                    refdata_tables.push_back(table.name);
                std::ranges::sort(refdata_tables);
            }
            if (event.stream_code == cgate::kFullOrderLogStreamCode &&
                event.kind == cgate::Plaza2ListenerEventKind::Open) {
                full_log_tables.clear();
                for (std::size_t i = 0; i < event.raw_tables.size(); ++i) {
                    require(event.raw_tables[i].index == i, "filtered composite table indices must be reindexed");
                    full_log_tables.push_back(event.raw_tables[i].name);
                }
                if (event.raw_tables.size() == 4)
                    require(event.raw_tables[2].name == "orders" && event.raw_tables[3].name == "info",
                            "client schema order must move snapshot indices before ClearDeleted probes");
                std::ranges::sort(full_log_tables);
            }
            if (event.stream_code == cgate::kFullOrderLogStreamCode &&
                event.kind == cgate::Plaza2ListenerEventKind::Close)
                close_reasons.push_back(event.close_reason);
        };
        auto scenario = f::Scenario{};
        scenario.spectra912_refdata_schema = true;
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogRefdata)] = "1";
        scenario.options[static_cast<std::size_t>(f::Option::DisablePublisher)] = "1";
        scenario.options[static_cast<std::size_t>(f::Option::DisableReplyListener)] = "1";
        fake.configure(scenario);
        fake.configure_refdata_count(5000);
        for (const auto& unsafe : {std::string("p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL"),
                                   canonical_url + ";tables=orders_log", canonical_url + ";unexpected=1"}) {
            auto bad = config;
            bad.full_order_log_stream.settings = unsafe;
            CgateSession rejected(bad);
            require(rejected.start().code == cgate::Plaza2ErrorCode::InvalidConfiguration &&
                        fake.opens(cgate::kFullOrderLogStreamCode) == 0,
                    "bare, tables= and unexpected composite URLs must fail before listener startup");
        }
        for (const auto& unsafe :
             {std::filesystem::path("relative"), fixture.scheme_dir / "bad;path", fixture.scheme_dir / "bad|path",
              fixture.scheme_dir / "bad\npath", fixture.scheme_dir / "bad;path/../safe"}) {
            bool refused = false;
            try {
                (void)cgate::full_order_log_listener_settings(unsafe);
            } catch (const std::invalid_argument&) {
                refused = true;
            }
            require(refused, "ambiguous CGate schema paths must be refused");
        }
        for (const auto* name : {"online.ini", "snapshot.ini"}) {
            const auto path = fixture.scheme_dir / "full_order_log" / name;
            std::filesystem::rename(path, path.string() + ".hidden");
            CgateSession missing_file(config);
            require(missing_file.start().code == cgate::Plaza2ErrorCode::InvalidConfiguration &&
                        fake.opens(cgate::kFullOrderLogStreamCode) == 0,
                    "a missing client schema must refuse startup without an unfiltered fallback");
            std::filesystem::rename(path.string() + ".hidden", path);
        }
        fake.set(f::Option::ConnHoldOpening);
        CgateSession session(config);
        require(!session.start(), "full order log start");
        const auto pump = [&](int n = 3) {
            for (int i = 0; i < n; ++i) {
                const auto e = session.poll(false);
                require(!e, "full order log poll: " + e.message + " callback: " + session.last_callback_error());
            }
        };
        pump();
        require(!session.runtime_health().private_active && fake.opens(generated::StreamCode::kFortsRefdataRepl) == 0 &&
                    fake.opens(cgate::kFullOrderLogStreamCode) == 0,
                "read-only startup must wait for ACTIVE with no listener opens or fabricated healthy streams");
        fake.clear(f::Option::ConnHoldOpening);
        pump();
        require(session.full_order_log_matching_id() == 3,
                "Spectra 9.12 REFDATA without opening price must resolve matching 3");
        require(refdata_tables == std::vector<std::string>{"fut_instruments", "fut_sess_contents", "fut_vcb",
                                                           "instr2matching_map", "session", "sys_messages"},
                "FullLog must negotiate exactly the six consumed REFDATA tables under native filtering");
        require(fake.listener_settings(generated::StreamCode::kFortsRefdataRepl) ==
                    "p2repl://"
                    "FORTS_REFDATA_REPL;tables=session,fut_instruments,fut_sess_contents,fut_vcb,sys_messages,"
                    "instr2matching_map",
                "FullLog alone must append matching to the reviewed REFDATA server subscription");
        require(fake.listener_settings(cgate::kFullOrderLogStreamCode) == match3_url &&
                    full_log_tables == std::vector<std::string>{"info", "orders", "orders_log", "sys_events"},
                "both client INIs must negotiate exactly the four composite tables with no tables selector");
        require(session.private_state().instruments().size() == 5000, "realistic REFDATA bootstrap size");
        const auto metadata_revision = session.market_data_metadata_revision();
        pump(100);
        require(session.market_data_metadata_revision() == metadata_revision,
                "idle polls must preserve committed metadata token");
        require(book.valid() && book.order_count() == 2, "composite snapshot must publish on first online commit");
        require(book.levels(1001, true).at(10250000000LL) == 7 && book.levels(1001, false).at(10260000000LL) == 9,
                "composite snapshot must retain exact d16.5 levels");
        require(session.runtime_health().full_order_log == 3 && !session.publisher_open() && !session.p2mqreply_open(),
                "full log must run without order-entry transports");
        require(session.connection_app_name() == "full-order-log-test",
                "market data process must retain own app identity");
        fake.enqueue({.kind = f::EventKind::Begin, .stream_code = cgate::kFullOrderLogStreamCode});
        for (unsigned i = 0; i < 80; ++i)
            fake.enqueue({.kind = f::EventKind::Row,
                          .stream_code = cgate::kFullOrderLogStreamCode,
                          .table_code = generated::TableCode::kFortsTradeReplSysEvents});
        fake.enqueue({.kind = f::EventKind::Commit, .stream_code = cgate::kFullOrderLogStreamCode});
        pump(100);
        require(std::count_if(operation_logs.begin(), operation_logs.end(),
                              [](const auto& line) { return line.starts_with("full_order_log_event "); }) == 64,
                "each cycle must log only the first 64 kinds, including ordinary filtered data: " +
                    std::to_string(std::count_if(operation_logs.begin(), operation_logs.end(), [](const auto& line) {
                        return line.starts_with("full_order_log_event ");
                    })));
        require(std::any_of(operation_logs.begin(), operation_logs.end(),
                            [](const auto& line) {
                                return line.starts_with("full_order_log_event ") &&
                                       line.find("\"data_size\":") != std::string::npos &&
                                       line.find("\"table\":\"sys_events\"") != std::string::npos;
                            }),
                "bounded diagnostics include table and size without decoding field values");
        const auto enqueue = [&](f::EventKind kind, std::int64_t rev = 0,
                                 generated::TableCode table = cgate::kNoTableCode, std::uint64_t value = 0) {
            fake.enqueue({.kind = kind,
                          .stream_code = cgate::kFullOrderLogStreamCode,
                          .table_code = table,
                          .revision = rev,
                          .value = value});
        };
        // Vendor channel timeout arrives as a native CLOSE reason. The old
        // committed book must disappear before the next fresh composite OPEN.
        auto timeout_opens = fake.opens(cgate::kFullOrderLogStreamCode);
        enqueue(f::EventKind::Close, 0, cgate::kNoTableCode, 0xA006);
        pump(1);
        require(!book.valid() && !close_reasons.empty() && close_reasons.back() == 0xA006,
                "REPL:CHANNEL_DISCONNECT_ON_TIMEOUT close reason must be delivered and fence the book");
        pump();
        require(fake.opens(cgate::kFullOrderLogStreamCode) == timeout_opens + 1 && book.valid() &&
                    book.order_count() == 2,
                "0xA006 native CLOSE must reopen the composite and publish a fresh snapshot/ONLINE commit");
        require(fake.listener_open_settings(cgate::kFullOrderLogStreamCode).find("replstate") == std::string::npos,
                "0xA006 composite recovery must not reuse replay state");
        timeout_opens = fake.opens(cgate::kFullOrderLogStreamCode);
        fake.set(f::Option::ProcessResult, "40966");
        pump(1);
        fake.clear(f::Option::ProcessResult);
        require(!book.valid() && std::any_of(operation_logs.begin(), operation_logs.end(),
                                             [](const auto& line) {
                                                 return line.find("cgate_operation") != std::string::npos &&
                                                        line.find("\"runtime_code\":40966") != std::string::npos;
                                             }),
                "injected 0xA006 process error must be logged and fence the composite");
        now += std::chrono::seconds(1);
        pump(5);
        require(fake.opens(cgate::kFullOrderLogStreamCode) == timeout_opens + 1 && book.valid() &&
                    !session.publisher_open() && !session.p2mqreply_open() && fake.commands().empty(),
                "0xA006 process recovery must restore only read-side replication with no order handles or posts");
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
        require(!book.valid() && book.order_count() == 0, "ClearDeleted MAX must fence until a fresh snapshot");
        auto opens = fake.opens(cgate::kFullOrderLogStreamCode);
        now += std::chrono::seconds(30);
        pump(1);
        require(fake.opens(cgate::kFullOrderLogStreamCode) == opens && book.needs_fresh_snapshot(),
                "watchdog must not reopen at the exact 30-second boundary");
        now += std::chrono::milliseconds(1);
        pump(1);
        require(fake.opens(cgate::kFullOrderLogStreamCode) == opens && !book.needs_fresh_snapshot(),
                "expired MAX watchdog closes the composite before retry");
        now += std::chrono::milliseconds(999);
        pump(1);
        require(fake.opens(cgate::kFullOrderLogStreamCode) == opens, "watchdog retry waits a full second");
        now += std::chrono::milliseconds(1);
        pump();
        require(fake.opens(cgate::kFullOrderLogStreamCode) == ++opens && book.valid() && book.order_count() == 2,
                "MAX watchdog reopens a fresh native snapshot");
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
        require(fake.listener_open_settings(cgate::kFullOrderLogStreamCode).find("replstate") == std::string::npos &&
                    fake.listener_settings(cgate::kFullOrderLogStreamCode) == match3_url &&
                    full_log_tables == std::vector<std::string>{"info", "orders", "orders_log", "sys_events"},
                "fresh composite recovery must retain both client schemas and never pass replay keys");
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
        const auto before_remap = session.market_data_metadata_revision();
        pump();
        require(book.valid() && session.full_order_log_matching_id() == 3 &&
                    session.market_data_metadata_revision() == before_remap,
                "uncommitted REFDATA remap must preserve matching/book/metadata");
        fake.enqueue({.kind = f::EventKind::Commit, .stream_code = generated::StreamCode::kFortsRefdataRepl});
        pump(1);
        require(session.market_data_metadata_revision() != before_remap, "metadata token changes at commit");
        require(session.full_order_log_matching_id() == 4 && !book.valid(),
                "matching remap must fence the old public book");
        now += std::chrono::seconds(1);
        pump();
        require(book.valid() && fake.listener_settings(cgate::kFullOrderLogStreamCode) ==
                                    cgate::full_order_log_listener_settings(fixture.scheme_dir, 4),
                "single matching remap must rebuild a fresh composite snapshot on the new service");
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
        // SESSIONSTATE may commit a new day before REFDATA has its contract.
        using enum generated::FieldCode;
        const auto status_stream = generated::StreamCode::kFortsSessionstateRepl;
        fake.enqueue({.kind = f::EventKind::Begin, .stream_code = status_stream});
        fake.enqueue({.kind = f::EventKind::Row,
                      .stream_code = status_stream,
                      .table_code = generated::TableCode::kFortsSessionstateReplSessionState,
                      .revision = 50,
                      .fields = {{.field_code = kFortsSessionstateReplSessionStateReplId, .signed_value = 30003},
                                 {.field_code = kFortsSessionstateReplSessionStateSessId, .signed_value = 322},
                                 {.field_code = kFortsSessionstateReplSessionStatePublicState, .signed_value = 1}}});
        fake.enqueue({.kind = f::EventKind::Commit, .stream_code = status_stream});
        pump();
        require(session.private_state().current_session_id() == 322 && !session.full_order_log_matching_id() &&
                    !book.valid(),
                "rollover missing REFDATA must wait and fence without exiting");
        const auto rollover_opens = fake.opens(cgate::kFullOrderLogStreamCode);
        now += std::chrono::seconds(2);
        pump(20);
        require(fake.opens(cgate::kFullOrderLogStreamCode) == rollover_opens,
                "missing rollover mapping must not cause a reopen storm");
        const auto ref_stream = generated::StreamCode::kFortsRefdataRepl;
        fake.enqueue({.kind = f::EventKind::Begin, .stream_code = ref_stream});
        fake.enqueue({.kind = f::EventKind::Row,
                      .stream_code = ref_stream,
                      .table_code = generated::TableCode::kFortsRefdataReplFutSessContents,
                      .revision = 51,
                      .fields = {{.field_code = kFortsRefdataReplFutSessContentsReplId, .signed_value = 50003},
                                 {.field_code = kFortsRefdataReplFutSessContentsIsinId, .signed_value = 1001},
                                 {.field_code = kFortsRefdataReplFutSessContentsSessId, .signed_value = 322},
                                 {.field_code = kFortsRefdataReplFutSessContentsBaseContractCode,
                                  .kind = f::FieldKind::Text,
                                  .text = "RTS"}}});
        fake.enqueue({.kind = f::EventKind::Commit, .stream_code = ref_stream});
        pump();
        require(session.full_order_log_matching_id() == 3 && book.valid(),
                "new committed rollover terms resume the composite snapshot");
        enqueue(f::EventKind::LifeNum, 0, cgate::kNoTableCode, 8);
        pump();
        require(!book.valid() && book.order_count() == 0 && book.needs_fresh_snapshot(),
                "unexplained LifeNum change must clear the book and arm snapshot recovery");
        const auto life_opens = fake.opens(cgate::kFullOrderLogStreamCode);
        now += std::chrono::seconds(29);
        // The native source can provide a fresh snapshot before the watchdog fires.
        enqueue(f::EventKind::Begin);
        fake.enqueue({.kind = f::EventKind::Row,
                      .stream_code = cgate::kFullOrderLogStreamCode,
                      .table_code = generated::TableCode::kFortsUserorderbookReplInfo,
                      .fields = {{.field_code = kFortsUserorderbookReplInfoPublicationState, .signed_value = 1},
                                 {.field_code = kFortsUserorderbookReplInfoTradesLifenum, .signed_value = 8}}});
        enqueue(f::EventKind::Commit);
        enqueue(f::EventKind::Online);
        pump();
        require(!book.valid() && book.needs_fresh_snapshot(), "fresh ONLINE alone cannot cancel the watchdog");
        enqueue(f::EventKind::Begin);
        enqueue(f::EventKind::Commit);
        pump();
        now += std::chrono::seconds(2);
        pump();
        require(book.valid() && !book.needs_fresh_snapshot() &&
                    fake.opens(cgate::kFullOrderLogStreamCode) == life_opens,
                "committed fresh snapshot cancels the pending LifeNum watchdog");
        enqueue(f::EventKind::LifeNum, 0, cgate::kNoTableCode, 99);
        pump();
        now += std::chrono::seconds(31);
        pump();
        require(!book.valid() && fake.opens(cgate::kFullOrderLogStreamCode) == life_opens,
                "a second unexplained LifeNum independently expires its watchdog");
        now += std::chrono::seconds(1);
        pump();
        require(book.valid() && book.order_count() == 2 && fake.opens(cgate::kFullOrderLogStreamCode) == life_opens + 1,
                "repeated watchdog recovery opens a fresh composite without replay state");
        now += std::chrono::seconds(31);
        pump();
        require(fake.opens(cgate::kFullOrderLogStreamCode) == life_opens + 1,
                "successful fresh reopen does not rearm the watchdog");
        require(!session.stop(), "full log stop");
        // Mutating the actual client schema must change the negotiated OPEN,
        // rather than letting a fake pass from URL text alone.
        const auto online_path = fixture.scheme_dir / "full_order_log/online.ini";
        std::ifstream online_input(online_path);
        const std::string online_text((std::istreambuf_iterator<char>(online_input)), {});
        for (const auto& omitted : {std::string("table=sys_events"), std::string("field=public_amount_rest,i8")}) {
            auto filtered = online_text;
            const auto at = filtered.find(omitted);
            require(at != std::string::npos, "vendor client schema contains required declaration");
            filtered.erase(at, omitted.size());
            test::write_text_file(online_path, filtered);
            fake.configure(scenario);
            CgateSession incomplete(config);
            require(!incomplete.start(), "incomplete client schema starts its read-side connection");
            cgate::Plaza2Error error;
            for (int i = 0; i < 5 && !error; ++i)
                error = incomplete.poll(false);
            require(error.code == cgate::Plaza2ErrorCode::IncompatibleScheme && !book.valid(),
                    "a required table or field omitted from the actual client INI must fail closed at OPEN");
            require(!incomplete.stop(), "incomplete client schema stop");
            test::write_text_file(online_path, online_text);
        }
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
        for (const bool missing_table : {true, false}) {
            scenario.omitted_schema_table =
                missing_table ? generated::TableCode::kFortsRefdataReplInstr2matchingMap : cgate::kNoTableCode;
            scenario.mutated_schema_field = missing_table
                                                ? generated::FieldCode{}
                                                : generated::FieldCode::kFortsRefdataReplInstr2matchingMapMatchingId;
            scenario.omit_schema_field = !missing_table;
            fake.configure(scenario);
            const auto before = fake.opens(cgate::kFullOrderLogStreamCode);
            CgateSession missing_matching(config);
            require(!missing_matching.start(), "missing matching contract start");
            cgate::Plaza2Error error;
            for (int i = 0; i < 5 && !error; ++i)
                error = missing_matching.poll(false);
            require(error.code == cgate::Plaza2ErrorCode::IncompatibleScheme &&
                        error.message.find("instr2matching_map") != std::string::npos &&
                        fake.opens(cgate::kFullOrderLogStreamCode) == before && !book.valid(),
                    "missing filtered matching table or field must refuse before composite startup");
            require(!missing_matching.stop(), "missing matching contract stop");
        }
        scenario.omitted_schema_table = {};
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
        config.full_order_log_stream.open_settings.clear();
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogMultiMatching)] = "";
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogSnapshotShortRow)] = "1";
        fake.configure(scenario);
        operation_logs.clear();
        const auto unframed_opens = fake.opens(cgate::kFullOrderLogStreamCode);
        CgateSession unframed(config);
        require(!unframed.start(), "unframed diagnostic start");
        cgate::Plaza2Error fatal;
        for (unsigned i = 0; i < 12 && !fatal; ++i) {
            fatal = unframed.poll(false);
            now += std::chrono::seconds(1);
        }
        require(fatal.code == cgate::Plaza2ErrorCode::DecodeFailed &&
                    fatal.message.find("3 identical row-validation failures") != std::string::npos &&
                    fatal.message.find("row size table=orders") != std::string::npos,
                "unframed snapshot must stop on the third identical pre-ONLINE row failure");
        const auto fatal_opens = fake.opens(cgate::kFullOrderLogStreamCode);
        require(fatal_opens == unframed_opens + 3 && !book.valid(),
                "no fourth OPEN or visible book after three strikes");
        for (unsigned i = 0; i < 5; ++i) {
            now += std::chrono::seconds(10);
            require(unframed.poll(false).message == fatal.message, "fatal must stay sticky");
        }
        require(fake.opens(cgate::kFullOrderLogStreamCode) == fatal_opens, "fatal must never reopen again");
        require(!unframed.stop(), "fatal diagnostic cleanup");
        require(std::count_if(operation_logs.begin(), operation_logs.end(),
                              [](const auto& line) { return line.starts_with("full_order_log_cycle_totals "); }) == 3,
                "one totals record per failed cycle despite native/synthetic CLOSE");
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogSnapshotShortRow)] = "";
        // A genuine successful ONLINE between failures breaks the fatal streak.
        fake.configure(scenario);
        operation_logs.clear();
        fake.set(f::Option::FullOrderLogSnapshotShortRow);
        CgateSession recovering(config);
        require(!recovering.start(), "diagnostic streak-reset start");
        const auto failures = [&] {
            return std::count_if(operation_logs.begin(), operation_logs.end(),
                                 [](const auto& line) { return line.starts_with("full_order_log_row_failure "); });
        };
        for (unsigned i = 0; i < 10 && failures() < 2; ++i) {
            require(!recovering.poll(false), "two failures remain recoverable");
            now += std::chrono::seconds(1);
        }
        require(failures() == 2, "streak fixture must deliver two real unframed snapshots");
        fake.clear(f::Option::FullOrderLogSnapshotShortRow);
        for (unsigned i = 0; i < 5 && !book.valid(); ++i) {
            require(!recovering.poll(false), "framed snapshot recovers before third strike");
            now += std::chrono::seconds(1);
        }
        require(book.valid(), "ONLINE must restore the book in the positive fake contract");
        fake.set(f::Option::FullOrderLogSnapshotShortRow);
        fake.enqueue({.kind = f::EventKind::Close, .stream_code = cgate::kFullOrderLogStreamCode});
        for (unsigned i = 0; i < 10 && failures() < 4; ++i) {
            require(!recovering.poll(false), "ONLINE must reset, not merely pause, the three-strike counter");
            now += std::chrono::seconds(1);
        }
        require(failures() == 4, "two new failures after ONLINE remain below the stop threshold");
        require(!recovering.stop(), "streak-reset cleanup");
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogSnapshotShortRow)] = "";
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogSnapshotShortRow)] = "";
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogSnapshotNoTransactions)] = "1";
        fake.configure(scenario);
        operation_logs.clear();
        fake.set(f::Option::FullOrderLogConsumesInfo);
        CgateSession snapshot_session(config);
        require(!snapshot_session.start(), "native-shaped snapshot start");
        for (unsigned i = 0; i < 5 && !book.valid(); ++i)
            require(!snapshot_session.poll(false), "unframed snapshot must reach ONLINE");
        require(book.valid() && book.order_count() == 2,
                "unframed rows publish at ONLINE, without a later fake commit");
        require(!book.committed_log_lifenum().has_value(), "hidden info does not fabricate a comparison anchor");
        fake.enqueue({.kind = f::EventKind::Begin, .stream_code = cgate::kFullOrderLogStreamCode});
        for (unsigned i = 0; i < 40; ++i)
            fake.enqueue({.kind = f::EventKind::Row,
                          .stream_code = cgate::kFullOrderLogStreamCode,
                          .table_code = generated::TableCode::kFortsTradeReplSysEvents});
        fake.enqueue({.kind = f::EventKind::Commit, .stream_code = cgate::kFullOrderLogStreamCode});
        for (unsigned i = 0; i < 50; ++i)
            require(!snapshot_session.poll(false), "framed post-ONLINE data is accepted");
        require(std::count_if(operation_logs.begin(), operation_logs.end(),
                              [](const auto& line) { return line.starts_with("full_order_log_online_event "); }) == 32,
                "first32 post-ONLINE events stay bounded independently of the cycle prefix");
        require(!snapshot_session.stop(), "native-shaped snapshot stop");
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogUnframedOnlineRow)] = "1";
        fake.configure(scenario);
        operation_logs.clear();
        CgateSession online_fault(config);
        require(!online_fault.start(), "post-ONLINE framing fault start");
        fatal = {};
        for (unsigned i = 0; i < 12 && !fatal; ++i) {
            fatal = online_fault.poll(false);
            now += std::chrono::seconds(1);
        }
        require(fatal && fatal.message.find("row outside transaction") != std::string::npos &&
                    fatal.message.find("online_seen=yes") != std::string::npos,
                "repeated post-ONLINE row violations stop despite successful bootstrap ONLINEs");
        require(!online_fault.stop(), "post-ONLINE framing fault cleanup");
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogSnapshotNoTransactions)] = "";
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogUnframedOnlineRow)] = "";
        fake.configure(scenario);
        fake.set(f::Option::DealsBadScheme);
        cgate::FullOrderLogOpenInterest oi(ids);
        config.full_order_log_oi_handler = &oi;
        CgateSession optional_deals(config);
        require(!optional_deals.start(), "optional DEALS schema failure start");
        for (int i = 0; i < 7; ++i)
            require(!optional_deals.poll(false), "optional DEALS schema failure must not stop FOL");
        require(book.valid() && optional_deals.runtime_health().public_deals != 3 &&
                    fake.opens(generated::StreamCode::kFortsDealsRepl) == 1,
                "FOL remains valid while incompatible DEALS is disabled");
        require(!optional_deals.stop(), "optional DEALS cleanup");
        config.full_order_log_oi_handler = nullptr;
        config.full_order_log_handler = nullptr;
        config.full_order_log_partitioned = false;
        config.full_order_log_stream = {};
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogMultiMatching)] = "";
        fake.configure(scenario);
        CgateSession ordinary(config);
        require(!ordinary.start(), "ordinary restricted REFDATA start");
        for (int i = 0; i < 5; ++i)
            require(!ordinary.poll(false), "ordinary restricted REFDATA poll");
        require(
            refdata_tables == std::vector<std::string>{"fut_instruments", "fut_sess_contents", "fut_vcb", "session",
                                                       "sys_messages"} &&
                ordinary.private_state().matching_map().empty() &&
                fake.listener_settings(generated::StreamCode::kFortsRefdataRepl) ==
                    "p2repl://FORTS_REFDATA_REPL;tables=session,fut_instruments,fut_sess_contents,fut_vcb,sys_messages",
            "no-handler subscription must retain exactly the certified five REFDATA tables");
        require(!ordinary.stop(), "ordinary restricted REFDATA stop");
        test::remove_tree(root);
        std::cout << "full order log session recovery/schema/matching passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
