#include "fake_cgate_control.hpp"
#include "full_order_log_router_lock.hpp"
#include "full_order_log_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_backfill.hpp"
#include "plaza2_runtime_test_support.hpp"

#include <csignal>
#include <iostream>
#include <unistd.h>

namespace cg = moex::plaza2::cgate;
namespace g = moex::plaza2::generated;
namespace t = moex::plaza2::test;
namespace f = moex::plaza2::test::fake;
namespace fs = std::filesystem;
using K = cg::Plaza2ListenerEventKind;
constexpr auto stream = cg::kFullOrderLogStreamCode;
constexpr auto table = g::TableCode::kFortsTradeReplOrdersLog;
constexpr std::string_view raw = "p2repl://FORTS_ORDLOG_REPL_MATCH2;tables=orders_log";
constexpr std::string_view connection_settings = "p2tcp://127.0.0.1:4102;app_name=offline_backfill_test";
constexpr std::array<std::int32_t, 1> isins{11};
volatile std::sig_atomic_t interrupted{};
void stop(int) {
    interrupted = 1;
}

void enqueue_frame(const f::Control& fake, std::int64_t rev, std::uint32_t trailing_close = 0) {
    using enum g::FieldCode;
    fake.enqueue({.kind = f::EventKind::Begin, .stream_code = stream});
    fake.enqueue({.kind = f::EventKind::Row,
                  .stream_code = stream,
                  .table_code = table,
                  .revision = rev,
                  .fields = {{.field_code = kFortsTradeReplOrdersLogReplId, .signed_value = rev},
                             {.field_code = kFortsTradeReplOrdersLogReplRev, .signed_value = rev},
                             {.field_code = kFortsTradeReplOrdersLogSessId, .signed_value = 321},
                             {.field_code = kFortsTradeReplOrdersLogIsinId, .signed_value = isins.front()}}});
    fake.enqueue({.kind = f::EventKind::Commit, .stream_code = stream, .value = trailing_close});
}

struct Tap : cg::Plaza2ListenerEventHandler {
    struct Row {
        std::vector<std::byte> data;
        std::vector<std::uint8_t> nulls;
    };
    std::vector<cg::Plaza2RawTableBinding> schema;
    std::vector<Row> rows;

    bool wants_raw_replication(std::string_view) const noexcept override {
        return true;
    }
    cg::Plaza2Error on_plaza2_listener_event(const cg::Plaza2ListenerEvent& event) override {
        if (event.kind == K::Open)
            schema.assign(event.raw_tables.begin(), event.raw_tables.end());
        else if (event.kind == K::StreamData)
            rows.push_back({{event.raw_payload.begin(), event.raw_payload.end()},
                            {event.raw_nulls.begin(), event.raw_nulls.end()}});
        return {};
    }
};

void write_primary(const fs::path& path, const cg::Plaza2Settings& runtime, const f::Control& fake) {
    Tap tap;
    {
        cg::Plaza2Env env;
        cg::Plaza2Connection conn;
        cg::Plaza2Listener listener;
        t::require(!env.open(runtime) && !conn.create(env, connection_settings) && !conn.open("") &&
                       !listener.create(conn, stream, raw, &tap) &&
                       !listener.open("mode=snapshot+online;lifenum=7;rev.orders_log=101"),
                   "open fake raw ORDLOG fixture");
        for (const auto rev : {102, 104, 111})
            enqueue_frame(fake, rev);
        for (int i = 0; i < 9; ++i)
            t::require(!conn.process(1) && !listener.last_callback_error(), "capture exact fake wire fixture");
        t::require(tap.rows.size() == 3 && tap.schema.size() == 1 && tap.schema.front().name == "orders_log",
                   "plain orders_log fake schema");
        t::require(!listener.close() && !listener.destroy() && !conn.close() && !conn.destroy() && !env.close(),
                   "close fake wire fixture");
    }
    auto info = full_order_log_test::tables()[1];
    info.index = tap.schema.front().index + 1;
    tap.schema.push_back(info);
    cg::FullOrderLogCapture primary({.directory = path, .min_free_bytes = 0}, isins, tap);
    const auto record = [&](cg::Plaza2ListenerEvent event) {
        event.stream_code = stream;
        t::require(!primary.record(event), "record synthetic composite history");
    };
    const auto open = [&] {
        record({.kind = K::Open,
                .text_value = "p2ordbook://FORTS_ORDLOG_REPL_MATCH2;snapshot=FORTS_ORDBOOK_REPL_MATCH2",
                .raw_tables = tap.schema});
        full_order_log_test::Row row(info);
        row.set("publication_state", std::int8_t{1});
        row.set("trades_lifenum", std::int64_t{7});
        record(row.event());
        record({.kind = K::Online});
    };
    const auto frame = [&](std::size_t index) {
        record({.kind = K::TransactionBegin});
        const auto& row = tap.rows[index];
        record({.kind = K::StreamData,
                .raw_payload = row.data,
                .raw_nulls = row.nulls,
                .table_index = tap.schema.front().index,
                .raw_table = &tap.schema.front()});
        record({.kind = K::TransactionCommit});
    };
    open();
    frame(0);
    frame(1);
    record({.kind = K::Close});
    open();
    frame(2);
    record({.kind = K::Close});
    primary.finish();
}

