#include "fake_cgate_abi.hpp"
#include "plaza2_c2_reference.hpp"
#include "moex/plaza2/cgate/plaza2_public_decode.hpp"
#include "plaza2_runtime_test_support.hpp"
#include <algorithm>
#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <numeric>
#include <memory>
#include <random>
#include <stdexcept>

using moex::plaza2::test::require;
namespace wire = moex::plaza2::public_wire;
using namespace c2;

Order order(std::int64_t id = 1, int pair = 0) {
    return {id, 7, 100 + id % 4096, 1 + id % 2, 120000, 10, 0, 0, pair};
}
Event event(std::int64_t id, std::int64_t rev, int action = 1, std::int64_t rest = 10, int pair = 0) {
    auto o = order(id, pair);
    o.remaining = rest;
    return {o, rev, 10, action};
}
void bootstrap(Model& m, const Book& book = {}, std::int64_t r = 0) {
    m.open();
    m.opened();
    m.lifenum(7);
    m.begin();
    for (const auto& [key, o] : book)
        m.snapshot(o);
    m.info(1, r, 7, 1);
    m.commit();
    m.online_event();
    require(m.state == State::CatchingUp, "ONLINE does not drain committed work");
    m.drain();
    require(m.state == State::Ready, "complete bootstrap");
}
void mutation_cases() {
    Mutation m;
    auto add = event(1, 1);
    add.amount = 99;
    require(m.apply(add) == Result::Valid && m.book.begin()->second.remaining == 10,
            "insert direct rest, no equality guess");
    require(m.apply(add) == Result::Duplicate, "exact last duplicate");
    auto partial = event(1, 2, 2, 4);
    partial.amount = 999;
    require(m.apply(partial) == Result::Valid && m.book.begin()->second.remaining == 4, "execution uses direct rest");
    require(m.apply(add) == Result::Replay, "known identical historical replay");
    require(m.apply(event(1, 3, 2, 0)) == Result::Valid && m.book.empty(), "full execution terminal");
    require(m.apply(event(2, 4)) == Result::Valid, "second add");
    require(m.apply(event(2, 5, 0, 10)) == Result::Valid && m.book.empty(),
            "cancel terminal even nonzero payload rest");
    for (int variant = 0; variant < 10; ++variant) {
        Mutation x;
        require(x.apply(event(1, 1)) == Result::Valid, "seed anomaly");
        auto bad = event(1, 2, 2, 4);
        switch (variant) {
        case 0:
            bad.action = 1;
            break;
        case 1:
            bad.order.id = 2;
            break;
        case 2:
            bad.order.id = 2;
            bad.action = 0;
            break;
        case 3:
            bad.order.remaining = 11;
            break;
        case 4:
            bad.order.remaining = -1;
            break;
        case 5:
            bad.order.side = 1;
            break;
        case 6:
            ++bad.order.price;
            break;
        case 7:
            ++bad.order.instrument;
            break;
        case 8:
            bad.revision = 1;
            break;
        case 9:
            bad.action = 3;
            break;
        }
        const auto before = x.book;
        const auto result = x.apply(bad);
        require(result == Result::NeedsResync || result == Result::Fatal, "contradiction rejected");
        require(x.book == before, "no silent repair");
    }
}
void recovery_cases() {
    for (int position = 0; position < 6; ++position) {
        Model m;
        bootstrap(m, {{Key{0, 1}, order()}});
        if (position < 2) {
            m.open();
            m.opened();
            m.lifenum(7);
            m.begin();
            m.snapshot(order());
            if (position == 1) {
                m.info(1, 0, 7, 1);
                m.commit();
            }
        }
        if (position == 3) {
            m.begin();
            m.row(event(1, 1, 2, 5));
        }
        if (position == 4) {
            m.begin();
            m.row(event(1, 1, 2, 5));
            m.commit();
        }
        if (position == 5) {
            m.close();
            m.retry(0);
        }
        const auto view = m.generation;
        m.lifenum(8);
        require(m.generation != view && !m.current(view) && m.data.book.empty() && m.data.seen.empty() &&
                    m.data.frontier == std::array<std::int64_t, 2>{} && !m.usable && !m.online && !m.transaction &&
                    !m.committed && !m.drained && m.pending.empty(),
                "LifeNum invalidates every derived object");
    }
    for (int position = 0; position < 3; ++position) {
        for (int error = 0; error < 2; ++error) {
            Model m;
            m.open();
            m.opened();
            m.lifenum(7);
            m.begin();
            m.snapshot(order());
            if (position > 0) {
                m.info(1, 1, 7, 1);
                m.commit();
            }
            if (position > 1) {
                m.online_event();
                m.drain();
            }
            const auto old = m.generation;
            if (error)
                m.fail();
            else
                m.close();
            require(!m.current(old) && m.data.book.empty(), "close/error cannot serve stale book");
            require(!m.retry(0) && !m.retry(999) && m.retry(1000), "bounded fresh retry");
            require(m.state == State::Opening && !m.life && !m.boundary, "fresh composite, no raw token");
        }
    }
    Model fatal;
    bootstrap(fatal);
    fatal.fail(true);
    fatal.close();
    fatal.online_event();
    fatal.drain();
    require(fatal.state == State::Failed && !fatal.retry(1000),
            "CLOSE or later callback cannot downgrade fatal corruption");
    Model retention;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        retention.fail();
        require(!retention.retry(attempt * 2000), "history-equivalent failure waits");
        const bool reopened = retention.retry(attempt * 2000 + 1000);
        require(reopened == (attempt < 3), "retry budget bounded");
    }
    require(retention.state == State::Failed, "exhaustion cannot become Ready");
    for (int table = 0; table < 4; ++table) {
        Model m;
        bootstrap(m, {{Key{table / 2, 1}, order(1, table / 2)}});
        const auto before = m.data.book;
        m.clear_deleted(false);
        require(m.data.book == before && m.state == State::Ready, "unrelated table marker");
        m.clear_deleted(true, true);
        require(m.data.book == before && m.state == State::Ready, "proven history compaction is not cancel");
        m.clear_deleted(true);
        require(m.state == State::NeedsResync && m.data.book.empty(), "ambiguous relevant marker invalidates");
    }
    for (int variant = 0; variant < 8; ++variant) {
        Model m;
        m.open();
        m.opened();
        m.lifenum(7);
        m.begin();
        m.snapshot(order());
        if (variant != 0)
            m.info(1, 1, variant == 1 ? 8 : 7, variant == 2 ? 0 : variant == 3 ? 2 : 1);
        if (variant == 4)
            m.info(2, 1, 7, 1);
        if (variant == 5)
            m.info(1, 2, 7, 1);
        if (variant == 6)
            m.info(1, 1, 7, 0);
        if (variant != 7 && m.state != State::NeedsResync)
            m.commit();
        m.online_event();
        m.drain();
        require(m.state != State::Ready, "invalid/partial/uncommitted snapshot never Ready");
    }
    Model multiple;
    multiple.open();
    multiple.opened();
    multiple.lifenum(7);
    multiple.begin();
    multiple.info(1, 9, 7, 0);
    multiple.snapshot(order());
    multiple.commit();
    multiple.begin();
    multiple.snapshot(order(2));
    multiple.info(1, 9, 7, 1);
    multiple.commit();
    multiple.online_event();
    multiple.drain();
    require(multiple.state == State::Ready, "conditional multi-transaction publication");
    Model overflow;
    bootstrap(overflow);
    overflow.begin();
    for (std::size_t i = 0; i <= Model::capacity; ++i)
        overflow.row(event(1, 1));
    require(overflow.state == State::NeedsResync && overflow.pending.empty(), "mandatory overflow fails closed");
}

