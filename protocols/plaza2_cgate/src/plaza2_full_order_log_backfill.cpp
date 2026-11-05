#include "moex/plaza2/cgate/plaza2_full_order_log_backfill.hpp"
#include "full_order_log_capture_format.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace moex::plaza2::cgate {
namespace {
using capture_format::load;
using capture_format::require;
using Kind = Plaza2ListenerEventKind;
using Boundary = FullOrderLogBackfillBoundary;

const Plaza2RawFieldBinding& required_field(const Plaza2RawTableBinding& table, std::string_view name,
                                            std::string_view type, std::size_t size) {
    const auto it = std::ranges::find(table.fields, name, &Plaza2RawFieldBinding::name);
    require(it != table.fields.end() && it->type_token == type && it->size == size,
            "backfill witness field missing or incompatible");
    return *it;
}

bool is_null(const Plaza2ListenerEvent& e, const Plaza2RawFieldBinding& field) {
    require(e.raw_nulls.empty() || field.index < e.raw_nulls.size(), "backfill witness null map invalid");
    return !e.raw_nulls.empty() && e.raw_nulls[field.index] != 0;
}

template <class T> T read_field(const Plaza2ListenerEvent& e, std::string_view name, std::string_view type) {
    const auto& field = required_field(*e.raw_table, name, type, sizeof(T));
    require(!is_null(e, field), "backfill witness has null identity field");
    return load<T>(e.raw_payload, field.offset);
}

bool table_is(const Plaza2ListenerEvent& e, std::string_view name) {
    return e.raw_table && e.raw_table->name == name;
}

struct OrderRow {
    std::int64_t revision{};
    std::int32_t session{};
};

OrderRow order_row(const Plaza2ListenerEvent& e) {
    require(e.raw_payload.size() == e.raw_table->row_size, "backfill witness row size mismatch");
    const auto rev = read_field<std::int64_t>(e, "replRev", "i8");
    const auto session = read_field<std::int32_t>(e, "sess_id", "i4");
    require(rev > 0 && session > 0 && read_field<std::int64_t>(e, "replID", "i8") > 0 &&
                read_field<std::int32_t>(e, "isin_id", "i4") > 0,
            "backfill witness row lacks replication identity");
    return {.revision = rev, .session = session};
}

std::string row_digest(const Plaza2ListenerEvent& e) {
    std::vector<const Plaza2RawFieldBinding*> fields;
    for (const auto& field : e.raw_table->fields)
        fields.push_back(&field);
    std::ranges::sort(fields, {}, [](const auto* field) { return field->name; });
    // CGate may place the same fields at different offsets in the two listeners.
    // Hash sorted names, wire types, null flags and field bytes without padding.
    std::string canonical;
    std::string_view last;
    for (const auto* field : fields) {
        require(!field->name.empty() && field->name != last && field->name.find('\0') == std::string::npos &&
                    field->type_token.find('\0') == std::string::npos && field->offset <= e.raw_payload.size() &&
                    field->size <= e.raw_payload.size() - field->offset,
                "backfill witness schema invalid");
        last = field->name;
        canonical.append(field->name).push_back('\0');
        canonical.append(field->type_token).push_back('\0');
        canonical.append(std::to_string(field->size)).push_back('\0');
        const auto null = is_null(e, *field);
        canonical.push_back(null ? '1' : '0');
        if (!null)
            canonical.append(reinterpret_cast<const char*>(e.raw_payload.data() + field->offset), field->size);
    }
    return plaza2_sha256_hex(canonical);
}

Boundary witness(const Plaza2ListenerEvent& event, OrderRow row) {
    return {.revision = row.revision, .session = row.session, .sha256 = row_digest(event)};
}

struct PrimaryScanner {
    FullOrderLogBackfillPlan& plan;
    std::uint64_t requested_reopen{};
    Boundary previous, latest, first_in_frame, last_in_frame;
    std::uint64_t known_ordlog_life{}, staged_ordlog_life{};
    bool open_seen{}, matching_open{}, open{}, online{}, in_frame{};

    void clear_frame() {
        first_in_frame = {};
        last_in_frame = {};
        staged_ordlog_life = 0;
        in_frame = false;
    }

