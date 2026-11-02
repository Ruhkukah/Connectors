#pragma once

#include "position_lag_regression.hpp"

namespace moex::connector_host::regression::pos_cutoff {
using position_lag::check;
using position_lag::fill;
using position_lag::Fixture;
using position_lag::observe;
using position_lag::request;

inline void cutoff_partition_is_recomputed() {
    Fixture f;
    auto manager = f.manager();
    auto before = fill(88001, 1);
    auto after = fill(88002, 1);
    after.repl_rev = 201;
    observe(manager, before);
    observe(manager, after);
    f.position = 2;
    f.proof->bought = 2;
    f.proof->last_deal_id = after.id_deal;
    check(manager.place(request("cutoff-original", 1)).empty(), "initial exact POS proof refused");
    check(manager.cancel("cutoff-original").empty(), "unsent original cutoff order cancel");

    f.proof->calendar_revision = 150;
    check(!manager.place(request("cutoff-without-day-open", 1)).empty(),
          "advanced info anchor bypassed the pre-cutoff fill's day-open proof");
    f.proof->day_open_bought = 1;
    check(manager.place(request("cutoff-between-fills", 1)).empty(),
          "validated cutoff advance moved a later covered fill into calendar coverage");
    check(manager.cancel("cutoff-between-fills").empty(), "unsent repartitioned cutoff order cancel");

    f.proof->calendar_revision = 202;
    check(!manager.place(request("cutoff-insufficient-prefix", 1)).empty(),
          "advanced cutoff consumed insufficient committed day-open quantity");
    f.proof->day_open_bought = 2;
    check(manager.place(request("cutoff-covers-both", 1)).empty(),
          "validated day-open proof did not cover both source revisions");
}

inline void info_only_does_not_clear_reservations() {
    Fixture f;
    auto manager = f.manager();
    auto row = fill(88003);
    observe(manager, row);
    f.proof->calendar_revision = 200;
    check(!manager.place(request("cutoff-info-only", 2)).empty(),
          "POS.info advancement alone released a lagged own fill");
    f.position = 2;
    f.proof->bought = 2;
    f.proof->last_deal_id = row.id_deal;
    check(!manager.place(request("cutoff-last-deal-only", 1)).empty(),
          "post-cutoff marker bypassed missing calendar prefix coverage");
    f.proof->day_open_bought = 2;
    check(manager.place(request("cutoff-committed-prefix", 1)).empty(),
          "committed position and day-open proof did not release the calendar fill");
}

inline void conflict_needs_later_position_proof() {
    Fixture f;
    f.config.risk.max_position_by_isin[42] = 4;
    auto manager = f.manager();
    auto original = fill(88004);
    auto contradictory = original;
    contradictory.amount = 1;
    contradictory.repl_rev = 102;
    observe(manager, original);
    observe(manager, contradictory);
    check(!manager.place(request("conflict-original-proof", 1)).empty(),
          "contradictory own fill allowed new risk before POS proof");
    f.proof->calendar_revision = 200;
    check(!manager.place(request("conflict-info-only", 1)).empty(),
          "an info revision alone cleared conflicting fill evidence");

    auto later = fill(88005, 1);
    later.repl_rev = 201;
    observe(manager, later);
    f.position = 3;
    f.proof->bought = 3;
    f.proof->day_open_bought = 2;
    f.proof->last_deal_id = 999999;
    check(!manager.place(request("conflict-unknown-marker", 1)).empty(),
          "unknown later POS deal ID cleared conflicting source evidence");
    f.proof->last_deal_id = later.id_deal;
    f.proof->bought = 2;
    check(!manager.place(request("conflict-insufficient-gross", 1)).empty(),
          "insufficient current POS side counter cleared conflicting fills");
    f.proof->bought = 3;
    check(manager.place(request("conflict-reconciled", 1)).empty(),
          "later exact POS proof covering conservative fill quantities remained permanently blocked");
    check(manager.cancel("conflict-reconciled").empty(), "unsent reconciled conflict order cancel");

    observe(manager, contradictory);
    check(manager.place(request("conflict-covered-replay", 1)).empty(),
          "replay of already covered contradictory evidence reblocked the instrument");
    check(manager.cancel("conflict-covered-replay").empty(), "unsent covered conflict replay order cancel");
    contradictory.amount = 3;
    contradictory.repl_rev = 202;
    observe(manager, contradictory);
    check(!manager.place(request("conflict-new-evidence", 1)).empty(),
          "newer conflicting evidence reused the earlier POS proof");
    check(std::count(f.events.begin(), f.events.end(), "trade_source_conflict") == 1,
          "one conflicting deal generated duplicate source-conflict warnings");
}

inline void missing_source_conflict_cannot_reuse_cached_proof() {
    Fixture f;
    f.config.risk.max_position_by_isin[42] = 5;
    auto manager = f.manager();
    auto original = fill(88101);
    auto marker = fill(88102, 1);
    marker.repl_rev = 201;
    observe(manager, original);
    observe(manager, marker);
    f.position = 4;
    f.proof->bought = 4;
    f.proof->last_deal_id = marker.id_deal;
    check(manager.place(request("missing-source-covered", 1)).empty(), "valid initial source proof refused");
    check(manager.cancel("missing-source-covered").empty(), "unsent missing-source order cancel");

    auto changed = original;
    changed.amount = 3;
    changed.trade_lifenum = 0;
    changed.repl_rev = 0;
    observe(manager, changed);
    check(!manager.place(request("missing-source-conflict", 1)).empty(),
          "missing-source contradiction reused already cached POS proof");
    observe(manager, original);
    check(!manager.place(request("missing-source-old-replay", 1)).empty(),
          "old valid source replay cleared an unproven conflicting quantity");
    changed.trade_lifenum = 7;
    changed.repl_rev = 102;
    observe(manager, changed);
    check(!manager.place(request("missing-source-before-bound", 1)).empty(),
          "valid conflicting revision below cached POS bound cleared unproven evidence");
    changed.repl_rev = 202;
    observe(manager, changed);
    check(!manager.place(request("missing-source-after-bound", 1)).empty(),
          "new source revision alone released conflicting quantity before later POS proof");

    auto later = fill(88103, 1);
    later.code_buy = "OTHER01";
    later.code_sell = "ABCD001";
    later.repl_rev = 301;
    observe(manager, later);
    f.position = 3;
    f.proof->sold = 1;
    f.proof->last_deal_id = later.id_deal;
    check(manager.place(request("missing-source-later-proof", 1)).empty(),
          "new conservative source and later exact POS proof failed to reconcile uncertainty");
    check(std::count(f.events.begin(), f.events.end(), "trade_source_conflict") == 1,
          "missing-source conflict replay duplicated its immutable source warning");
}

inline void restored_quantity_needs_new_source_and_pos_proof() {
    Fixture f;
    f.config.risk.max_position_by_isin[42] = 5;
    auto manager = f.manager();
    auto original = fill(88201);
    auto marker = fill(88202, 1);
    marker.repl_rev = 201;
    observe(manager, original);
    observe(manager, marker);
    f.position = 4;
    f.proof->bought = 4;
    f.proof->last_deal_id = marker.id_deal;
    check(manager.place(request("restored-source-covered", 1)).empty(), "initial restored-source proof refused");
    check(manager.cancel("restored-source-covered").empty(), "unsent restored-source order cancel");

    auto missing = original;
    missing.amount = 1;
    missing.trade_lifenum = 0;
    missing.repl_rev = 0;
    observe(manager, missing);
    check(!manager.place(request("restored-source-missing", 1)).empty(),
          "missing smaller contradiction released its conservative quantity");
    observe(manager, original);
    check(!manager.place(request("restored-source-old", 1)).empty(),
          "old agreeing source replay cleared unproven evidence");
    original.repl_rev = 102;
    observe(manager, original);
    check(!manager.place(request("restored-source-before-bound", 1)).empty(),
          "agreeing source below cached POS bound cleared unproven evidence");
    original.repl_rev = 301;
    observe(manager, original);
    check(!manager.place(request("restored-source-without-pos", 1)).empty(),
          "newer agreeing source released quantity before later POS proof");

    auto later = fill(88203, 1);
    later.repl_rev = 401;
    observe(manager, later);
    f.proof->last_deal_id = later.id_deal;
    check(manager.place(request("restored-source-later-proof", 1)).empty(),
          "newer agreeing source and later exact POS proof remained blocked");
    check(std::count(f.events.begin(), f.events.end(), "trade_source_conflict") == 1,
          "agreeing source repair erased or duplicated the immutable conflict warning");
}

inline void run() {
    cutoff_partition_is_recomputed();
    info_only_does_not_clear_reservations();
    conflict_needs_later_position_proof();
    missing_source_conflict_cannot_reuse_cached_proof();
    restored_quantity_needs_new_source_and_pos_proof();
}
} // namespace moex::connector_host::regression::pos_cutoff

namespace moex::connector_host {
inline void pos_cutoff_regression() {
    regression::pos_cutoff::run();
}
} // namespace moex::connector_host
