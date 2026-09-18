#include "plaza2_day_observer_journal.hpp"

#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

namespace {
using namespace moex::plaza2;
using namespace cgate;
using namespace observer;
using generated::FieldCode;
using generated::StreamCode;
using generated::TableCode;
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
    stopping = 1;
}

void check(Plaza2Error error) {
    if (error)
        throw error;
}

std::string file_hash(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("missing pinned T1 router config");
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return plaza2_sha256_hex(bytes);
}

// Exercise the exact callback journal without loading CGate or opening sockets.
void fixture(Journal& journal) {
    Handler aggr(journal, StreamCode::kFortsAggrRepl, 1);
    const auto emit = [&](Plaza2ListenerEventKind kind, std::span<const Plaza2DecodedFieldValue> fields = {}) {
        check(aggr.on_plaza2_listener_event(
            {.kind = kind, .table_code = TableCode::kFortsAggrReplSysEvents, .fields = fields, .unsigned_value = 42}));
    };
    const std::string cyrillic = "\xd4\xfc\xfe\xf7\xe5\xf0\xf1\xed\xfb\xe9 \xea\xee\xed\xf2\xf0\xe0\xea\xf2 ALRS-12.26";
    const std::string decoded_cyrillic = text::windows1251_to_utf8(cyrillic);
    const std::array fields = {Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsEventId,
                                                       .kind = Plaza2DecodedValueKind::SignedInteger,
                                                       .signed_value = 678984},
                               Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsSessId,
                                                       .kind = Plaza2DecodedValueKind::SignedInteger,
                                                       .signed_value = 11709},
                               Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsEventType,
                                                       .kind = Plaza2DecodedValueKind::SignedInteger,
                                                       .signed_value = 1},
                               Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsMessage,
                                                       .kind = Plaza2DecodedValueKind::String,
                                                       .text_value = "session_data_ready"},
                               Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsServerTime,
                                                       .kind = Plaza2DecodedValueKind::Timestamp,
                                                       .unsigned_value = 123456}};
    using K = Plaza2ListenerEventKind;
    emit(K::Open);
    emit(K::LifeNum);
    // Ten thousand irrelevant transactions must cause no evidence writes.
    for (int i = 0; i < 10000; ++i) {
        emit(K::TransactionBegin);
        check(
            aggr.on_plaza2_listener_event({.kind = K::StreamData, .table_code = TableCode::kFortsAggrReplOrdersAggr}));
        emit(K::TransactionCommit);
    }
    emit(K::TransactionBegin);
    check(aggr.on_plaza2_listener_event({.kind = K::StreamData, .table_code = TableCode::kFortsAggrReplOrdersAggr}));
    emit(K::StreamData, fields);
    emit(K::TransactionCommit);
    emit(K::Online);
    emit(K::TransactionBegin);
    emit(K::StreamData, fields);
    emit(K::TransactionCommit);
    emit(K::TransactionBegin);
    emit(K::StreamData, fields);
    emit(K::ClearDeleted);
    emit(K::TransactionCommit);
    emit(K::TransactionBegin);
    emit(K::StreamData, fields);
    emit(K::LifeNum);
    emit(K::Close);
    emit(K::Open);
    emit(K::Online);
    emit(K::TransactionBegin);
    emit(K::StreamData, fields);
    emit(K::TransactionCommit);
    emit(K::Close);
    Handler diagnostics(journal, StreamCode::kFortsAggrRepl, 99);
    diagnostics.on_plaza2_listener_error({Plaza2ErrorCode::DecodeFailed, 10, "bad fixed string"});
    diagnostics.on_plaza2_listener_error({Plaza2ErrorCode::DecodeFailed, 11, "ini=t1.ini;key=secret"});
    // Fresh-generation identity and state rows, including exact exchange CP1251 bytes.
    for (const auto stream :
         {StreamCode::kFortsRefdataRepl, StreamCode::kFortsSessionstateRepl, StreamCode::kFortsInstrumentstateRepl}) {
        Handler handler(journal, stream, 2);
        const auto table = stream == StreamCode::kFortsRefdataRepl ? TableCode::kFortsRefdataReplFutInstruments
                           : stream == StreamCode::kFortsSessionstateRepl
                               ? TableCode::kFortsSessionstateReplSessionState
                               : TableCode::kFortsInstrumentstateReplInstrumentState;
        std::vector<Plaza2DecodedFieldValue> row;
        for (const auto& desc : generated::FieldDescriptors()) {
            if (desc.table_id != static_cast<std::uint32_t>(table))
                continue;
            if (desc.field_name == "sess_id" || desc.field_name == "isin_id")
                row.push_back({.field_code = desc.field_code,
                               .kind = Plaza2DecodedValueKind::SignedInteger,
                               .signed_value = desc.field_name == "sess_id" ? 11709 : 123});
            if (desc.field_name == "name")
                row.push_back({.field_code = desc.field_code,
                               .kind = Plaza2DecodedValueKind::String,
                               .text_value = decoded_cyrillic,
                               .raw_value = std::as_bytes(std::span(cyrillic.data(), cyrillic.size()))});
        }
        for (auto kind : {K::Open, K::TransactionBegin, K::StreamData, K::TransactionCommit, K::Online, K::Close})
            check(handler.on_plaza2_listener_event({.kind = kind, .table_code = table, .fields = row}));
    }
    journal.append("\"kind\":\"fixture_text\",\"undefined\":" + quote("\x98") + ",\"escaped\":" + quote("\"\\\n\r\t"));
}

