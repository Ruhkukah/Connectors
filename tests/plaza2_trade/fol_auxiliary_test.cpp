#include "fol_auxiliary.hpp"

#include <array>
#include <iostream>

namespace {
using namespace moex::plaza2_trade::fol_auxiliary;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void send(Projection& projection, const Event& event) {
    if (const auto error = projection.on_plaza2_listener_event(event); error)
        throw std::runtime_error(error.message);
}

// Vendor base-100 groups exercise odd fractional and integer lengths,
// the first-group sign, and the legacy zero byte.
constexpr std::array<std::uint8_t, 11> price{5, 16, 0x80, 0x80, 0, 0, 1, 23, 45, 67, 80};
constexpr std::array<std::uint8_t, 15> balance{2, 26, 12, 34, 56, 78, 90, 12, 34, 56, 78, 90, 12, 34, 56};
constexpr std::array<std::uint8_t, 13> settlement{2, 21, 9, 87, 65, 43, 21, 9, 87, 65, 43, 21, 42};

struct RawRow {
    cg::Plaza2RawTableBinding table;
    std::vector<std::byte> data;
    std::vector<std::uint8_t> nulls;

    RawRow(std::string name, std::size_t index) {
        table.name = std::move(name);
        table.index = index;
        add("replID", "i8", 8);
        add("replRev", "i8", 8);
        add("replAct", "i8", 8);
    }

    void add(std::string name, std::string type, std::size_t bytes) {
        table.fields.push_back({std::move(name), std::move(type), data.size(), bytes, table.fields.size()});
        data.resize(data.size() + bytes);
        nulls.push_back(0);
        table.row_size = data.size();
    }

    void set(std::string_view name, std::span<const std::byte> value) {
        const auto field = std::ranges::find(table.fields, name, &cg::Plaza2RawFieldBinding::name);
        require(field != table.fields.end() && value.size() <= field->size, "invalid fixture field");
        auto target = std::span(data).subspan(field->offset, field->size);
        std::ranges::fill(target, std::byte{});
        std::ranges::copy(value, target.begin());
    }

    template <class T> void scalar(std::string_view name, T value) {
        set(name, std::as_bytes(std::span(&value, 1)));
    }

    void text(std::string_view name, std::string_view value) {
        set(name, std::as_bytes(std::span(value.data(), value.size())));
    }

    void revision(std::int64_t id, std::int64_t revision, std::int64_t action = 0) {
        scalar("replID", id);
        scalar("replRev", revision);
        scalar("replAct", action);
    }