    void close() {
        clear_frame();
        open = false;
        online = false;
    }

    void commit_life() {
        if (!staged_ordlog_life)
            return;
        require(!known_ordlog_life || known_ordlog_life == staged_ordlog_life,
                "backfill ORDLOG LifeNum changed inside a composite generation");
        known_ordlog_life = std::exchange(staged_ordlog_life, 0);
    }

    void verify_life() {
        require(known_ordlog_life, "backfill requires committed ORDLOG LifeNum in both composite generations");
        require(!plan.ordlog_lifenum || known_ordlog_life == plan.ordlog_lifenum,
                "backfill ORDLOG LifeNum conflicts with composite info");
        plan.ordlog_lifenum = known_ordlog_life;
    }

    void visit(const FullOrderLogCaptureRecord& record) {
        const auto& e = record.event;
        if (e.stream_code != kFullOrderLogStreamCode || plan.right.commit_sequence)
            return;
        switch (e.kind) {
        case Kind::Open: {
            const auto matching = "_MATCH" + std::to_string(plan.matching_id);
            const bool next_matching =
                e.text_value == "p2ordbook://FORTS_ORDLOG_REPL" + matching + ";snapshot=FORTS_ORDBOOK_REPL" + matching;
            require(!plan.reopen_sequence, "backfill selected reopen did not produce a committed right boundary");
            if (open_seen && (!requested_reopen || record.sequence == requested_reopen)) {
                require(matching_open && next_matching,
                        "backfill requires matching-suffixed composite Open provenance on both boundary generations");
                require(previous.commit_sequence && latest.commit_sequence,
                        "backfill needs two committed ORDLOG transactions before the reopen");
                verify_life();
                plan.start = std::move(previous);
                plan.left = std::move(latest);
                plan.reopen_sequence = record.sequence;
            }
            previous = {};
            latest = {};
            close();
            open_seen = open = true;
            matching_open = next_matching;
            known_ordlog_life = 0;
            break;
        }
        case Kind::Close:
            close();
            break;
        case Kind::Online:
            if (!open)
                break;
            if (!in_frame)
                commit_life();
            online = true;
            break;
        case Kind::LifeNum:
            if (online)
                close(); // A composite notification cannot identify the changed leg.
            break;
        case Kind::ClearDeleted:
            if (online && (table_is(e, "orders_log") || table_is(e, "orders") || table_is(e, "info")) &&
                e.signed_value == std::numeric_limits<std::int64_t>::max())
                close();
            break;
        case Kind::TransactionBegin:
            if (open) {
                require(!in_frame, "backfill nested capture transaction");
                in_frame = true;
            }
            break;
        case Kind::StreamData:
            if (open && table_is(e, "info") && read_field<std::int8_t>(e, "publication_state", "i1") == 1 &&
                read_field<std::int64_t>(e, "replAct", "i8") == 0) {
                const auto life = read_field<std::int64_t>(e, "trades_lifenum", "i8");
                require(life > 0 && life <= UINT32_MAX, "backfill composite info ORDLOG LifeNum invalid");
                staged_ordlog_life = static_cast<std::uint64_t>(life);
            }
            if (open && online && table_is(e, "orders_log")) {
                require(in_frame, "backfill ORDLOG row outside transaction");
                auto row = witness(e, order_row(e));
                require(row.revision > std::max(latest.revision, last_in_frame.revision),
                        "backfill ORDLOG revisions not increasing");
                if (!first_in_frame.revision)
                    first_in_frame = row;
                last_in_frame = std::move(row);
            }
            break;
        case Kind::TransactionCommit:
            if (open) {
                require(in_frame, "backfill commit without transaction");
                commit_life();
                if (last_in_frame.revision) {
                    if (plan.reopen_sequence) {
                        verify_life();
                        first_in_frame.commit_sequence = record.sequence;
                        plan.right = std::move(first_in_frame);
                    } else {
                        previous = std::move(latest);
                        last_in_frame.commit_sequence = record.sequence;
                        latest = std::move(last_in_frame);
                    }
                }
                clear_frame();
            }
            break;
        default:
            break;
        }
    }
};

struct RecoveryScanner {
    const FullOrderLogBackfillPlan& plan;
    std::int64_t latest{};
    std::uint64_t between{}, staged_between{};
    bool opened{}, closed{}, life{}, online{}, in_frame{}, left{}, right{}, staged_left{}, staged_right{};

