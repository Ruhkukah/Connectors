#include "moex/plaza2/cgate/plaza2_public_deals.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace moex::plaza2::cgate;
namespace generated = moex::plaza2::generated;

namespace {

constexpr auto kStream = generated::StreamCode::kFortsDealsRepl;
constexpr auto kDealTable = generated::TableCode::kFortsDealsReplDeal;
constexpr auto kActiveSide = std::uint64_t{0x20000000000};
constexpr auto kPassiveSide = std::uint64_t{0x40000000000};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

Plaza2ListenerEvent control(Plaza2ListenerEventKind kind, std::uint64_t value = 0,
                            generated::TableCode table = kNoTableCode) {
    return {.kind = kind, .stream_code = kStream, .table_code = table, .unsigned_value = value};
}

struct DealOptions {
    std::int64_t repl_id{1};
    std::int64_t repl_rev{1};
    std::int64_t repl_act{0};
    std::int64_t session_id{321};
    std::int64_t isin_id{1001};
    std::int64_t deal_id{9001};
    std::int64_t quantity{3};
    std::int64_t nosystem{0};
    std::uint64_t moment{1700000000};
    std::uint64_t moment_ns{1700000000123456789ULL};
    std::string_view price{"102500.12500"};
    bool decimal_exact{true};
    std::int64_t decimal_mantissa{10250012500LL};
    std::int32_t decimal_scale{5};
    std::optional<std::uint64_t> xstatus_buy{0};
    std::optional<std::uint64_t> xstatus_sell{0};
};

struct DecodedDeal {
    std::vector<Plaza2DecodedFieldValue> fields;

