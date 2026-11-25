#pragma once
#include "full_order_log_live_probe.hpp"
#include <sstream>
#include <stdexcept>

namespace full_order_log_probe {
inline void run_content_review_tests() {
    const auto expect = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    constexpr std::uint64_t base = 1700000000000000000ULL;
    const auto event = [](std::int8_t action, std::int64_t revision, std::int64_t remaining, std::uint64_t moment,
                          std::uint64_t sequence = 1) {
        return ContentWindow::Change{action, revision, remaining, moment, moment, true, sequence};
    };
    const auto metadata = [](std::uint64_t moment, std::int64_t revision = 1) {
        return PublicationInfo{.provided = true,
                               .info_revision = revision,
                               .trades_revision = revision * 100,
                               .log_life = 7,
                               .end_utc_ns = moment,
                               .received_utc_ns = base + 151000000000ULL};
    };
    std::ostringstream output;
    ContentWindow content(2000, 64, &output);
    std::uint64_t utc = base + 140000000000ULL;
    content.utc_clock = [&] { return utc; };
    OrderSet raw, composite;
    // Same lifecycle pattern as 25 November, with synthetic public values and IDs.
    for (std::int64_t id = 1; id <= 5; ++id) {
        raw[id] = {10, 100, 42, true};
        content.change(11, id, event(0, id, 0, base + 10000000000ULL));
    }
    for (std::int64_t id = 6; id <= 8; ++id) {
        composite[id] = {5, 100, 42, false};
        content.change(11, id, event(1, id, 5, base + 20000000000ULL));
    }
    raw[9] = {30, 100, 42, true};
    composite[9] = {16, 100, 42, true};
    for (std::int64_t i = 1; i <= 14; ++i)
        content.change(11, 9, event(2, 100 + i, 30 - i, base + i * 1000000000ULL));
    raw[10] = {10, 100, 42, false};
    composite[10] = {7, 100, 42, false};
    content.change(11, 10, event(2, 200, 7, base + 30000000000ULL));
    content.record(11, composite, 100);
    content.raw(11, raw, 101, 102, metadata(base));
    content.poll(2103);
    expect(content.pre_connection == 1 && content.stale_hypothesis_proven == 1 && content.suspected_defects == 0 &&
               content.different == 0,
           "five deletes, three adds and two partially filled orders explain a stale publication");
    const auto json = output.str();
    expect(json.find("\"missing_orders\":5,\"extra_orders\":3,\"changed_orders\":2") != std::string::npos &&
               json.find("\"stale_snapshot_hypothesis\":\"proven_for_every_differing_id\"") != std::string::npos,
           "the observed lifecycle pattern is proven, not a suspected defect");
    expect(
        json.find("\"raw_order\":{\"remaining\":\"30\",\"price\":\"100\",\"session_id\":42,\"bid\":true}") !=
                std::string::npos &&
            json.find("\"composite_order\":{\"remaining\":\"16\",\"price\":\"100\",\"session_id\":42,\"bid\":true}") !=
                std::string::npos &&
            json.find("\"raw_order\":null") != std::string::npos &&
            json.find("\"composite_order\":null") != std::string::npos,
        "difference diagnostics retain both complete tuples and explicit absent sides");

    using Value = OrderSet::mapped_type;
    const Value before{10, 100, 42, true}, after{7, 100, 42, true};
    const auto classify = [&](Value first, Value last, std::vector<ContentWindow::Change> changes,
                              bool evicted = false) {
        ContentWindow window;
        window.utc_clock = [&] { return utc; };
        for (const auto& change : changes)
            window.change(11, 1, change);
        window.record(11, OrderSet{{1, last}}, 100);
        window.change_evictions = evicted;
        ContentWindow::Publication publication{
            .isin = 11, .image = window.image(OrderSet{{1, first}}), .info = metadata(base)};
        return window.hypothesis(publication, &window.history.at(11).back());
    };
    const auto fill = event(2, 10, 7, base + 2000000);
    expect(classify(before, after, {fill}) == 1, "a complete positive partial fill fits");
    for (const auto& wrong :
         {Value{7, 101, 42, true}, Value{7, 100, 43, true}, Value{7, 100, 42, false}, Value{11, 100, 42, true}})
        expect(classify(before, wrong, {fill}) == 2, "price, session, side or remaining increase remains contrary");
    expect(classify(before, before, {fill}) == 0, "identical tuples do not invent a changed-order proof");
    expect(classify(before, after, {}) == 0, "missing fill history is unproven");
    expect(classify(before, after, {fill}, true) == 0, "evicted history is not complete proof");
    for (const auto action : {std::int8_t{0}, std::int8_t{1}, std::int8_t{3}}) {
        auto wrong = fill;
        wrong.action = action;
        expect(classify(before, after, {wrong}) == 2, "a post-snapshot non-trade action remains contrary");
    }
    auto wrong = fill;
    wrong.remaining = 0;
    expect(classify(before, after, {wrong}) == 2, "a completed fill cannot explain a still-resting order");
    wrong = fill;
    wrong.remaining = 8;
    expect(classify(before, after, {wrong}) == 2, "last committed fill must match composite remaining");
    wrong = fill;
    wrong.consistent = false;
    expect(classify(before, after, {wrong}) == 0, "inconsistent timestamps make history unproven");
    wrong = fill;
    wrong.moment_ns = 0;
    expect(classify(before, after, {wrong}) == 0, "unknown event time cannot prove a post-snapshot fill");
    wrong = fill;
    wrong.moment_ns = base - 1000000;
    expect(classify(before, after, {wrong}) == 0, "an old fill alone cannot explain a later quantity difference");
    wrong.moment_ns = base + ContentWindow::time_precision_ns - 1;
    expect(classify(before, after, {wrong}) == 0, "a fill inside snapshot timestamp precision is unproven");
    wrong.moment_ns = base + ContentWindow::time_precision_ns;
    expect(classify(before, after, {wrong}) == 1, "the existing exact precision boundary is retained");
    wrong = fill;
    wrong.committed_sequence = 2;
    expect(classify(before, after, {wrong}) == 0, "events after the sampled commit cannot explain that state");
    wrong = fill;
    wrong.revision = 0;
    expect(classify(before, after, {wrong}) == 0, "unknown revision cannot identify the last fill");
    expect(classify(before, after, {event(1, 1, 10, base - 1000000000ULL), fill}) == 1,
           "pre-snapshot lifecycle events do not veto valid later partial fills");
    expect(classify(before, after, {event(2, 9, 9, base + 1000000), fill}) == 1,
           "the last of several committed partial fills determines the remaining quantity");

    // Explaining the initial publication must not excuse a genuinely fresh difference.
    utc = base + 151000000000ULL;
    content.record(11, composite, 10000);
    auto changed = composite;
    std::get<0>(changed.at(9)) = 15;
    content.raw(11, changed, 10001, 10002, metadata(base + 150000000000ULL, 2));
    content.poll(12003);
    expect(content.fresh_different == 1 && content.different == 1 && !content.fresh_check_pass(600),
           "a later fresh mismatch remains fatal even after explained startup catch-up");
    const std::array<std::int32_t, 1> ids{11};
    const StartupComparison startup(ids, 100);
    expect(comparison_exit_code(Window{}, content, startup) == 4,
           "the executable's result function must still return exit 4 for a later fresh mismatch");
}
} // namespace full_order_log_probe