// Negotiated descriptors are read on each OPEN. All fixture indices below are deliberately synthetic.
struct Consumer {
    Model model;
    int pair{};
    std::vector<const wire::Table*> mapping;
    std::uint32_t (*getscheme)(void*, void**){};
    bool bind(void* listener) {
        CgSchemeDesc* scheme{};
        if (getscheme(listener, reinterpret_cast<void**>(&scheme)) || !scheme || scheme->num_messages != 3)
            return false;
        mapping.clear();
        std::array<bool, 3> found{};
        const std::array<std::size_t, 3> expected{pair ? 1U : 0U, 6, pair ? 5U : 4U};
        auto* message = scheme->messages;
        for (std::size_t i = 0; i < scheme->num_messages; ++i) {
            if (!message || !message->name)
                return false;
            const wire::Table* selected{};
            for (std::size_t j = 0; j < expected.size(); ++j) {
                const auto& table = wire::kTables[expected[j]];
                if (table.name == message->name) {
                    if (found[j])
                        return false;
                    found[j] = true;
                    selected = &table;
                }
            }
            if (!selected || selected->size != message->size || selected->fields.size() != message->num_fields)
                return false;
            auto* field = message->fields;
            for (const auto& locked : selected->fields) {
                if (!field || !field->name || !field->type || locked.name != field->name ||
                    locked.type != field->type || locked.size != field->size || locked.offset != field->offset)
                    return false;
                field = field->next;
            }
            if (field)
                return false;
            mapping.push_back(selected);
            message = message->next;
        }
        return !message && std::all_of(found.begin(), found.end(), [](bool b) { return b; });
    }
    std::size_t index(std::string_view name) const {
        for (std::size_t i = 0; i < mapping.size(); ++i)
            if (mapping[i]->name == name)
                return i;
        throw std::runtime_error("unbound table");
    }
    static std::uint32_t callback(void*, void* listener, void* raw, void* context) {
        auto& self = *static_cast<Consumer*>(context);
        auto& m = self.model;
        const auto& message = *static_cast<const CgMsg*>(raw);
        switch (message.type) {
        case 0x100:
            if (!self.bind(listener))
                m.fail(true);
            else
                m.opened();
            break;
        case 0x101:
            m.close();
            break;
        case 0x200:
            m.begin();
            break;
        case 0x210:
            m.commit();
            break;
        case 0x1112:
            m.online_event();
            break;
        case 0x1115:
            break; // Opaque raw token cannot establish a composite checkpoint.
        case 0x1110:
            if (!message.data || message.data_size != sizeof(CgDataLifeNum))
                m.fail(true);
            else
                m.lifenum(static_cast<const CgDataLifeNum*>(message.data)->life_number);
            break;
        case 0x1111: {
            struct Clear {
                std::int32_t table;
                std::int64_t rev;
                std::uint32_t flags;
            };
            if (!message.data || message.data_size != sizeof(Clear)) {
                m.fail(true);
                break;
            }
            const auto& clear = *static_cast<const Clear*>(message.data);
            if (clear.table < 0 || static_cast<std::size_t>(clear.table) >= self.mapping.size())
                m.fail(true);
            else
                m.clear_deleted(true);
            break;
        }
        case 0x120: {
            const auto& row = *static_cast<const CgMsgStreamData*>(raw);
            if (row.msg_index >= self.mapping.size() || !row.data || (row.num_nulls && !row.nulls)) {
                m.fail(true);
                break;
            }
            const auto& table = *self.mapping[row.msg_index];
            const std::span bytes{static_cast<const std::byte*>(row.data), row.data_size};
            const std::span nulls{row.nulls, row.num_nulls};
            if (wire::validate(table, bytes, nulls) != wire::DecodeResult::Ok ||
                std::any_of(nulls.begin(), nulls.end(), [](auto n) { return n != 0; })) {
                m.fail(true);
                break;
            }
            const auto number = [&](std::string_view name) -> std::int64_t {
                for (const auto& f : table.fields)
                    if (f.name == name) {
                        if (f.type == "d16.5")
                            return *wire::decimal_scaled(wire::load<wire::Bcd16_5>(bytes, f.offset));
                        if (f.size == 1)
                            return wire::load<std::int8_t>(bytes, f.offset);
                        if (f.size == 4)
                            return wire::load<std::int32_t>(bytes, f.offset);
                        return wire::load<std::int64_t>(bytes, f.offset);
                    }
                throw std::runtime_error("required field missing");
            };
            if (number("replAct") != 0) {
                m.fail();
                break;
            }
            if (table.name == "info") {
                m.info(number("infoID"), number("trades_rev"), number("trades_lifenum"), number("publication_state"));
                break;
            }
            Order o{number("public_order_id"),
                    number("sess_id"),
                    number("isin_id"),
                    number("dir"),
                    number(self.pair ? "swap_price" : "price"),
                    number("public_amount_rest"),
                    number("xstatus"),
                    number("xstatus2"),
                    self.pair};
            const bool snapshot = table.name == "orders" || table.name == "multileg_orders";
            if (snapshot)
                m.snapshot(o);
            else if (row.rev != number("replRev"))
                m.fail(true);
            else {
                std::uint64_t digest = 14695981039346656037ULL;
                for (const auto& field : table.fields)
                    for (const auto byte : bytes.subspan(field.offset, field.size)) {
                        digest ^= std::to_integer<unsigned>(byte);
                        digest *= 1099511628211ULL;
                    }
                m.row({o, row.rev, number("public_amount"), static_cast<int>(number("public_action")), digest});
            }
            break;
        }
        default:
            m.fail(true);
            break;
        }
        return m.state == State::Failed || m.state == State::NeedsResync ? 1 : 0;
    }
};
struct Bytes {
    const wire::Table& table;
    std::vector<std::byte> data;
    explicit Bytes(std::size_t source) : table(wire::kTables[source]), data(table.size) {
        for (const auto& f : table.fields)
            if (f.type == "d16.5") {
                wire::Bcd16_5 b{5, 16, 0, 0, 0, 0, 0, 0, 12, 0, 0};
                std::memcpy(data.data() + f.offset, b.data(), b.size());
            }
    }
    void set(std::string_view name, std::int64_t value) {
        for (const auto& f : table.fields)
            if (f.name == name) {
                std::memcpy(data.data() + f.offset, &value, f.size);
                return;
            }
        throw std::runtime_error("fixture field missing");
    }
    void set_order(std::int64_t id, std::int64_t rev, int action, std::int64_t rest) {
        set("replID", id);
        set("replRev", rev);
        set("public_order_id", id);
        set("sess_id", 7);
        set("isin_id", 100);
        set("dir", 1);
        set("public_action", action);
        set("public_amount", 10);
        set("public_amount_rest", rest);
    }
};
struct Driver {
    void* module{};
    void* connection{};
    void* listener{};
    Consumer consumer;
    template <class T> T symbol(const char* name) {
        auto p = reinterpret_cast<T>(dlsym(module, name));
        require(p != nullptr, name);
        return p;
    }
    using Emit = std::uint32_t (*)(std::uint32_t, std::size_t, void*, std::size_t, std::int64_t, std::uint8_t*,
                                   std::size_t);
    Emit emit{};
    Driver(const char* library, int pair) {
        module = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        require(module, "load existing fake CGate library");
        require(symbol<std::uint32_t (*)(const char*)>("cg_env_open")("c2-model-only") == 0, "fake env");
        require(symbol<std::uint32_t (*)(const char*, void**)>("cg_conn_new")("p2tcp://127.0.0.1:1", &connection) == 0,
                "fake conn");
        consumer.pair = pair;
        consumer.getscheme = symbol<std::uint32_t (*)(void*, void**)>("cg_lsn_getscheme");
        consumer.model.open();
        const char* url = pair ? "p2ordbook://"
                                 "FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;snapshot.data=multileg_orders;online."
                                 "data=multileg_orders_log"
                               : "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL";
        using Callback = CgListenerCallback;
        require(symbol<std::uint32_t (*)(void*, const char*, Callback, void*, void**)>("cg_lsn_new")(
                    connection, url, &Consumer::callback, &consumer, &listener) == 0,
                "fake composite");
        emit = symbol<Emit>("moex_fake_ordlog_emit");
    }
    std::uint32_t open() {
        return symbol<std::uint32_t (*)(void*, const char*)>("cg_lsn_open")(listener, "");
    }
    ~Driver() {
        symbol<std::uint32_t (*)(void*)>("cg_lsn_destroy")(listener);
        symbol<std::uint32_t (*)(void*)>("cg_conn_destroy")(connection);
        symbol<std::uint32_t (*)()>("cg_env_close")();
        dlclose(module);
    }
    std::uint32_t control(unsigned kind) {
        return emit(kind, 0, nullptr, 0, 0, nullptr, 0);
    }
    std::uint32_t row(Bytes& b, std::int64_t rev) {
        return emit(0x120, consumer.index(b.table.name), b.data.data(), b.data.size(), rev, nullptr, 0);
    }
    void snapshot() {
        require(open() == 0, "schema bound at OPEN");
        CgDataLifeNum life{7, 0};
        require(emit(0x1110, 0, &life, sizeof(life), 0, nullptr, 0) == 0, "life callback");
        require(control(0x200) == 0, "snapshot TN_BEGIN");
        Bytes b(consumer.pair ? 5 : 4);
        b.set_order(1, 1, 2, 6);
        require(row(b, 1) == 0, "snapshot seeds remaining quantity despite last action execution");
        Bytes info(6);
        info.set("infoID", 1);
        info.set("trades_rev", 10);
        info.set("trades_lifenum", 7);
        info.set("publication_state", 1);
        require(row(info, 1) == 0 && control(0x210) == 0 && control(0x1112) == 0, "commit and ONLINE");
        require(consumer.model.state == State::CatchingUp, "callback backlog blocks readiness");
        consumer.model.drain();
        require(consumer.model.state == State::Ready, "callback snapshot ready");
    }
};
void callback_cases(const char* library) {
    // A real capture loads CGate once for all listener lifetimes. Keep that DSO lifetime in the fixture too.
    const std::unique_ptr<void, decltype(&dlclose)> module(dlopen(library, RTLD_NOW | RTLD_LOCAL), &dlclose);
    require(static_cast<bool>(module), "pin callback fixture library");
    for (int pair = 0; pair < 2; ++pair)
        for (int reverse = 0; reverse < 2; ++reverse) {
            if (reverse)
                setenv("MOEX_FAKE_COMPOSITE_REVERSE", "1", 1);
            Driver d(library, pair);
            d.snapshot();
            require(d.consumer.mapping[reverse ? 0 : 2]->name == (pair ? "multileg_orders" : "orders"),
                    "bind permuted snapshot index");
            Bytes log(pair ? 1 : 0);
            log.set_order(1, 11, 2, 3);
            require(d.control(0x200) == 0 && d.row(log, 11) == 0 && d.control(0x210) == 0,
                    "partial execution callback");
            d.consumer.model.drain();
            require(d.consumer.model.data.book.begin()->second.remaining == 3, "direct rest through schema decode");
            require(d.control(0x200) == 0 && d.row(log, 11) == 0 && d.control(0x210) == 0,
                    "duplicate callback transaction");
            log.set_order(2, 12, 1, 10);
            require(d.control(0x200) == 0 && d.row(log, 12) == 0 && d.control(0x210) == 0, "add callback");
            log.set_order(1, 11, 2, 3);
            require(d.control(0x200) == 0 && d.row(log, 11) == 0 && d.control(0x210) == 0,
                    "known historical replay callback");
            log.set_order(1, 13, 2, 0);
            require(d.control(0x200) == 0 && d.row(log, 13) == 0 && d.control(0x210) == 0, "full execution callback");
            log.set_order(2, 14, 0, 10);
            require(d.control(0x200) == 0 && d.row(log, 14) == 0 && d.control(0x210) == 0, "cancel callback");
            d.consumer.model.drain();
            require(d.consumer.model.data.book.empty(), "terminal callbacks removed both orders");
            log.set_order(2, 14, 0, 9);
            require(d.control(0x200) == 0 && d.row(log, 14) == 0 && d.control(0x210) != 0,
                    "conflicting replay callback failure");
            require(d.consumer.model.state == State::NeedsResync, "contradictory callback fails closed");
            unsetenv("MOEX_FAKE_COMPOSITE_REVERSE");
        }
    for (int pair = 0; pair < 2; ++pair)
        for (int fault = 0; fault < 9; ++fault) {
            Driver d(library, pair);
            d.snapshot();
            const auto old = d.consumer.model.generation;
            if (fault == 0) {
                Bytes log(pair ? 1 : 0);
                log.set_order(1, 11, 2, 3);
                require(d.emit(0x120, 999, log.data.data(), log.data.size(), 11, nullptr, 0) != 0, "malformed index");
            } else if (fault == 1) {
                // Both regular layouts are 128 bytes: size alone cannot distinguish them.
                Bytes log(pair ? 1 : 0);
                log.set_order(1, 11, 2, 3);
                d.control(0x200);
                require(d.emit(0x120, d.consumer.index(pair ? "multileg_orders" : "orders"), log.data.data(),
                               log.data.size(), 11, nullptr, 0) != 0,
                        "snapshot/log index confusion rejected");
            } else if (fault == 2) {
                CgDataLifeNum next{8, 0};
                d.emit(0x1110, 0, &next, sizeof(next), 0, nullptr, 0);
            } else if (fault == 3)
                d.control(0x101);
            else if (fault == 4) {
                d.control(0x200);
                d.consumer.model.drain();
                require(d.consumer.model.state != State::Ready, "missing TN_COMMIT");
                d.control(0x101);
            } else if (fault == 5 || fault == 6) {
                struct Clear {
                    std::int32_t table;
                    std::int64_t rev;
                    std::uint32_t flags;
                };
                const auto name =
                    fault == 5 ? (pair ? "multileg_orders" : "orders") : (pair ? "multileg_orders_log" : "orders_log");
                Clear clear{static_cast<std::int32_t>(d.consumer.index(name)), 99, 0};
                require(d.emit(0x1111, 0, &clear, sizeof(clear), 0, nullptr, 0) != 0,
                        "relevant ClearDeleted requests rebuild");
            } else if (fault == 8) {
                Bytes log(pair ? 1 : 0);
                log.set_order(1, 11, 2, 3);
                d.control(0x200);
                d.row(log, 11);
                require(d.control(0x210) == 0, "original semantic row");
                log.set("moment_ns", 999);
                d.control(0x200);
                d.row(log, 11);
                require(d.control(0x210) != 0, "same revision with changed non-book field is conflicting");
            } else {
                char token[] = "raw-token";
                d.emit(0x1115, 0, token, sizeof(token), 0, nullptr, 0);
                d.symbol<void (*)()>("moex_fake_ordlog_error")();
                std::uint32_t state = 0;
                require(d.symbol<std::uint32_t (*)(void*, std::uint32_t*)>("cg_lsn_getstate")(d.listener, &state) ==
                                0 &&
                            state == 1,
                        "observed listener ERROR");
                d.consumer.model.fail(); // Supervisor input, not an invented CG_MSG_ERROR.
            }
            require(!d.consumer.model.current(old) && d.consumer.model.data.book.empty(), "callback invalidation");
        }
    setenv("MOEX_FAKE_LSN_ERROR_STATE", "1", 1);
    for (int pair = 0; pair < 2; ++pair) {
        Driver d(library, pair);
        require(d.open() == 0, "asynchronous open accepted");
        std::uint32_t state{};
        require(d.symbol<std::uint32_t (*)(void*, std::uint32_t*)>("cg_lsn_getstate")(d.listener, &state) == 0 &&
                    state == 1,
                "retention-equivalent simulated open ERROR");
        d.consumer.model.fail();
        require(!d.consumer.model.retry(0) && d.consumer.model.retry(1000), "failed open retries fresh");
        require(d.consumer.mapping.empty() && d.consumer.model.state == State::Opening,
                "failed negotiation has no usable schema");
    }
    unsetenv("MOEX_FAKE_LSN_ERROR_STATE");
    setenv("MOEX_FAKE_ORDLOG_BAD_SCHEME", "1", 1);
    {
        Driver d(library, 0);
        require(d.open() != 0 && d.consumer.model.state == State::Failed, "schema drift before row decode");
    }
    unsetenv("MOEX_FAKE_ORDLOG_BAD_SCHEME");
}