int run(int argc, char** argv) {
    Plaza2Settings settings;
    settings.environment = Plaza2Environment::Test;
    settings.expected_spectra_release = "SPECTRA9.9.0";
    settings.expected_runtime_library_sha256 = "f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f";
    settings.expected_scheme_sha256 = "7b93117ee435fd0cb2849b677fc32a9d581364b6ee9afeac9c6c002875400746";
    std::filesystem::path output;
    std::uint32_t seconds = 86400;
    bool offline = false, armed = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--offline-fixture") {
            offline = true;
            continue;
        }
        if (arg == "--armed-test-read-only") {
            armed = true;
            continue;
        }
        if (arg == "--help") {
            std::cout << "Usage: moex_plaza2_day_observer --output FILE [--offline-fixture]\n"
                         "  --runtime-root DIR --library-path FILE --scheme-dir DIR --config-dir DIR\n"
                         "  --armed-test-read-only [--observation-seconds N]\n"
                         "Pinned T1 only: 127.0.0.1:4101, key from MOEX_PLAZA2_CGATE_SOFTWARE_KEY.\n";
            return 0;
        }
        if (++i == argc)
            throw std::runtime_error("missing argument value");
        const std::string_view value = argv[i];
        if (arg == "--output")
            output = value;
        else if (arg == "--runtime-root")
            settings.runtime_root = value;
        else if (arg == "--library-path")
            settings.library_path = value;
        else if (arg == "--scheme-dir")
            settings.scheme_dir = value;
        else if (arg == "--config-dir")
            settings.config_dir = value;
        else if (arg == "--observation-seconds") {
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (error != std::errc{} || end != value.data() + value.size() || seconds == 0 || seconds > 604800)
                throw std::runtime_error("observation seconds must be 1..604800");
        } else
            throw std::runtime_error("unsupported observer option");
    }
    if (output.empty())
        throw std::runtime_error("--output required");
    Journal journal(output);
    if (offline) {
        fixture(journal);
        journal.finish();
        return 0;
    }
    if (!armed || settings.library_path.empty() || settings.scheme_dir.empty() || settings.config_dir.empty())
        throw std::runtime_error("explicit pinned TEST runtime and read-only arm required");
    // Pin the same T1 router configuration as the validated authority evidence runner.
    if (file_hash(settings.config_dir / "t1.ini") != "30ff2ec566edac9e7b8d8d255c1fa2161edc77f4741076ded9b80fd9ad573d68")
        throw std::runtime_error("T1 router configuration hash mismatch");
    const char* key = std::getenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY");
    if (!key || !*key || std::string_view(key).find_first_of(";\r\n") != std::string_view::npos)
        throw std::runtime_error("missing or invalid software key");
    settings.env_open_settings =
        "ini=" + std::filesystem::absolute(settings.config_dir / "t1.ini").string() + ";key=" + key;
    check(validate_plaza2_settings(settings));
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    journal.append_durable(
        "\"kind\":\"start\",\"journal_format\":2,\"order_entry_allowed\":false,\"endpoint\":\"127.0.0.1:4101\","
        "\"text_policy\":\"runtime_utf8_and_raw_hex\",\"observation_seconds\":" +
        std::to_string(seconds));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    Plaza2Env env;
    check(env.open(settings));
    const std::array streams = {StreamCode::kFortsAggrRepl, StreamCode::kFortsRefdataRepl,
                                StreamCode::kFortsSessionstateRepl, StreamCode::kFortsInstrumentstateRepl};
    const auto scheme = std::filesystem::absolute(settings.scheme_dir / "forts_scheme.ini").string();
    const std::array urls = {"p2repl://FORTS_AGGR20_REPL;scheme=|FILE|" + scheme + "|Aggr",
                             "p2repl://FORTS_REFDATA_REPL;scheme=|FILE|" + scheme + "|REFDATA",
                             std::string("p2repl://FORTS_SESSIONSTATE_REPL"),
                             std::string("p2repl://FORTS_INSTRUMENTSTATE_REPL")};
    std::uint64_t generation = 0;
    bool all_online = false, had_gap = false;
    while (!stopping && std::chrono::steady_clock::now() < deadline) {
        ++generation;
        journal.append_durable("\"kind\":\"generation_begin\",\"generation\":" + std::to_string(generation));
        Plaza2Connection connection;
        std::array<std::unique_ptr<Handler>, 4> handlers;
        std::array<Plaza2Listener, 4> listeners;
        bool retry = false;
        try {
            check(connection.create(env, "p2tcp://127.0.0.1:4101;app_name=read_only_day_observer_" +
                                             std::to_string(::getpid())));
            check(connection.open(""));
            for (std::size_t i = 0; i < streams.size(); ++i) {
                handlers[i] = std::make_unique<Handler>(journal, streams[i], generation);
                check(listeners[i].create(connection, streams[i], urls[i], handlers[i].get()));
                check(listeners[i].open("mode=snapshot+online"));
                journal.append_durable(
                    "\"kind\":\"listener_open_accepted\",\"generation\":" + std::to_string(generation) +
                    ",\"stream\":" + quote(generated::FindStreamByCode(streams[i])->stream_name));
            }
            auto heartbeat = std::chrono::steady_clock::now();
            while (!stopping && std::chrono::steady_clock::now() < deadline) {
                check(connection.process(50));
                std::uint32_t connection_state = 0;
                check(connection.state(connection_state));
                if (connection_state == 0 || connection_state == 1)
                    throw Plaza2Error{Plaza2ErrorCode::RuntimeCallFailed, connection_state, "connection inactive"};
                all_online = true;
                for (std::size_t i = 0; i < streams.size(); ++i) {
                    check(listeners[i].last_callback_error());
                    if (handlers[i]->failed)
                        throw std::runtime_error("observer evidence failure");
                    if (handlers[i]->closed || handlers[i]->recovery_requested)
                        throw Plaza2Error{Plaza2ErrorCode::RuntimeCallFailed, 0, "listener closed"};
                    std::uint32_t state = 0;
                    check(listeners[i].state(state));
                    if (state == 0 || state == 1)
                        throw Plaza2Error{Plaza2ErrorCode::RuntimeCallFailed, state, "listener inactive"};
                    all_online = all_online && handlers[i]->online;
                }
                if (std::chrono::steady_clock::now() >= heartbeat) {
                    const auto io = journal.metrics();
                    journal.append_durable("\"kind\":\"heartbeat\",\"generation\":" + std::to_string(generation) +
                                           ",\"all_online\":" + (all_online ? "true" : "false") +
                                           ",\"journal_bytes_written\":" + std::to_string(io.bytes) +
                                           ",\"journal_write_calls\":" + std::to_string(io.write_calls) +
                                           ",\"journal_sync_calls\":" + std::to_string(io.sync_calls) +
                                           ",\"max_write_ns\":" + std::to_string(io.max_write_ns) +
                                           ",\"max_sync_ns\":" + std::to_string(io.max_sync_ns) +
                                           ",\"peak_buffer_bytes\":" + std::to_string(io.peak_buffer_bytes));
                    heartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                }
            }
        } catch (const Plaza2Error& error) {
            // cg_conn_process may wrap a callback failure as a runtime error.
            // Never retry a failed decoder or evidence writer underneath that wrapper.
            for (std::size_t i = 0; i < streams.size(); ++i) {
                check(listeners[i].last_callback_error());
                if (handlers[i] && handlers[i]->failed)
                    throw std::runtime_error("observer callback failure");
            }
            // Decode/schema errors remain terminal. Availability failures are visible gaps;
            // the next generation always requests fresh snapshots, never stale replstate.
            journal.append_durable("\"kind\":\"generation_error\",\"generation\":" + std::to_string(generation) +
                                   ",\"code\":" + std::to_string(static_cast<unsigned>(error.code)) +
                                   ",\"runtime_code\":" + std::to_string(error.runtime_code) +
                                   ",\"message\":" + quote(diagnostic(error.message)));
            if (error.code != Plaza2ErrorCode::RuntimeCallFailed)
                throw;
            retry = true;
            had_gap = true;
            all_online = false;
        }
        for (std::size_t i = 0; i < listeners.size(); ++i) {
            auto& listener = listeners[i];
            if (listener.is_created()) {
                if (const auto error = listener.close(); error)
                    journal.append_durable("\"kind\":\"cleanup_close_error\",\"runtime_code\":" +
                                           std::to_string(error.runtime_code));
                check(listener.last_callback_error());
                check(listener.destroy());
                journal.append_durable("\"kind\":\"listener_destroyed\",\"generation\":" + std::to_string(generation) +
                                       ",\"stream\":" + quote(generated::FindStreamByCode(streams[i])->stream_name));
            }
        }
        if (connection.is_created()) {
            if (const auto error = connection.close(); error)
                journal.append_durable("\"kind\":\"cleanup_connection_close_error\",\"runtime_code\":" +
                                       std::to_string(error.runtime_code));
            check(connection.destroy());
        }
        journal.append_durable("\"kind\":\"generation_end\",\"generation\":" + std::to_string(generation));
        if (!retry)
            break;
        for (int tick = 0; tick < 50 && !stopping && std::chrono::steady_clock::now() < deadline; ++tick)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(env.close());
    journal.append_durable("\"kind\":\"end\",\"reason\":" + quote(stopping ? "signal" : "deadline") +
                           ",\"all_online_at_end\":" + (all_online ? "true" : "false") +
                           ",\"had_gap\":" + (had_gap ? "true" : "false"));
    journal.finish();
    return stopping || had_gap || !all_online ? 2 : 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const Plaza2Error& error) {
        // Runtime error text can contain settings. Keep secrets out of evidence/stderr.
        std::cerr << "observer failed: code=" << static_cast<unsigned>(error.code)
                  << " runtime_code=" << error.runtime_code << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
