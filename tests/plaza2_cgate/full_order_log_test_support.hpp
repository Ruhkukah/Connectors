#pragma once
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_decimal.hpp"
#include <array>
#include <cstring>
#include <stdexcept>
using namespace moex::plaza2::cgate;
namespace full_order_log_test {
inline void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
inline std::array<Plaza2RawTableBinding, 4> tables() {
    std::array<Plaza2RawTableBinding, 4> result;
    result[0] = {.name = "orders",
                 .fields =
                     {
                         {.name = "replID", .type_token = "i8", .offset = 0, .size = 8, .index = 0},
                         {.name = "replRev", .type_token = "i8", .offset = 8, .size = 8, .index = 1},
                         {.name = "replAct", .type_token = "i8", .offset = 16, .size = 8, .index = 2},
                         {.name = "public_order_id", .type_token = "i8", .offset = 24, .size = 8, .index = 3},
                         {.name = "sess_id", .type_token = "i4", .offset = 32, .size = 4, .index = 4},
                         {.name = "moment", .type_token = "t", .offset = 36, .size = 10, .index = 5},
                         {.name = "moment_ns", .type_token = "u8", .offset = 48, .size = 8, .index = 6},
                         {.name = "xstatus", .type_token = "i8", .offset = 56, .size = 8, .index = 7},
                         {.name = "xstatus2", .type_token = "i8", .offset = 64, .size = 8, .index = 8},
                         {.name = "public_action", .type_token = "i1", .offset = 72, .size = 1, .index = 9},
                         {.name = "isin_id", .type_token = "i4", .offset = 76, .size = 4, .index = 10},
                         {.name = "dir", .type_token = "i1", .offset = 80, .size = 1, .index = 11},
                         {.name = "price", .type_token = "d16.5", .offset = 81, .size = 11, .index = 12},
                         {.name = "public_amount", .type_token = "i8", .offset = 92, .size = 8, .index = 13},
                         {.name = "public_amount_rest", .type_token = "i8", .offset = 100, .size = 8, .index = 14},
                         {.name = "public_init_moment", .type_token = "t", .offset = 108, .size = 10, .index = 15},
                         {.name = "public_init_amount", .type_token = "i8", .offset = 120, .size = 8, .index = 16},
                     },
                 .index = 0,
                 .row_size = 128};
    result[1] = {.name = "info",
                 .fields =
                     {
                         {.name = "replID", .type_token = "i8", .offset = 0, .size = 8, .index = 0},
                         {.name = "replRev", .type_token = "i8", .offset = 8, .size = 8, .index = 1},
                         {.name = "replAct", .type_token = "i8", .offset = 16, .size = 8, .index = 2},
                         {.name = "infoID", .type_token = "i8", .offset = 24, .size = 8, .index = 3},
                         {.name = "moment", .type_token = "t", .offset = 32, .size = 10, .index = 4},
                         {.name = "publication_state", .type_token = "i1", .offset = 42, .size = 1, .index = 5},
                         {.name = "trades_rev", .type_token = "i8", .offset = 44, .size = 8, .index = 6},
                         {.name = "trades_lifenum", .type_token = "i8", .offset = 52, .size = 8, .index = 7},
                     },
                 .index = 1,
                 .row_size = 60};
    result[2] = {.name = "orders_log",
                 .fields =
                     {
                         {.name = "replID", .type_token = "i8", .offset = 0, .size = 8, .index = 0},
                         {.name = "replRev", .type_token = "i8", .offset = 8, .size = 8, .index = 1},
                         {.name = "replAct", .type_token = "i8", .offset = 16, .size = 8, .index = 2},
                         {.name = "public_order_id", .type_token = "i8", .offset = 24, .size = 8, .index = 3},
                         {.name = "sess_id", .type_token = "i4", .offset = 32, .size = 4, .index = 4},
                         {.name = "isin_id", .type_token = "i4", .offset = 36, .size = 4, .index = 5},
                         {.name = "public_amount", .type_token = "i8", .offset = 40, .size = 8, .index = 6},
                         {.name = "public_amount_rest", .type_token = "i8", .offset = 48, .size = 8, .index = 7},
                         {.name = "id_deal", .type_token = "i8", .offset = 56, .size = 8, .index = 8},
                         {.name = "xstatus", .type_token = "i8", .offset = 64, .size = 8, .index = 9},
                         {.name = "xstatus2", .type_token = "i8", .offset = 72, .size = 8, .index = 10},
                         {.name = "price", .type_token = "d16.5", .offset = 80, .size = 11, .index = 11},
                         {.name = "moment", .type_token = "t", .offset = 92, .size = 10, .index = 12},
                         {.name = "moment_ns", .type_token = "u8", .offset = 104, .size = 8, .index = 13},
                         {.name = "dir", .type_token = "i1", .offset = 112, .size = 1, .index = 14},
                         {.name = "public_action", .type_token = "i1", .offset = 113, .size = 1, .index = 15},
                         {.name = "deal_price", .type_token = "d16.5", .offset = 114, .size = 11, .index = 16},
                     },
                 .index = 2,
                 .row_size = 128};
    result[3] = {.name = "multileg_orders_log", .index = 3, .row_size = 1};
    return result;
}
struct Row {
    std::array<std::byte, 128> wire{};
    std::vector<std::uint8_t> nulls;
    const Plaza2RawTableBinding* table;
    explicit Row(const Plaza2RawTableBinding& t) : nulls(t.fields.size()), table(&t) {}
    const Plaza2RawFieldBinding& field(std::string_view name) const {
        for (const auto& f : table->fields)
            if (f.name == name)
                return f;
        throw std::runtime_error("unknown fixture field");
    }
    template <class T> void set(std::string_view name, T value) {
        std::memcpy(wire.data() + field(name).offset, &value, sizeof(value));
    }
    void price(std::int64_t scaled) {
        moex::plaza2::public_wire::Bcd16_5 b{};
        b[0] = 5;
        b[1] = 16;
        const bool negative = scaled < 0;
        std::uint64_t digits = static_cast<std::uint64_t>(negative ? -scaled : scaled) * 10;
        for (std::size_t i = b.size() - 1; i > 2; --i) {
            b[i] = digits % 100;
            digits /= 100;
        }
        b[2] = static_cast<std::uint8_t>(digits | (negative ? 0x80 : 0));
        set("price", b);
    }
    Plaza2ListenerEvent event() const {
        return {.kind = Plaza2ListenerEventKind::StreamData,
                .stream_code = kFullOrderLogStreamCode,
                .raw_payload = std::span(wire).first(table->row_size),
                .raw_nulls = nulls,
                .table_index = table->index,
                .raw_table = table};
    }
};
inline Row row(const Plaza2RawTableBinding& table, std::int64_t id, std::int32_t isin, std::int64_t price,
               std::int64_t qty, std::int8_t action = 1, std::int64_t rev = 1, std::int64_t status = 1,
               std::int8_t dir = 1) {
    Row r(table);
    r.set("public_order_id", id);
    r.set("isin_id", isin);
    r.price(price);
    r.set("public_amount_rest", qty);
    r.set("public_action", action);
    r.set("replRev", rev);
    r.set("xstatus", status);
    r.set("dir", dir);
    return r;
}
struct Harness {
    std::array<Plaza2RawTableBinding, 4> schema{tables()};
    std::array<std::int32_t, 2> ids{11, 22};
    Plaza2FullOrderLog book;
    explicit Harness(std::size_t orders = 1024, std::size_t levels = 256) : book(ids, orders, levels) {
        open();
    }
    void open() {
        require(!book.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::Open, .raw_tables = schema}), "open");
    }
    void control(Plaza2ListenerEventKind kind) {
        require(!book.on_plaza2_listener_event({.kind = kind}), "control");
    }
    void begin() {
        control(Plaza2ListenerEventKind::TransactionBegin);
    }
    void commit() {
        control(Plaza2ListenerEventKind::TransactionCommit);
    }
    void online() {
        control(Plaza2ListenerEventKind::Online);
    }
    void apply(const Row& r) {
        require(!book.on_plaza2_listener_event(r.event()), "row");
    }
    void add(std::int64_t id, std::int32_t isin, std::int64_t price, std::int64_t qty, std::int8_t action = 1,
             std::int64_t rev = 1, std::int64_t flags = 1, std::int8_t dir = 1) {
        apply(row(schema[2], id, isin, price, qty, action, rev, flags, dir));
    }
    void clear(std::size_t table, std::int64_t revision) {
        require(!book.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                .signed_value = revision,
                                                .table_index = table,
                                                .raw_table = &schema[table]}),
                "clear deleted");
    }
};
} // namespace full_order_log_test