struct Counts {
    std::size_t histories{}, equivalences{}, restarts{}, illegal{};
} counts;
// Independent history producer stores post-event states; it does not call Mutation to build snapshots.
void equivalence(std::uint64_t seed, int mode, std::size_t size) {
    std::mt19937_64 rng(seed);
    std::vector<Event> history;
    Book truth;
    std::array<std::int64_t, 2> revisions{};
    for (std::size_t i = 0; i < size; ++i) {
        const int pair = mode == 2 ? static_cast<int>(rng() % 2) : mode;
        auto e = event(static_cast<std::int64_t>(i + 1), ++revisions[pair], 1, 10, pair);
        e.order.status = static_cast<std::int64_t>(rng() % 256);
        history.push_back(e);
        if (i % 3 == 0) {
            e.revision = ++revisions[pair];
            e.action = 2;
            e.order.remaining = 4;
            e.amount = 6;
            history.push_back(e);
        }
        if (i % 5 == 0) {
            e.revision = ++revisions[pair];
            e.action = 2;
            e.order.remaining = 0;
            e.amount = 4;
            history.push_back(e);
        } else if (i % 7 == 0) {
            e.revision = ++revisions[pair];
            e.action = 0;
            history.push_back(e);
        }
    }
    std::vector<std::size_t> boundaries{0, 1, history.size() / 2, history.size()};
    for (int i = 0; i < 8; ++i)
        boundaries.push_back(rng() % (history.size() + 1));
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    std::map<std::size_t, Book> snapshots;
    std::map<std::size_t, std::array<std::int64_t, 2>> frontiers;
    revisions = {};
    snapshots[0] = {};
    frontiers[0] = {};
    for (std::size_t i = 0; i < history.size(); ++i) {
        const auto& e = history[i];
        const Key key{e.order.pair, e.order.id};
        if (e.action == 0 || e.order.remaining == 0)
            truth.erase(key);
        else
            truth[key] = e.order;
        revisions[e.order.pair] = e.revision;
        if (std::binary_search(boundaries.begin(), boundaries.end(), i + 1)) {
            snapshots[i + 1] = truth;
            frontiers[i + 1] = revisions;
        }
    }
    Mutation continuous;
    for (const auto& e : history)
        require(continuous.apply(e) == Result::Valid, "generated legal continuous history");
    require(continuous.book == truth, "independent exchange oracle agrees");
    for (auto r : boundaries) {
        Mutation resumed;
        resumed.book = snapshots[r];
        resumed.frontier = frontiers[r];
        for (std::size_t i = r; i < history.size(); ++i)
            require(resumed.apply(history[i]) == Result::Valid, "bound continuation legal");
        require(resumed.book == truth && resumed.hash(7) == continuous.hash(7), "snapshot versus continuous equality");
        ++counts.equivalences;
        Model interrupted;
        bootstrap(interrupted);
        for (std::size_t i = 0; i < r; ++i) {
            interrupted.begin();
            interrupted.row(history[i]);
            interrupted.commit();
            interrupted.drain();
        }
        const auto old = interrupted.generation;
        interrupted.close();
        require(!interrupted.current(old), "restart invalidates borrowed generation");
        bootstrap(interrupted, snapshots[r]);
        interrupted.data.frontier = frontiers[r];
        for (std::size_t i = r; i < history.size(); ++i) {
            interrupted.begin();
            interrupted.row(history[i]);
            interrupted.commit();
            interrupted.drain();
        }
        require(interrupted.state == State::Ready && interrupted.data.book == truth &&
                    interrupted.data.hash(7) == continuous.hash(7),
                "restart final exchange-state hash equal");
        ++counts.restarts;
    }
    // One-element changes in an otherwise legal history; replay full prefix through the recovery model.
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto corrupted = history;
        const auto offset = rng() % corrupted.size();
        auto& e = corrupted[offset];
        if (mutation == 0)
            e.order.id = -1;
        if (mutation == 1)
            e.order.remaining = -1;
        if (mutation == 2)
            e.order.side = 3;
        if (mutation == 3)
            e.action = 9;
        Model m;
        bootstrap(m);
        for (std::size_t i = 0; i < offset; ++i) {
            m.begin();
            m.row(corrupted[i]);
            m.commit();
            m.drain();
        }
        if (mutation == 0)
            e.action = 2;
        m.begin();
        m.row(e);
        m.commit();
        m.drain();
        require(m.state != State::Ready && m.data.book.empty(), "property mutation fails closed");
        ++counts.illegal;
    }
    ++counts.histories;
}
int capture_fixture(const char* path) {
    std::ifstream input(path);
    require(static_cast<bool>(input), "capture fixture unavailable");
    std::string line;
    std::getline(input, line);
    require(line.starts_with("# source_sha256=") && line.size() == 80, "source trace hash required");
    const auto source_hash = line.substr(16);
    std::map<unsigned, Model> models;
    std::map<unsigned, std::uint64_t> ordinals;
    std::map<std::tuple<unsigned, std::uint64_t, std::int64_t>, std::pair<std::uint64_t, std::uint64_t>> states;
    std::size_t comparisons = 0, conflicts = 0, unknown = 0, rejected = 0, ready = 0;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        char kind{};
        unsigned listener{};
        std::uint64_t epoch{}, ordinal{};
        require(static_cast<bool>(row >> kind >> listener >> epoch >> ordinal), "invalid capture fixture record");
        auto& m = models[listener];
        if (ordinal) {
            require(ordinal == ++ordinals[listener], "capture fixture ordinal continuity");
        }
        if (kind == 'O') {
            m.open();
            m.opened();
        } else if (kind == 'L') {
            std::uint64_t life{};
            require(static_cast<bool>(row >> life), "life fixture");
            m.lifenum(life);
        } else if (kind == 'B')
            m.begin();
        else if (kind == 'C') {
            m.commit();
            m.drain();
        } else if (kind == 'N') {
            m.online_event();
            m.drain();
        } else if (kind == 'X')
            m.close();
        else if (kind == 'E' || kind == 'D')
            m.fail();
        else if (kind == 'I') {
            std::int64_t id{}, rev{}, life{}, pub{};
            require(static_cast<bool>(row >> id >> rev >> life >> pub), "info fixture");
            m.info(id, rev, life, pub);
        } else if (kind == 'S' || kind == 'R') {
            Event e;
            auto& o = e.order;
            require(static_cast<bool>(row >> o.pair >> o.id >> o.session >> o.instrument >> o.side >> o.price >>
                                      o.remaining >> o.status >> o.status2),
                    "order fixture");
            if (kind == 'S')
                m.snapshot(o);
            else {
                require(static_cast<bool>(row >> e.revision >> e.amount >> e.action >> e.fingerprint), "log fixture");
                m.row(e);
            }
        } else if (kind != 'T') {
            ++unknown;
            m.fail();
        }
        if (m.state == State::NeedsResync || m.state == State::Failed)
            ++rejected;
        if ((kind == 'N' || kind == 'C') && m.state == State::Ready) {
            ++ready;
            const auto frontier = m.data.frontier.at(listener == 1 ? 1 : 0);
            const auto key = std::tuple{listener, m.life, frontier};
            const auto hash = m.data.hash(m.life);
            if (const auto it = states.find(key); it != states.end() && it->second.first != epoch) {
                ++comparisons;
                if (it->second.second != hash)
                    ++conflicts;
            } else
                states[key] = {epoch, hash};
        }
    }
    const char* equivalence = comparisons && !conflicts && !unknown && !rejected ? "PASS_CONDITIONAL"
                              : conflicts                                        ? "MISMATCH"
                                                                                 : "NOT_OBSERVED";
    std::cout << "{\"source_sha256\":\"" << source_hash << "\",\"equivalence\":\"" << equivalence
              << "\",\"common_frontier_comparisons\":" << comparisons << ",\"conflicts\":" << conflicts
              << ",\"unknown_records\":" << unknown << ",\"model_rejections\":" << rejected
              << ",\"ready_observations\":" << ready << ",\"production_c2\":\"BLOCKED\"}\n";
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--capture-fixture")
            return capture_fixture(argv[2]);
        require(argc == 2, "fake runtime library required");
        mutation_cases();
        recovery_cases();
        callback_cases(argv[1]);
        for (std::uint64_t seed = 1; seed <= 32; ++seed)
            for (int mode = 0; mode < 3; ++mode)
                equivalence(seed, mode, 64);
        for (int mode = 0; mode < 3; ++mode)
            equivalence(99, mode, 10000);
        std::cout << "MODEL_ONLY PASS histories=" << counts.histories
                  << " snapshot_equivalences=" << counts.equivalences << " restart_equivalences=" << counts.restarts
                  << " illegal_properties=" << counts.illegal
                  << " life_positions=6 close_error_positions=6 callback_pair_permutations=4 callback_faults=18\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