    bool complete() const {
        return left && right && !in_frame;
    }

    void visit(const Plaza2ListenerEvent& e) {
        require(e.stream_code == kFullOrderLogStreamCode && (!closed || e.kind == Kind::ReplState),
                "backfill recovery capture must contain only one ORDLOG generation");
        switch (e.kind) {
        case Kind::Open:
            require(!opened && e.text_value.empty() && e.raw_tables.size() == 1 &&
                        e.raw_tables.front().name == "orders_log",
                    "backfill recovery must use only the plain orders_log listener");
            opened = true;
            break;
        case Kind::LifeNum:
            require(e.unsigned_value == plan.ordlog_lifenum, "backfill recovery LifeNum mismatch");
            life = true;
            break;
        case Kind::Online:
            require(opened && !in_frame && !online, "backfill recovery ONLINE without open or at invalid boundary");
            online = true;
            break;
        case Kind::TransactionBegin:
            require(opened && !in_frame, "backfill recovery transaction without valid open");
            in_frame = true;
            break;
        case Kind::StreamData: {
            require(in_frame && table_is(e, "orders_log"), "backfill recovery row outside ORDLOG transaction");
            const auto row = order_row(e);
            require(row.revision >= plan.start.revision && row.revision > latest,
                    "backfill recovery revisions before cursor or not increasing");
            require(row.revision <= plan.left.revision || left || staged_left,
                    "backfill recovery passed the left witness without overlap");
            require(row.revision <= plan.right.revision || right || staged_right,
                    "backfill recovery passed the right witness without overlap");
            latest = row.revision;
            if (row.revision == plan.left.revision || row.revision == plan.right.revision) {
                const auto& expected = row.revision == plan.left.revision ? plan.left : plan.right;
                require(row.session == expected.session && row_digest(e) == expected.sha256,
                        "backfill overlap row conflicts with original capture");
                (row.revision == plan.left.revision ? staged_left : staged_right) = true;
            }
            if (row.revision > plan.left.revision && row.revision < plan.right.revision)
                ++staged_between;
            break;
        }
        case Kind::TransactionCommit:
            require(in_frame, "backfill recovery commit without transaction");
            left |= staged_left;
            right |= staged_right;
            between += staged_between;
            in_frame = staged_left = staged_right = false;
            staged_between = 0;
            break;
        case Kind::ClearDeleted:
            require(table_is(e, "orders_log") && e.signed_value <= plan.start.revision,
                    "backfill recovery history was cleared after requested checkpoint");
            break;
        case Kind::Close:
            require(opened && !in_frame, "backfill recovery closed inside transaction");
            closed = true;
            break;
        default:
            break;
        }
    }
};

FullOrderLogRecoveryCursor cursor(const FullOrderLogBackfillPlan& plan) {
    return {plan.capture_identity, plan.reopen_sequence, plan.ordlog_lifenum, plan.start.revision, plan.matching_id};
}

std::string restricted_listener(std::int32_t matching) {
    return "p2repl://FORTS_ORDLOG_REPL_MATCH" + std::to_string(matching) + ";tables=orders_log";
}

std::string restricted_open(const FullOrderLogRecoveryCursor& request) {
    return "mode=snapshot+online;lifenum=" + std::to_string(request.life_number) +
           ";rev.orders_log=" + std::to_string(request.revision);
}

FullOrderLogBackfillCheck check_recovery(FullOrderLogBackfillPlan plan, const std::filesystem::path& path) {
    RecoveryScanner scan{plan};
    const auto recovery = read_full_order_log_capture(path, [&](const auto& record) { scan.visit(record.event); });
    require(recovery.capture_identity != plan.capture_identity && plan.isins == recovery.isins,
            "backfill recovery must be a separate capture of the original instruments");
    if (recovery.recovery_cursor)
        require(*recovery.recovery_cursor == cursor(plan), "backfill collector request does not match the plan");
    // An exact requested LifeNum is silent under CGate's open handshake.
    // Keep the actual resume request in the manifest, separate from callbacks.
    require(scan.complete() && (recovery.recovery_cursor || (scan.life && scan.closed)),
            "backfill recovery lacks committed overlap at both ends or generation proof");
    return {std::move(plan), recovery.capture_identity, scan.between};
}

using Clock = std::chrono::steady_clock;

class RecoveryCollector final : public Plaza2ListenerEventHandler {
  public:
    RecoveryCollector(const FullOrderLogBackfillPlan& plan, Clock::time_point deadline,
                      std::chrono::sys_seconds not_after, const std::function<bool()>& stopped)
        : scan_{plan}, deadline_(deadline), not_after_(not_after), stopped_(stopped) {}