void test_router_lock(const fs::path& root) {
    using moex::connector_host::FullOrderLogRouterLock;
    const auto directory = root / "locks";
    fs::create_directory(directory);
    constexpr std::string_view alias = "p2tcp://localhost:04102;app_name=another";
    {
        const FullOrderLogRouterLock first(connection_settings, directory);
        bool busy{};
        try {
            const FullOrderLogRouterLock second(alias, directory);
        } catch (const std::exception& error) {
            busy = std::string_view(error.what()).find("router already in use") != std::string_view::npos;
        }
        t::require(busy, "same port aliases contend for the connection lifetime");
        const FullOrderLogRouterLock other("p2tcp://127.0.0.1:4103;app_name=another", directory);
    }
    t::require(fs::exists(directory / "moex-fol-router-4102.lock"), "release leaves lock inode intact");
    const FullOrderLogRouterLock next(alias, directory);
}

int main(int argc, char** argv) {
    try {
        t::require(argc == 2, "fake runtime argument");
        const auto root = t::make_temp_directory("fol_backfill_collector");
        test_router_lock(root);
        const auto fixture =
            t::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                           t::build_vendor_like_runtime_scheme("SPECTRA 9.12.0", "9.12", "T1"));
        const cg::Plaza2Settings runtime{.runtime_root = fixture.root,
                                         .scheme_dir = fixture.scheme_dir,
                                         .env_open_settings = "ini=config/t1.ini;key=00000000"};
        const f::Control fake(fixture.library_path);
        f::Scenario scenario;
        scenario.trade_server_lifenum = 7;
        fake.configure(scenario);
        const auto primary = root / "primary";
        write_primary(primary, runtime, fake);
        const auto plan = cg::plan_full_order_log_backfill(primary, 2);
        t::require(plan.start.revision == 102 && plan.left.revision == 104 && plan.right.revision == 111,
                   "matching provenance and LifeNum produce expected recovery plan");
        const auto collect = [&](const fs::path& output, std::chrono::sys_seconds deadline,
                                 const std::function<bool()>& stopped = {}) {
            return cg::collect_full_order_log_backfill(plan, {.directory = output, .min_free_bytes = 0}, runtime,
                                                       connection_settings, std::chrono::seconds(2), deadline, stopped);
        };
        using namespace std::chrono;
        const auto deadline = floor<seconds>(system_clock::now()) + seconds(20);
        const auto closes = fake.successful_closes();
        for (const auto rev : {104, 105, 111})
            enqueue_frame(fake, rev);
        const auto good = root / "good";
        t::require(collect(good, deadline).committed_rows_between == 1, "collect overlap before ONLINE");
        const auto coverage = cg::read_full_order_log_capture(good, [](const auto&) {});
        t::require(coverage.fol_coverage.onlines == 0 && fake.listener_handles(stream) == 0 &&
                       fake.successful_closes()[1] == closes[1] + 1 && fake.successful_closes()[2] == closes[2] + 1,
                   "collector closes listener then connection without waiting for ONLINE");

        // The right witness commits before a Close delivered by the same process call.
        enqueue_frame(fake, 104);
        enqueue_frame(fake, 111, 0xA006);
        const auto closed = root / "closed";
        t::require(collect(closed, deadline).committed_rows_between == 0 && fake.listener_handles(stream) == 0,
                   "transport Close after completed overlap does not invalidate the proof");

        const auto expect_error = [&](const fs::path& output, std::string_view expected, std::chrono::sys_seconds limit,
                                      const std::function<bool()>& stopped = {}) {
            try {
                static_cast<void>(collect(output, limit, stopped));
            } catch (const std::exception& error) {
                if (std::string_view(error.what()).find(expected) != std::string_view::npos)
                    return;
                throw;
            }
            throw std::runtime_error("collector unexpectedly succeeded");
        };
        scenario.trade_server_lifenum = 8;
        fake.configure(scenario);
        const auto stale = root / "stale";
        const auto processed = fake.process_count();
        expect_error(stale, "LifeNum mismatch", deadline);
        t::require(fake.process_count() == processed && fake.listener_handles(stream) == 0,
                   "LifeNum before OPEN remains fatal before polling");

        scenario.trade_server_lifenum = 7;
        fake.configure(scenario);
        fake.enqueue({.kind = f::EventKind::ListenerError, .stream_code = stream});
        expect_error(root / "listener-error", "listener interrupted", deadline);
        fake.enqueue({.kind = f::EventKind::ClearDeleted, .stream_code = stream, .table_code = table, .revision = 103});
        expect_error(root / "clear", "cleared after requested checkpoint", deadline);
        enqueue_frame(fake, 105);
        const auto overshoot = root / "left-overshoot";
        expect_error(overshoot, "passed the left witness", deadline);
        // The overshoot stops on the row rather than consuming its following commit.
        const auto failed = cg::read_full_order_log_capture(overshoot, [](const auto&) {}, true);
        t::require(failed.rows == 1 && failed.commits == 0 && failed.listener_errors == 1 &&
                       failed.end_status == cg::FullOrderLogCaptureEndStatus::CaptureFailed,
                   "the rejected selected row is retained before overlap validation");
        const auto expired = root / "expired";
        expect_error(expired, "deadline reached", floor<seconds>(system_clock::now()));
        t::require(!fs::exists(expired), "expired absolute deadline creates no capture");
        t::require(std::signal(SIGTERM, stop) != SIG_ERR, "install test signal handler");
        const auto process_at_term = fake.process_count();
        expect_error(root / "term", "interrupted", deadline, [&] {
            if (fake.process_count() > process_at_term)
                ::kill(::getpid(), SIGTERM);
            return interrupted;
        });
        t::require(interrupted && fake.listener_handles(stream) == 0, "SIGTERM shuts down the collector");
        t::remove_tree(root);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