    Event event() const {
        return {.kind = Kind::StreamData,
                .raw_payload = data,
                .raw_nulls = nulls,
                .table_index = table.index,
                .raw_table = &table};
    }
};

RawRow common_row() {
    RawRow common("common", 4);
    common.add("isin_id", "i4", 4);
    common.add("best_buy_native", "d16.5", price.size());
    common.scalar("isin_id", std::int32_t{11});
    common.set("best_buy_native", std::as_bytes(std::span(price)));
    common.revision(1, 7);
    return common;
}

void test_decimal_bytes() {
    require(decimal(std::as_bytes(std::span(price)), "d16.5") == "-123.45678", "signed price must be exact");
    require(decimal(std::as_bytes(std::span(balance)), "d26.2") == "123456789012345678901234.56",
            "PART balance must retain all 26 digits");
    require(decimal(std::as_bytes(std::span(settlement)), "d21.2") == "9876543210987654321.42",
            "settlement with odd integer precision must be exact");
}

void test_vendor_string_widths() {
    RawRow schedule("trade_periods", 1);
    schedule.add("trade_period_name", "c255", 256);
    const std::string name(255, 'S');
    schedule.text("trade_period_name", name);
    schedule.revision(1, 2);

    RawRow fee("fee_all", 2);
    fee.add("p2login", "c64", 65);
    fee.add("points", "i4", 4);
    const std::string login(64, 'L');
    fee.text("p2login", login);
    fee.scalar("points", std::int32_t{3});
    fee.revision(1, 2);

    Projection projection({{"trade_periods", {{"trade_period_name", "c255"}}},
                           {"fee_all", {{"p2login", "c64"}, {"points", "i4"}}, "p2login"}},
                          login, {});
    std::array schema{schedule.table, fee.table};
    send(projection, {.kind = Kind::Open, .raw_tables = schema});
    send(projection, schedule.event());
    send(projection, fee.event());
    send(projection, {.kind = Kind::Online});
    require(projection.rows().at({0, 1}).values.at("trade_period_name") == name,
            "schedule must retain all 255 characters");
    const auto& fee_values = projection.rows().at({1, 1}).values;
    require(fee_values.at("points") == "3" && !fee_values.contains("p2login"),
            "fee must match all 64 login characters without publishing the login");

    --schema.front().fields.back().size;
    const auto error = projection.on_plaza2_listener_event({.kind = Kind::Open, .raw_tables = schema});
    require(error.code == cg::Plaza2ErrorCode::IncompatibleScheme && !projection.online(),
            "a c255 field still needs the full storage width");
}

void test_projection() {
    RawRow part("part", 8);
    part.add("client_code", "c7", 8);
    part.add("money_free", "d26.2", balance.size());
    part.text("client_code", "CLIENT1");
    part.set("money_free", std::as_bytes(std::span(balance)));
    part.revision(31, 101);
    auto common = common_row();
    Projection projection(
        {{"part", {{"client_code", "c7"}, {"money_free", "d26.2"}}, "client_code"},
         {"common", {{"best_buy_native", "d16.5"}, {"settlement_price_open", "d16.5", true}}, {}, true}},
        "CLIENT1", {11});
    const std::array schema{common.table, part.table};
    const auto open = Event{.kind = Kind::Open, .raw_tables = schema};
    const auto key = std::pair<std::size_t, std::int64_t>{0, 31};
    send(projection, open);
    send(projection, {.kind = Kind::LifeNum, .unsigned_value = 45});
    send(projection, part.event());
    require(projection.rows().empty() && !projection.online(), "unframed snapshot row must remain pending");
    part.text("client_code", "CLIENT2");
    part.revision(32, 102);
    send(projection, part.event());
    send(projection, {.kind = Kind::TransactionBegin});
    send(projection, common.event());
    common.scalar("isin_id", std::int32_t{22});
    common.revision(2, 8);
    send(projection, common.event());
    send(projection, {.kind = Kind::TransactionCommit});
    require(!projection.online(), "framed snapshot is not ready until ONLINE");

    send(projection, {.kind = Kind::Online});
    require(projection.online() && projection.life() == 45 && projection.rows().size() == 2,
            "ONLINE publishes only the selected account and instrument");
    const auto& balance_row = projection.rows().at(key);
    require(balance_row.revision == 101 && balance_row.values.size() == 1 &&
                balance_row.values.at("money_free") == "123456789012345678901234.56",
            "account filter omits identity text from the projected row");
    const auto& instrument = projection.rows().at({1, 1}).values;
    require(instrument.at("isin_id") == "11" && instrument.at("best_buy_native") == "-123.45678" &&
                !instrument.contains("settlement_price_open"),
            "missing optional field is accepted without a fabricated value");

    const auto before = projection.committed_at();
    part.text("client_code", "CLIENT1");
    part.revision(31, 103);
    part.nulls.back() = 1;
    send(projection, {.kind = Kind::TransactionBegin});
    send(projection, part.event());
    require(projection.rows().at(key).revision == 101 && projection.committed_at() == before,
            "pending update cannot publish before commit");
    send(projection, {.kind = Kind::TransactionCommit});
    require(projection.rows().at(key).revision == 103 && projection.rows().at(key).values.empty(),
            "committed null update clears old value");

    part.revision(31, 102);
    part.nulls.back() = 0;
    send(projection, {.kind = Kind::TransactionBegin});
    send(projection, part.event());
    send(projection, {.kind = Kind::TransactionCommit});
    require(projection.rows().at(key).revision == 103, "older revision cannot overwrite committed state");

    part.revision(31, 104, 1);
    send(projection, {.kind = Kind::TransactionBegin});
    send(projection, part.event());
    require(projection.rows().contains(key), "pending deletion must retain committed row");
    send(projection, {.kind = Kind::TransactionCommit});
    require(!projection.rows().contains(key) && projection.rows().size() == 1,
            "committed deletion must remove only its own row");

    part.revision(31, 105);
    send(projection, {.kind = Kind::TransactionBegin});
    send(projection, part.event());
    const auto changed = projection.on_plaza2_listener_event({.kind = Kind::LifeNum, .unsigned_value = 46});
    require(changed.code == cg::Plaza2ErrorCode::DecodeFailed && !projection.online() && projection.life() == 0 &&
                projection.rows().empty() && projection.committed_at() == Projection::Clock::time_point{},
            "LifeNum change withdraws committed rows and rolls back the pending transaction");
    send(projection, open);
    send(projection, {.kind = Kind::Online});
    require(projection.rows().empty() && projection.life() == 0,
            "fresh empty snapshot cannot inherit rolled-back data or the rejected LifeNum");
    projection.on_plaza2_listener_error({.code = cg::Plaza2ErrorCode::RuntimeCallFailed});
    require(!projection.online() && projection.rows().empty(), "listener error clears projection authority");
}

void test_login_cod() {
    RawRow user("user", 0);
    user.add("login", "c20", 21);
    user.add("sma_flags", "i4", 4);
    user.add("start_date", "t", sizeof(moex::plaza2::public_wire::Time));
    user.add("end_date", "t", sizeof(moex::plaza2::public_wire::Time));
    user.scalar("start_date",
                moex::plaza2::public_wire::Time{.year = 1970, .month = 1, .day = 1, .hour = 3, .second = 10});
    user.scalar("end_date",
                moex::plaza2::public_wire::Time{.year = 1970, .month = 1, .day = 1, .hour = 3, .minute = 1});
    Projection projection(
        {{"user", {{"login", "c20"}, {"sma_flags", "i4"}, {"start_date", "t"}, {"end_date", "t"}}, "login"}},
        "configured_login", {});
    const auto open = Event{.kind = Kind::Open, .raw_tables = std::span(&user.table, 1)};
    const auto commit_row = [&] {
        send(projection, {.kind = Kind::TransactionBegin});
        send(projection, user.event());
        send(projection, {.kind = Kind::TransactionCommit});
    };

    send(projection, open);
    user.revision(1, 1);
    user.text("login", "configured_login");
    user.scalar("sma_flags", std::int32_t{2});
    send(projection, user.event());
    require(login_cod_state(projection, 20) == "unknown", "uncommitted login cannot authorize entry");
    user.revision(2, 2);
    user.text("login", "other_login");
    user.scalar("sma_flags", std::int32_t{1});
    send(projection, user.event());
    send(projection, {.kind = Kind::Online});
    require(login_cod_state(projection, 9) == "unknown" && login_cod_state(projection, 20) == "disabled" &&
                login_cod_state(projection, 61) == "unknown",
            "COD must use only bit zero of the selected currently effective login");

    user.revision(1, 3);
    user.text("login", "configured_login");
    user.scalar("sma_flags", std::int32_t{3});
    send(projection, {.kind = Kind::TransactionBegin});
    send(projection, user.event());
    require(login_cod_state(projection, 20) == "disabled", "pending COD update must remain hidden");
    send(projection, {.kind = Kind::TransactionCommit});
    require(login_cod_state(projection, 20) == "enabled", "committed COD change must fence entry");

    user.revision(1, 4);
    user.text("login", "other_login");
    commit_row();
    require(login_cod_state(projection, 20) == "unknown", "out-of-scope revision must withdraw prior login authority");
    user.revision(1, 3);
    user.text("login", "configured_login");
    commit_row();
    require(login_cod_state(projection, 20) == "unknown", "older revision cannot resurrect a replaced login");

    user.revision(1, 5);
    commit_row();
    user.revision(3, 6);
    commit_row();
    require(login_cod_state(projection, 20) == "unknown", "multiple effective selected rows must fence entry");
    const auto cleared = projection.on_plaza2_listener_event(
        {.kind = Kind::ClearDeleted, .signed_value = INT64_MAX, .table_index = user.table.index});
    require(cleared && !projection.online() && login_cod_state(projection, 20) == "unknown",
            "whole-table clear must invalidate COD before resnapshot");
}

void test_environment_contract() {
    using cg::Plaza2Environment;
    RawRow environment("spectra_environment", 0);
    environment.add("environment_id", "i4", 4);
    environment.add("environment_name", "c8", 9);
    constexpr std::array<std::uint8_t, 9> observed_t1_name{0x4d, 0x58, 0x46, 0x4f, 0x54, 0x31, 0, 0, 0};
    environment.revision(1, 1);
    environment.scalar("environment_id", std::int32_t{7});
    environment.set("environment_name", std::as_bytes(std::span(observed_t1_name)));
    Projection projection({{"spectra_environment", {{"environment_id", "i4"}, {"environment_name", "c8"}}}}, {}, {});
    const auto open = Event{.kind = Kind::Open, .raw_tables = std::span(&environment.table, 1)};
    send(projection, {.kind = Kind::LifeNum, .unsigned_value = 131140477});
    send(projection, open);
    send(projection, {.kind = Kind::TransactionBegin});
    send(projection, environment.event());
    send(projection, {.kind = Kind::TransactionCommit});
    send(projection, {.kind = Kind::Online});
    require(projection.rows().size() == 1 && projection.life() == 131140477,
            "committed T1 environment must retain the LifeNum announced before OPEN");
    const auto& t1 = projection.rows().begin()->second;
    require(value(t1, "environment_name") == "MXFOT1" && environment_matches(t1, Plaza2Environment::Test) &&
                !environment_matches(t1, Plaza2Environment::Prod),
            "the observed c8 T1 row must decode to MXFOT1 and authorize only TEST");

    send(projection, open);
    require(projection.life() == 0 && !projection.online() && projection.rows().empty(),
            "reopening a bound listener cannot carry its previous generation");
    send(projection, {.kind = Kind::LifeNum, .unsigned_value = 131140478});
    send(projection, {.kind = Kind::Close});
    send(projection, open);
    require(projection.life() == 0, "CLOSE clears generation evidence before the next OPEN");
    send(projection, {.kind = Kind::Close});
    send(projection, {.kind = Kind::LifeNum, .unsigned_value = 131140479});
    projection.on_plaza2_listener_error({.code = cg::Plaza2ErrorCode::RuntimeCallFailed});
    send(projection, open);
    require(projection.life() == 0, "listener error clears an announced but unopened generation");
    send(projection, {.kind = Kind::Close});
    send(projection, {.kind = Kind::LifeNum, .unsigned_value = 131140480});
    const auto bad_open = projection.on_plaza2_listener_event({.kind = Kind::Open});
    require(bad_open.code == cg::Plaza2ErrorCode::IncompatibleScheme && projection.life() == 0,
            "failed schema binding clears the announced generation");
    send(projection, open);
    require(projection.life() == 0, "successful OPEN cannot inherit a failed OPEN's generation");

    const auto check = [](const char* id, const char* name, bool test, bool prod) {
        const Projection::Row row{.values = {{"environment_id", id}, {"environment_name", name}}};
        require(environment_matches(row, Plaza2Environment::Test) == test &&
                    environment_matches(row, Plaza2Environment::Prod) == prod,
                "environment identity must match an exact pair for the configured runtime environment");
    };
    check("2", "MXFOPREP", true, false);
    check("1", "MXFOPROD", false, true);
    check("7", "MXFOPREP", false, false);
    check("2", "MXFOT1", false, false);
    check("7", "MXFOPROD", false, false);
    check("1", "MXFOT1", false, false);
    check("2", "MXFOPROD", false, false);
    check("1", "MXFOPREP", false, false);
    check("8", "MXFOT1", false, false);
    check("7", "UNKNOWN", false, false);
}

void test_empty_prohibition_snapshot() {
    RawRow prohibition("prohibition", 0);
    prohibition.add("client_code", "c7", 8);
    prohibition.add("type", "i4", 4);
    Projection projection({{"prohibition", {{"client_code", "c7"}, {"type", "i4"}}, "client_code"}}, "CLIENT1", {});
    const auto open = Event{.kind = Kind::Open, .raw_tables = std::span(&prohibition.table, 1)};
    send(projection, open);
    require(!projection.online(), "empty state before ONLINE is unavailable");
    send(projection, {.kind = Kind::Online});
    require(projection.online() && projection.rows().empty(), "empty ONLINE snapshot is current authority");
    send(projection, {.kind = Kind::Close});
    require(!projection.online() && projection.rows().empty(), "closed empty snapshot loses authority");
    send(projection, open);
    require(!projection.online(), "reopen must await fresh snapshot");
}

void test_schema_refusal() {
    auto common = common_row();
    Projection projection({{"common", {{"best_buy_native", "d16.5"}}, {}, true}}, {}, {11});
    const auto open = Event{.kind = Kind::Open, .raw_tables = std::span(&common.table, 1)};
    send(projection, open);
    send(projection, {.kind = Kind::Online});
    const auto reject = [&](std::span<const cg::Plaza2RawTableBinding> tables) {
        const auto result = projection.on_plaza2_listener_event({.kind = Kind::Open, .raw_tables = tables});
        require(result.code == cg::Plaza2ErrorCode::IncompatibleScheme && !projection.online() &&
                    projection.rows().empty(),
                "bad schema must withdraw the previous generation");
    };
    auto bad = common.table;
    bad.fields.back().type_token = "d16.2";
    reject(std::span(&bad, 1));
    bad = common.table;
    bad.fields.back().offset = bad.row_size;
    reject(std::span(&bad, 1));
    bad = common.table;
    bad.fields.pop_back();
    reject(std::span(&bad, 1));
    const std::array extra{common.table, common.table};
    reject(extra);

    send(projection, open);
    send(projection, {.kind = Kind::Online});
    auto malformed = common.event();
    malformed.raw_payload = malformed.raw_payload.first(malformed.raw_payload.size() - 1);
    send(projection, {.kind = Kind::TransactionBegin});
    const auto error = projection.on_plaza2_listener_event(malformed);
    require(error.code == cg::Plaza2ErrorCode::DecodeFailed && !projection.online() && projection.rows().empty(),
            "malformed row invalidates current publication");
}
} // namespace

int main() {
    try {
        test_decimal_bytes();
        test_vendor_string_widths();
        test_projection();
        test_login_cod();
        test_environment_contract();
        test_empty_prohibition_snapshot();
        test_schema_refusal();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
