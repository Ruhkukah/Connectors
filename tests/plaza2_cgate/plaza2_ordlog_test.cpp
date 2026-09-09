#include "moex/plaza2/cgate/plaza2_ordlog.hpp"
#include "plaza2_runtime_test_support.hpp"
#include <dlfcn.h>
#include <iostream>
#include <limits>

using namespace moex::plaza2;
using namespace moex::plaza2::cgate;
using moex::plaza2::test::require;
using Kind = Plaza2ListenerEventKind;
constexpr auto stream = generated::StreamCode::kFortsOrdlogRepl;

Plaza2ListenerEvent control(Kind kind) {
    return {.kind = kind, .stream_code = stream};
}
void send(Plaza2Ordlog& pipe, Plaza2ListenerEvent event) {
    const auto error = pipe.on_plaza2_listener_event(event);
    require(!error, error.message);
}
struct Wire {
    std::array<std::byte, 148> bytes{};
    Plaza2ListenerEvent event{};
    explicit Wire(std::size_t index, std::int64_t rev = 1) {
        const auto& table = public_wire::kTables[index];
        event = {.kind = Kind::StreamData,
                 .stream_code = stream,
                 .table_code = table.code,
                 .raw_payload = {bytes.data(), table.size},
                 .signed_value = rev,
                 .table_index = index};
        std::memcpy(bytes.data(), &rev, 8);
        std::memcpy(bytes.data() + 8, &rev, 8);
        for (const auto& f : table.fields) {
            if (f.type == "d16.5") {
                const public_wire::Bcd16_5 price{5, 16, 0, 0, 0, 0, 0, 12, 34, 56, 70};
                std::memcpy(bytes.data() + f.offset, price.data(), price.size());
            }
        }
    }
};
void drain(Plaza2Ordlog& pipe) {
    while (pipe.acknowledge()) {
    }
}
void transaction(Plaza2Ordlog& pipe, Wire& row) {
    send(pipe, control(Kind::TransactionBegin));
    send(pipe, row.event);
    send(pipe, control(Kind::TransactionCommit));
}

void replay_checks() {
    Plaza2Ordlog pipe(32, 1);
    pipe.poll_time(100);
    send(pipe, control(Kind::Open));
    send(pipe, {.kind = Kind::LifeNum, .stream_code = stream, .unsigned_value = 7});
    drain(pipe);
    for (std::size_t index = 0; index < 4; ++index) {
        Wire row(index);
        transaction(pipe, row);
        require(pipe.front() && pipe.front()->table_index == index, "all four tables exposed");
        require(pipe.front()->bytes().size() == row.event.raw_payload.size(), "complete wire size");
        require(std::memcmp(pipe.front()->wire.data(), row.bytes.data(), row.event.raw_payload.size()) == 0,
                "raw bytes preserved exactly");
        drain(pipe);
    }
    Wire row(0, 2);
    transaction(pipe, row);
    transaction(pipe, row);
    require(pipe.metrics().duplicates == 1, "duplicate is labeled, retained");
    Wire older(0, 1);
    transaction(pipe, older);
    require(pipe.metrics().replays == 1, "history replay is legal");
    Wire jump(0, 10);
    transaction(pipe, jump);
    require(pipe.metrics().discontinuities == 1 && pipe.metrics().dropped == 0, "numeric jump is not proven loss");
    send(pipe, control(Kind::Online));
    require(pipe.state() == OrdlogState::CatchingUp, "mandatory catch-up required");
    send(pipe, {.kind = Kind::ReplState, .stream_code = stream, .text_value = "opaque-vendor-token"});
    require(pipe.checkpoint().empty(), "no checkpoint before processing");
    drain(pipe);
    require(pipe.state() == OrdlogState::Online && pipe.checkpoint() == "opaque-vendor-token",
            "acknowledged checkpoint");
    require(pipe.optional_overflowed() && pipe.metrics().optional_first_lost_sequence == 2, "optional loss explicit");
    require(pipe.metrics().mandatory_overflows == 0, "optional backlog does not block mandatory");
    send(pipe, control(Kind::TransactionBegin));
    require(pipe.checkpoint().empty(), "new incomplete transaction invalidates eligibility");
    require(static_cast<bool>(pipe.on_plaza2_listener_event(older.event)), "online backward conflict fails");
    require(pipe.state() == OrdlogState::Failed && pipe.metrics().revision_failures == 1, "corruption health");
}