    [[nodiscard]] Plaza2ListenerEvent event(generated::TableCode table = kDealTable) const {
        return {
            .kind = Plaza2ListenerEventKind::StreamData, .stream_code = kStream, .table_code = table, .fields = fields};
    }
};

Plaza2DecodedFieldValue i64(generated::FieldCode code, std::int64_t value) {
    return {.field_code = code, .kind = Plaza2DecodedValueKind::SignedInteger, .signed_value = value};
}

DecodedDeal decoded_deal(const DealOptions& options = {}) {
    using generated::FieldCode;
    DecodedDeal row;
    row.fields = {
        i64(FieldCode::kFortsDealsReplDealReplId, options.repl_id),
        i64(FieldCode::kFortsDealsReplDealReplRev, options.repl_rev),
        i64(FieldCode::kFortsDealsReplDealReplAct, options.repl_act),
        i64(FieldCode::kFortsDealsReplDealSessId, options.session_id),
        i64(FieldCode::kFortsDealsReplDealIsinId, options.isin_id),
        i64(FieldCode::kFortsDealsReplDealIdDeal, options.deal_id),
        i64(FieldCode::kFortsDealsReplDealXamount, options.quantity),
        i64(FieldCode::kFortsDealsReplDealNosystem, options.nosystem),
        {.field_code = FieldCode::kFortsDealsReplDealPrice,
         .kind = Plaza2DecodedValueKind::Decimal,
         .text_value = options.price,
         .type_token = "d16.5",
         .decimal_mantissa = options.decimal_mantissa,
         .decimal_scale = options.decimal_scale,
         .decimal_exact = options.decimal_exact},
        {.field_code = FieldCode::kFortsDealsReplDealMoment,
         .kind = Plaza2DecodedValueKind::Timestamp,
         .unsigned_value = options.moment},
        {.field_code = FieldCode::kFortsDealsReplDealMomentNs,
         .kind = Plaza2DecodedValueKind::UnsignedInteger,
         .unsigned_value = options.moment_ns},
    };
    if (options.xstatus_buy) {
        row.fields.push_back({.field_code = FieldCode::kFortsDealsReplDealXstatusBuy,
                              .kind = Plaza2DecodedValueKind::SignedInteger,
                              .signed_value = static_cast<std::int64_t>(*options.xstatus_buy)});
    }
    if (options.xstatus_sell) {
        row.fields.push_back({.field_code = FieldCode::kFortsDealsReplDealXstatusSell,
                              .kind = Plaza2DecodedValueKind::SignedInteger,
                              .signed_value = static_cast<std::int64_t>(*options.xstatus_sell)});
    }
    return row;
}

void send(Plaza2PublicDealsBridge& bridge, const Plaza2ListenerEvent& event) {
    const auto error = bridge.on_message(event);
    require(!error, error.message);
}

void must_fail(Plaza2PublicDealsBridge& bridge, const Plaza2ListenerEvent& event, std::string_view reason) {
    const auto error = bridge.on_message(event);
    require(static_cast<bool>(error), reason);
    require(!bridge.valid() && !bridge.online(), "a failed callback must fail closed");
    require(!bridge.error().empty(), "a failed callback must expose its diagnostic");
}

void start(Plaza2PublicDealsBridge& bridge, std::uint64_t lifenum = 77) {
    send(bridge, control(Plaza2ListenerEventKind::Open));
    send(bridge, control(Plaza2ListenerEventKind::LifeNum, lifenum));
}

void online(Plaza2PublicDealsBridge& bridge) {
    send(bridge, control(Plaza2ListenerEventKind::Online));
}

void transaction(Plaza2PublicDealsBridge& bridge, const std::vector<DecodedDeal>& rows) {
    send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
    for (const auto& row : rows) {
        send(bridge, row.event());
    }
    send(bridge, control(Plaza2ListenerEventKind::TransactionCommit));
}

void test_snapshot_barrier_and_atomic_commit() {
    Plaza2PublicDealsBridge legacy_host(0, 0);
    require(!legacy_host.valid() && !legacy_host.online(), "legacy disabled target must construct inertly");
    must_fail(legacy_host, control(Plaza2ListenerEventKind::Open),
              "disabled legacy target must reject listener Open without allocating a ring");

    Plaza2PublicDealsBridge bridge(1001, 321);
    start(bridge);

    DealOptions bootstrap;
    bootstrap.repl_id = 91;
    bootstrap.repl_rev = 91;
    bootstrap.deal_id = 9001;
    auto historic = decoded_deal(bootstrap);
    send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
    send(bridge, historic.event());
    require(bridge.snapshot().trades.empty(), "snapshot deal is invisible before TN_COMMIT");
    send(bridge, control(Plaza2ListenerEventKind::TransactionCommit));
    require(bridge.snapshot().trades.empty(), "bootstrap commit must update identity only");
    online(bridge);

    DealOptions first;
    first.repl_id = 92;
    first.repl_rev = 92;
    first.deal_id = 9002;
    first.moment_ns = 1700000000123456790ULL;
    DealOptions second = first;
    second.repl_id = 93;
    second.repl_rev = 93;
    second.deal_id = 9003;
    second.moment_ns = 1700000000123456791ULL;
    auto first_decoded = decoded_deal(first);
    auto second_decoded = decoded_deal(second);

    send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
    send(bridge, first_decoded.event());
    send(bridge, second_decoded.event());
    require(bridge.snapshot().trades.empty(), "committed ticks must remain hidden until TN_COMMIT");
    send(bridge, control(Plaza2ListenerEventKind::TransactionCommit));

    const auto result = bridge.snapshot();
    require(result.online && result.valid && result.error.empty(), "online snapshot health");
    require(result.trades.size() == 2, "both rows in one committed transaction must publish");
    require(result.first_sequence == 1 && result.last_sequence == 2, "initial sequence range");
    require(result.trades[0].deal_id == 9002 && result.trades[1].deal_id == 9003,
            "pre-ONLINE 9001 must not become a tick");
    require(result.trades[0].price_scaled == 10250012500LL && result.trades[0].price == "102500.12500",
            "runtime d16.5 mantissa and text must remain exact");
    require(result.trades[0].moment == 1700000000 && result.trades[0].moment_ns == 1700000000123456790ULL,
            "exchange timestamps must retain their separate values");
    require(result.trades[0].received_at_unix_ns > 0, "local callback receipt timestamp");
    require(result.trades[0].stream_epoch == result.stream_epoch && result.trades[0].lifenum == 77,
            "published row provenance");

    // Replay the bootstrap identity after ONLINE. It advances neither output nor cursor.
    transaction(bridge, {historic});
    require(bridge.snapshot().trades.size() == 2, "post-ONLINE bootstrap replay must remain suppressed");
    auto alternate_replay = bootstrap;
    alternate_replay.repl_id = 111;
    alternate_replay.repl_rev = 94;
    transaction(bridge, {decoded_deal(alternate_replay)});
    require(bridge.snapshot().trades.size() == 2, "same deal identity under a new repl_id must remain suppressed");

    DealOptions wrong_session = first;
    wrong_session.repl_id = 112;
    wrong_session.repl_rev = 95;
    wrong_session.deal_id = 9012;
    wrong_session.session_id = 999;
    DealOptions wrong_isin = wrong_session;
    wrong_isin.repl_id = 113;
    wrong_isin.repl_rev = 96;
    wrong_isin.deal_id = 9013;
    wrong_isin.session_id = 321;
    wrong_isin.isin_id = 9999;
    DealOptions system_trade = first;
    system_trade.repl_id = 114;
    system_trade.repl_rev = 97;
    system_trade.deal_id = 9014;
    system_trade.nosystem = 1;
    transaction(bridge, {decoded_deal(wrong_session), decoded_deal(wrong_isin), decoded_deal(system_trade)});
    require(bridge.snapshot().trades.size() == 2, "session, ISIN, and system trade filters");

    send(bridge, control(Plaza2ListenerEventKind::ClearDeleted, 0, kDealTable));
    require(bridge.snapshot().trades.size() == 2, "clear_deleted must not invent a trade or retract history");

    send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
    send(bridge, decoded_deal(first).event(generated::TableCode::kFortsDealsReplMultilegDeal));
    send(bridge, control(Plaza2ListenerEventKind::TransactionCommit));
    require(bridge.snapshot().trades.size() == 2, "multileg rows are not ordinary public deals");
}

void test_aggressor_status_flags() {
    Plaza2PublicDealsBridge bridge(1001, 321);
    start(bridge);
    online(bridge);

    DealOptions buy_active;
    buy_active.repl_id = 1;
    buy_active.repl_rev = 1;
    buy_active.deal_id = 1;
    buy_active.xstatus_buy = kActiveSide;
    buy_active.xstatus_sell = kPassiveSide;

    DealOptions sell_active = buy_active;
    sell_active.repl_id = 2;
    sell_active.repl_rev = 2;
    sell_active.deal_id = 2;
    sell_active.xstatus_buy = kPassiveSide;
    sell_active.xstatus_sell = kActiveSide;

    DealOptions contradictory = buy_active;
    contradictory.repl_id = 3;
    contradictory.repl_rev = 3;
    contradictory.deal_id = 3;
    contradictory.xstatus_buy = kActiveSide | kPassiveSide;
    contradictory.xstatus_sell = kPassiveSide;

    DealOptions absent = buy_active;
    absent.repl_id = 4;
    absent.repl_rev = 4;
    absent.deal_id = 4;
    absent.xstatus_buy.reset();
    absent.xstatus_sell.reset();

    transaction(bridge, {decoded_deal(buy_active), decoded_deal(sell_active), decoded_deal(contradictory),
                         decoded_deal(absent)});
    const auto trades = bridge.snapshot().trades;
    require(trades.size() == 4, "all distinct flagged trades publish");
    require(trades[0].at_bid_or_ask == 2 && trades[1].at_bid_or_ask == 1,
            "documented ActiveSide/PassiveSide combinations map to DTC side values");
    require(trades[0].xstatus_buy == kActiveSide && trades[0].xstatus_sell == kPassiveSide,
            "raw status flags are preserved");
    require(trades[2].at_bid_or_ask == 0 && trades[3].at_bid_or_ask == 0,
            "contradictory and absent flags remain unknown");
}

void test_lifenum_revision_and_cursor_ring() {
    Plaza2PublicDealsBridge bridge(1001, 321, 2);
    start(bridge, 77);
    online(bridge);

    DealOptions first;
    first.repl_id = 10;
    first.repl_rev = 10;
    first.deal_id = 10;
    DealOptions second = first;
    second.repl_id = 20;
    second.repl_rev = 20; // Revisions need not be consecutive.
    second.deal_id = 20;
    DealOptions third = second;
    third.repl_id = 30;
    third.repl_rev = 30;
    third.deal_id = 30;
    transaction(bridge, {decoded_deal(first)});
    transaction(bridge, {decoded_deal(second)});
    transaction(bridge, {decoded_deal(third)});

    const auto retained = bridge.snapshot();
    require(retained.trades.size() == 2 && retained.first_sequence == 2 && retained.last_sequence == 3,
            "ring wrap reports the oldest retained sequence");
    require(retained.trades[0].sequence == 2 && retained.trades[1].sequence == 3, "ring preserves chronological order");
    const auto after_one = bridge.snapshot(1);
    require(after_one.first_sequence == 2 && after_one.trades.size() == 2,
            "cursor overflow remains detectable while only retained rows are copied");
    const auto after_two = bridge.snapshot(2);
    require(after_two.first_sequence == 2 && after_two.trades.size() == 1 && after_two.trades[0].sequence == 3,
            "cursor snapshot copies only newer retained rows");
    require(bridge.snapshot(3).trades.empty(), "cursor at last sequence returns no trade copies");

    DealOptions stale = third;
    stale.repl_id = 25;
    stale.repl_rev = 25;
    stale.deal_id = 25;
    transaction(bridge, {decoded_deal(stale)});
    require(bridge.valid() && bridge.snapshot().last_sequence == 3,
            "older replRev is skipped without assuming consecutive revisions");

    const auto old_epoch = bridge.snapshot().stream_epoch;
    send(bridge, control(Plaza2ListenerEventKind::LifeNum, 77));
    require(bridge.snapshot().stream_epoch == old_epoch && bridge.snapshot().trades.size() == 2,
            "repeated LifeNum is idempotent");
    send(bridge, control(Plaza2ListenerEventKind::LifeNum, 78));
    auto changed = bridge.snapshot();
    require(changed.stream_epoch == old_epoch + 1 && changed.trades.empty() && !changed.online &&
                changed.last_sequence == 3 && changed.lifenum == 78,
            "changed LifeNum clears retained state and starts a new epoch without rewinding sequence");
    online(bridge);
    DealOptions after_life = first;
    after_life.repl_id = 1;
    after_life.repl_rev = 1;
    after_life.deal_id = 1;
    transaction(bridge, {decoded_deal(after_life)});
    const auto new_epoch_trade = bridge.snapshot();
    require(new_epoch_trade.trades.size() == 1 && new_epoch_trade.trades[0].sequence == 4 &&
                new_epoch_trade.trades[0].stream_epoch == old_epoch + 1 && new_epoch_trade.trades[0].lifenum == 78,
            "new generation resumes with monotonic cursor sequence");
}

void test_bad_transactions_prices_and_lifenum() {
    {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        DealOptions item;
        item.repl_id = 1;
        item.repl_rev = 1;
        item.deal_id = 1;
        send(bridge, decoded_deal(item).event());
        must_fail(bridge, control(Plaza2ListenerEventKind::TransactionBegin), "nested transaction must fail");
        require(bridge.snapshot().trades.empty(), "nested transaction cannot publish pending rows");
    }
    {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        online(bridge);
        must_fail(bridge, control(Plaza2ListenerEventKind::TransactionCommit), "commit without begin must fail");
    }
    {
        Plaza2PublicDealsBridge bridge(1001, 321);
        send(bridge, control(Plaza2ListenerEventKind::Open));
        must_fail(bridge, control(Plaza2ListenerEventKind::Online), "ONLINE without nonzero LifeNum must fail");
    }
    {
        Plaza2PublicDealsBridge bridge(1001, 321);
        send(bridge, control(Plaza2ListenerEventKind::Open));
        must_fail(bridge, control(Plaza2ListenerEventKind::LifeNum, 0), "zero LifeNum must fail closed");
    }
    {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        online(bridge);
        DealOptions invalid_price;
        invalid_price.price = "102500.123456";
        invalid_price.decimal_exact = false;
        invalid_price.repl_id = 1;
        invalid_price.repl_rev = 1;
        invalid_price.deal_id = 1;
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        must_fail(bridge, decoded_deal(invalid_price).event(), "over-precise d16.5 price must fail");
        require(bridge.snapshot().trades.empty(), "malformed row transaction cannot partially publish");
    }
    for (const auto invalid_revision : {std::int64_t{0}, std::int64_t{-1}}) {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        online(bridge);
        DealOptions invalid;
        invalid.repl_id = 1;
        invalid.repl_rev = invalid_revision;
        invalid.deal_id = 1;
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        must_fail(bridge, decoded_deal(invalid).event(), "published source repl_rev must be positive");
        require(bridge.snapshot().trades.empty(), "invalid repl_rev cannot publish a tick");
    }
    for (const auto [price_text, price_mantissa] :
         {std::pair<std::string_view, std::int64_t>{"0.00000", 0}, {"-1.00000", -100000}}) {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        online(bridge);
        DealOptions invalid;
        invalid.repl_id = 1;
        invalid.repl_rev = 1;
        invalid.deal_id = 1;
        invalid.price = price_text;
        invalid.decimal_mantissa = price_mantissa;
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        must_fail(bridge, decoded_deal(invalid).event(), "public price must be positive");
        require(bridge.snapshot().trades.empty(), "nonpositive price cannot publish a tick");
    }
    {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        online(bridge);
        DealOptions inconsistent_decimal;
        inconsistent_decimal.repl_id = 1;
        inconsistent_decimal.repl_rev = 1;
        inconsistent_decimal.deal_id = 1;
        inconsistent_decimal.decimal_mantissa = 10250012501LL;
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        must_fail(bridge, decoded_deal(inconsistent_decimal).event(),
                  "exact runtime decimal and its textual representation must agree");
    }
    for (const auto invalid_moment_ns :
         {std::uint64_t{0}, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U}) {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        online(bridge);
        DealOptions invalid_time;
        invalid_time.repl_id = 1;
        invalid_time.repl_rev = 1;
        invalid_time.deal_id = 1;
        invalid_time.moment_ns = invalid_moment_ns;
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        must_fail(bridge, decoded_deal(invalid_time).event(), "invalid moment_ns must fail closed");
        require(bridge.snapshot().trades.empty(), "invalid timestamp cannot publish a tick");
    }
    {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        online(bridge);
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        DealOptions pending;
        pending.repl_id = 1;
        pending.repl_rev = 1;
        pending.deal_id = 1;
        send(bridge, decoded_deal(pending).event());
        bridge.reset(); // reset/reopen rolls back the open source transaction
        require(bridge.snapshot().trades.empty() && !bridge.online(), "reset discards uncommitted rows");
        const auto prior_epoch = bridge.snapshot().stream_epoch;
        start(bridge);
        require(bridge.snapshot().stream_epoch == prior_epoch + 1,
                "reopen increments epoch while preserving global sequence");
    }
}

void test_conflicting_identity_and_pending_bound() {
    {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        online(bridge);
        DealOptions original;
        original.repl_id = 1;
        original.repl_rev = 1;
        original.deal_id = 1;
        transaction(bridge, {decoded_deal(original)});
        auto conflict = original;
        conflict.price = "102501.12500";
        conflict.decimal_mantissa = 10250112500LL;
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        must_fail(bridge, decoded_deal(conflict).event(), "conflicting repl identity must fail closed");
        require(bridge.snapshot().last_sequence == 1, "conflicting replay does not emit a second tick");
    }
    {
        Plaza2PublicDealsBridge bridge(1001, 321);
        start(bridge);
        online(bridge);
        DealOptions original;
        original.repl_id = 1;
        original.repl_rev = 1;
        original.deal_id = 1;
        transaction(bridge, {decoded_deal(original)});
        auto reused_deal_id = original;
        reused_deal_id.repl_id = 2;
        reused_deal_id.repl_rev = 2;
        reused_deal_id.price = "102501.12500";
        reused_deal_id.decimal_mantissa = 10250112500LL;
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        must_fail(bridge, decoded_deal(reused_deal_id).event(),
                  "reused deal_id with conflicting economics must fail closed");
        require(bridge.snapshot().last_sequence == 1, "conflicting deal_id does not emit a second tick");
    }
    {
        Plaza2PublicDealsBridge bridge(1001, 321, 2);
        start(bridge);
        online(bridge);
        send(bridge, control(Plaza2ListenerEventKind::TransactionBegin));
        DealOptions a;
        a.repl_id = 1;
        a.repl_rev = 1;
        a.deal_id = 1;
        DealOptions b = a;
        b.repl_id = 2;
        b.repl_rev = 2;
        b.deal_id = 2;
        DealOptions c = b;
        c.repl_id = 3;
        c.repl_rev = 3;
        c.deal_id = 3;
        send(bridge, decoded_deal(a).event());
        send(bridge, decoded_deal(b).event());
        must_fail(bridge, decoded_deal(c).event(), "pending transaction must enforce configured bound");
        require(bridge.snapshot().trades.empty(), "pending overflow cannot expose a prefix");
    }
}

} // namespace

int main() {
    try {
        test_snapshot_barrier_and_atomic_commit();
        test_aggressor_status_flags();
        test_lifenum_revision_and_cursor_ring();
        test_bad_transactions_prices_and_lifenum();
        test_conflicting_identity_and_pending_bound();
        std::cout << "PASS plaza2_public_deals_test\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL plaza2_public_deals_test: " << error.what() << '\n';
        return 1;
    }
}