    FullOrderLogCapture* capture{};
    bool closing{};

    bool wants_raw_replication(std::string_view) const noexcept override {
        return true;
    }

    void check_budget() const {
        require(Clock::now() < deadline_ &&
                    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) < not_after_ &&
                    (!stopped_ || !stopped_()),
                "backfill collection interrupted or deadline reached");
    }

    bool complete() const {
        return scan_.complete();
    }

    void check_callback() const {
        if (callback_failed_)
            throw std::runtime_error(error_.empty() ? "backfill listener callback failed" : error_);
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        try {
            if (callback_failed_)
                return {.code = Plaza2ErrorCode::DecodeFailed, .message = error_};
            // cg_conn_process may deliver more events before returning to the owner.
            if (complete() && !closing && event.kind != Kind::LifeNum)
                return {};
            if (!closing && !complete())
                check_budget();
            require(++events_ <= 2000000, "backfill callback budget exhausted");
            if (auto error = capture->record(event); error)
                return error;
            require(closing || event.kind != Kind::Close, "backfill listener closed before collector shutdown");
            require(!closing || event.kind == Kind::Close || event.kind == Kind::ReplState,
                    "backfill data delivered during collector shutdown");
            scan_.visit(event);
            return {};
        } catch (const std::exception& ex) {
            callback_failed_ = true;
            error_ = ex.what();
            return {.code = Plaza2ErrorCode::DecodeFailed, .message = error_};
        }
    }

    void on_plaza2_listener_error(const Plaza2Error& error) noexcept override {
        callback_failed_ = true;
        capture->record_error(kFullOrderLogStreamCode, error);
    }

  private:
    RecoveryScanner scan_;
    Clock::time_point deadline_;
    std::chrono::sys_seconds not_after_;
    const std::function<bool()>& stopped_;
    std::uint64_t events_{};
    std::string error_;
    bool callback_failed_{};
};

void check(const Plaza2Error& error) {
    if (error)
        throw std::runtime_error(error.message);
}
} // namespace

FullOrderLogBackfillOpens list_full_order_log_opens(const std::filesystem::path& capture) {
    FullOrderLogBackfillOpens opens;
    const auto summary = read_full_order_log_capture(
        capture,
        [&](const auto& record) {
            const auto& e = record.event;
            if (e.stream_code == kFullOrderLogStreamCode && e.kind == Kind::Open)
                opens.generations.push_back({record.sequence, std::string(e.text_value)});
        },
        true);
    opens.capture_identity = summary.capture_identity;
    opens.truncated = summary.truncated;
    opens.end_record_present = summary.end_record_present;
    return opens;
}

FullOrderLogBackfillPlan plan_full_order_log_backfill(const std::filesystem::path& capture, std::int32_t matching_id,
                                                      std::uint64_t ordlog_lifenum, std::uint64_t reopen_sequence) {
    require(matching_id > 0 && matching_id <= INT8_MAX && ordlog_lifenum <= UINT32_MAX,
            "backfill requires a valid matching ID and optional ORDLOG LifeNum");
    FullOrderLogBackfillPlan plan{.ordlog_lifenum = ordlog_lifenum, .matching_id = matching_id};
    PrimaryScanner scan{plan, reopen_sequence};
    auto summary = read_full_order_log_capture(capture, [&](const auto& record) { scan.visit(record); }, true);
    require(summary.end_record_present && !summary.recovery_cursor,
            "backfill planning requires a finalized original composite capture");
    require(plan.start.commit_sequence && plan.left.commit_sequence && plan.right.commit_sequence &&
                plan.start.session == plan.left.session && plan.left.session == plan.right.session &&
                plan.start.revision < plan.left.revision && plan.left.revision < plan.right.revision,
            "backfill requires committed, increasing boundaries in the same session");
    plan.capture_identity = summary.capture_identity;
    plan.isins = std::move(summary.isins);
    plan.listener_settings = restricted_listener(matching_id);
    plan.open_settings = restricted_open(cursor(plan));
    return plan;
}