void failure_checks() {
    for (int variant = 0; variant < 5; ++variant) {
        Plaza2Ordlog pipe(2);
        send(pipe, control(Kind::TransactionBegin));
        Wire row(0);
        if (variant == 0)
            row.event.table_index = 999;
        if (variant == 1)
            row.event.table_code = kNoTableCode;
        if (variant == 2)
            row.event.raw_payload = row.event.raw_payload.first(10);
        if (variant == 3) {
            for (const auto& f : public_wire::kTables[0].fields) {
                if (f.type == "d16.5")
                    row.bytes[f.offset] = std::byte{0xFF};
            }
        }
        std::array<std::uint8_t, 17> nulls{};
        if (variant == 4) {
            nulls[0] = 1;
            row.event.raw_nulls = nulls;
        }
        require(static_cast<bool>(pipe.on_plaza2_listener_event(row.event)), "malformed input rejected");
        require(pipe.metrics().decode_failures == 1 && pipe.checkpoint().empty(), "malformed input blocks checkpoint");
    }
    Plaza2Ordlog pipe(1);
    Wire row(0);
    send(pipe, control(Kind::TransactionBegin));
    send(pipe, row.event);
    require(!pipe.front(), "uncommitted events are invisible");
    require(static_cast<bool>(pipe.on_plaza2_listener_event(row.event)), "bounded transaction overflow");
    require(pipe.metrics().mandatory_overflows == 1 && !pipe.front(), "overflow rolls back uncommitted transaction");
    send(pipe, {.kind = Kind::ReplState, .stream_code = stream, .text_value = "unsafe"});
    require(pipe.checkpoint().empty(), "later replstate cannot mask mandatory failure");
    Plaza2Ordlog restarted(1);
    require(restarted.checkpoint().empty(), "crash before ack cannot acquire an eligible token");
}

void generation_checks() {
    Plaza2Ordlog pipe(16);
    Wire row(0, 10);
    transaction(pipe, row);
    drain(pipe);
    send(pipe, control(Kind::Online));
    send(pipe, {.kind = Kind::ClearDeleted,
                .stream_code = stream,
                .table_code = row.event.table_code,
                .signed_value = std::numeric_limits<std::int64_t>::max(),
                .clear_deleted_flags = 0x80000000U});
    require(pipe.front()->flags == 0x80000000U, "clear-deleted flags retained");
    drain(pipe);
    Wire next(0, 1);
    transaction(pipe, next);
    drain(pipe);
    send(pipe, {.kind = Kind::LifeNum, .stream_code = stream, .unsigned_value = 8});
    require(pipe.state() == OrdlogState::History && pipe.front()->life == 8, "new life invalidates online generation");
    drain(pipe);
    transaction(pipe, next);
    require(pipe.metrics().revision_failures == 0, "life and max revision marker allow reset");
    drain(pipe);
    send(pipe, control(Kind::TransactionBegin));
    send(pipe, next.event);
    send(pipe, control(Kind::Close));
    require(!pipe.front() && pipe.checkpoint().empty() && pipe.state() == OrdlogState::Stale,
            "close rolls back partial transaction");
}

void runtime_checks(const char* library) {
    using namespace moex::plaza2::test;
    const auto root = make_temp_directory("ordlog_runtime");
    const auto fixture = materialize_runtime_fixture(
        root, library, Plaza2Environment::Test, build_vendor_like_runtime_scheme("SPECTRA99", "990.1.6.42752", "test"));
    {
        Plaza2Settings settings;
        settings.environment = Plaza2Environment::Test;
        settings.runtime_root = fixture.root;
        settings.env_open_settings = "ini=config/t1.ini;key=00000000";
        Plaza2Env env;
        require(!env.open(settings), "fake environment");
        Plaza2Connection connection;
        require(!connection.create(env, "p2tcp://127.0.0.1:4001;app_name=ordlog_test"), "fake connection create");
        require(!connection.open({}), "fake connection open");
        void* module = dlopen(fixture.library_path.c_str(), RTLD_NOW);
        require(module != nullptr, "open fake driver");
        using Emit =
            std::uint32_t (*)(std::uint32_t, std::size_t, void*, std::size_t, std::int64_t, std::uint8_t*, std::size_t);
        const auto emit = reinterpret_cast<Emit>(dlsym(module, "moex_fake_ordlog_emit"));
        require(emit != nullptr, "fake callback driver");
        const auto opened = reinterpret_cast<const char* (*)()>(dlsym(module, "moex_fake_ordlog_open_settings"));
        require(opened != nullptr, "fake open-settings capture");
        {
            Plaza2Ordlog pipe(16);
            require(!pipe.create(connection, fixture.scheme_path.string()), "raw listener create");
            require(!pipe.open(), "qualified scheme opens");
            require(emit(0x200, 0, nullptr, 0, 0, nullptr, 0) == 0, "callback begin");
            for (std::size_t index = 0; index < 4; ++index) {
                Wire row(index);
                require(emit(0x120, index, row.bytes.data(), row.event.raw_payload.size(), 1, nullptr, 0) == 0,
                        "wire callback dispatch/decode");
            }
            require(!pipe.front(), "callback transaction staging");
            require(emit(0x210, 0, nullptr, 0, 0, nullptr, 0) == 0, "callback commit");
            require(pipe.metrics().records == 4, "all callbacks accounted");
            require(static_cast<bool>(pipe.open()), "reopen cannot discard pending mandatory work");
            drain(pipe);
            char marker[] = "life=7;rev.orders_log=1";
            require(emit(0x1115, 0, marker, sizeof(marker), 0, nullptr, 0) == 0, "callback replstate");
            require(!pipe.checkpoint().empty(), "processed marker eligible");
            require(emit(0x101, 0, nullptr, 0, 0, nullptr, 0) == 0, "callback listener loss");
            require(pipe.state() == OrdlogState::Stale, "listener loss immediately invalidates health");
            require(!pipe.supervise(1) && pipe.state() == OrdlogState::Recovering, "bounded recovery starts");
            require(!pipe.supervise(1000000001ULL) && pipe.state() == OrdlogState::History, "listener reopened");
            require(std::string_view(opened()) == "replstate=life=7;rev.orders_log=1",
                    "exact acknowledged token used on reopen");
            require(emit(0x200, 0, nullptr, 0, 0, nullptr, 0) == 0, "replay transaction");
            Wire replay(0);
            require(emit(0x120, 0, replay.bytes.data(), 128, 1, nullptr, 0) == 0, "legal continuation replay");
            require(emit(0x210, 0, nullptr, 0, 0, nullptr, 0) == 0, "replay commit");
            require(pipe.front() && pipe.front()->revision == OrdlogRevision::Duplicate,
                    "replayed record retained after restart");
            drain(pipe);
            require(emit(0x200, 0, nullptr, 0, 0, nullptr, 0) == 0, "corrupt transaction");
            Wire row(0);
            require(emit(0x120, 999, row.bytes.data(), 128, 1, nullptr, 0) != 0, "bad callback index rejected");
            require(pipe.state() == OrdlogState::Failed && pipe.checkpoint().empty(),
                    "runtime failure invalidates checkpoint");
        }
        {
            ::setenv("MOEX_FAKE_ORDLOG_BAD_SCHEME", "1", 1);
            Plaza2Ordlog pipe;
            require(!pipe.create(connection, fixture.scheme_path.string()), "bad fixture create");
            require(static_cast<bool>(pipe.open()), "wrong native scheme size fails before records");
            require(pipe.state() == OrdlogState::Failed, "wrong scheme health fails");
            ::unsetenv("MOEX_FAKE_ORDLOG_BAD_SCHEME");
        }
        dlclose(module);
    }
    remove_tree(root);
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected fake runtime path");
        replay_checks();
        failure_checks();
        generation_checks();
        runtime_checks(argv[1]);
        std::cout << "ORDLOG wire callbacks, replay, generation, bounded delivery and checkpoint tests PASS\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