FullOrderLogBackfillCheck check_full_order_log_backfill(const std::filesystem::path& capture,
                                                        const std::filesystem::path& recovery_capture,
                                                        std::int32_t matching_id, std::uint64_t ordlog_lifenum,
                                                        std::uint64_t reopen_sequence) {
    return check_recovery(plan_full_order_log_backfill(capture, matching_id, ordlog_lifenum, reopen_sequence),
                          recovery_capture);
}

FullOrderLogBackfillCheck
collect_full_order_log_backfill(const FullOrderLogBackfillPlan& plan, FullOrderLogCaptureOptions capture,
                                const Plaza2Settings& runtime, std::string_view connection_settings,
                                std::chrono::seconds timeout, std::chrono::sys_seconds not_after,
                                const std::function<bool()>& stopped) {
    const auto request = cursor(plan);
    require(runtime.environment == Plaza2Environment::Test && timeout.count() > 0 && timeout.count() <= 900 &&
                connection_settings.starts_with("p2tcp://127.0.0.1:4102;") && capture.directory.is_absolute() &&
                capture.max_bytes <= (4ULL << 30) && capture_format::valid_recovery_cursor(request) &&
                plan.start.revision < plan.left.revision && plan.left.revision < plan.right.revision &&
                plan.listener_settings == restricted_listener(plan.matching_id) &&
                plan.open_settings == restricted_open(request),
            "backfill collection requires a bounded TEST plan and the dedicated loopback router");
    const auto directory = capture.directory;
    capture.recovery_cursor = request;
    RecoveryCollector handler(plan, Clock::now() + timeout, not_after, stopped);
    handler.check_budget();
    FullOrderLogCapture recorder(std::move(capture), plan.isins, handler);
    handler.capture = &recorder;
    Plaza2Env env;
    Plaza2Connection connection;
    Plaza2Listener listener;
    try {
        check(env.open(runtime));
        handler.check_budget();
        check(connection.create(env, connection_settings));
        check(connection.open(""));
        std::uint32_t state{};
        for (;;) {
            handler.check_budget();
            check(connection.state(state));
            if (state == 3)
                break;
            require(state == 2, "backfill connection did not become active");
            check(connection.process(1));
        }
        check(listener.create(connection, kFullOrderLogStreamCode, plan.listener_settings, &handler));
        handler.check_budget();
        check(listener.open(plan.open_settings));
        handler.check_callback();
        while (!handler.complete()) {
            handler.check_budget();
            const auto process_error = connection.process(1);
            handler.check_callback();
            if (handler.complete())
                break;
            check(process_error);
            check(connection.state(state));
            require(state == 3, "backfill connection interrupted");
            check(listener.state(state));
            require(state == 2 || state == 3, "backfill listener interrupted");
            recorder.poll();
            require(!recorder.metrics().failed, "backfill capture failed");
        }
        handler.closing = true;
        check(listener.state(state));
        if (state != 0)
            check(listener.close());
        check(listener.destroy());
        handler.check_callback();
        check(connection.state(state));
        if (state != 0)
            check(connection.close());
        check(connection.destroy());
        check(env.close());
    } catch (...) {
        handler.closing = true;
        static_cast<void>(listener.close());
        static_cast<void>(listener.destroy());
        static_cast<void>(connection.close());
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        try {
            recorder.finish(FullOrderLogCaptureEndStatus::CaptureFailed, 2);
        } catch (...) {
        }
        throw;
    }
    recorder.finish();
    return check_recovery(plan, directory);
}
} // namespace moex::plaza2::cgate
