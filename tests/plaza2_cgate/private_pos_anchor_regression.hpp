#pragma once

#include "fake_cgate_control.hpp"
#include "moex/plaza2_trade/cgate_session.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "plaza2_trade_test_support.hpp"

#include <limits>

namespace moex::plaza2::test {
inline void private_pos_anchor_regression(plaza2_trade::CgateSessionConfig config, const fake::Control& control) {
    using namespace plaza2_trade;
    using enum generated::StreamCode;
    using enum generated::TableCode;
    using enum generated::FieldCode;
    auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    config.recovery_now = [&] { return now; };
    config.trade_replay_from_pos_anchor = true;
    config.mode = CgateSessionMode::Live;
    config.allow_orders = true;
    for (auto code : {kFortsSessionstateRepl, kFortsInstrumentstateRepl}) {
        const auto* descriptor = generated::FindStreamByCode(code);
        config.status_streams.push_back(
            {code, "p2repl://" + std::string(descriptor->stream_name), "mode=snapshot+online"});
    }
    for (auto& stream : config.private_streams)
        if (stream.stream_code == kFortsTradeRepl)
            stream.open_settings += ";lifenum=${POS_TRADES_LIFENUM}";
    config.event_log = {};
    config.listener_event_log = {};
    control.configure({});
    CgateSession session(config);
    require(!session.start(), "POS anchor regression startup");
    const auto poll = [&] {
        now += std::chrono::seconds(1);
        require(!session.poll(), "POS anchor regression poll");
    };
    const auto ready = [&] {
        for (int i = 0; i < 30 && !session.trade_replay_anchor_ready(); ++i)
            poll();
        require(session.trade_replay_anchor_ready(), "committed valid POS info did not make TRADE ready");
    };
    const auto info = [&](std::int64_t revision, std::int64_t trades_rev, std::int64_t life, bool online = false) {
        if (life > 0) {
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsUserorderbookRepl});
            control.enqueue(
                {.stream_code = kFortsUserorderbookRepl,
                 .table_code = kFortsUserorderbookReplInfo,
                 .revision = revision,
                 .fields = {{.field_code = kFortsUserorderbookReplInfoPublicationState, .signed_value = 1},
                            {.field_code = kFortsUserorderbookReplInfoTradesRev, .signed_value = trades_rev},
                            {.field_code = kFortsUserorderbookReplInfoTradesLifenum, .signed_value = life}}});
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = kFortsUserorderbookRepl});
            control.enqueue({.kind = fake::EventKind::Online, .stream_code = kFortsUserorderbookRepl});
        }
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsPosRepl});
        control.enqueue({.kind = fake::EventKind::Row,
                         .stream_code = kFortsPosRepl,
                         .table_code = kFortsPosReplInfo,
                         .revision = revision,
                         .fields = {{.field_code = kFortsPosReplInfoReplId, .signed_value = 7001},
                                    {.field_code = kFortsPosReplInfoReplRev, .signed_value = revision},
                                    {.field_code = kFortsPosReplInfoTradesRev, .signed_value = trades_rev},
                                    {.field_code = kFortsPosReplInfoTradesLifenum, .signed_value = life},
                                    {.field_code = kFortsPosReplInfoServerTime,
                                     .kind = fake::FieldKind::Timestamp,
                                     .unsigned_value = 1700000100}}});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = kFortsPosRepl});
        if (online)
            control.enqueue({.kind = fake::EventKind::Online, .stream_code = kFortsPosRepl});
        poll();
    };
    const auto clear = [&](generated::TableCode table) {
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsPosRepl});
        control.enqueue({.kind = fake::EventKind::ClearDeleted,
                         .stream_code = kFortsPosRepl,
                         .table_code = table,
                         .revision = std::numeric_limits<std::int64_t>::max(),
                         .flags = 8});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = kFortsPosRepl});
        poll();
        poll(); // Anchor supervision runs before the process drain in each owner turn.
    };
    ready();
    const auto initial = session.trade_replay_anchor_used();
    require(initial && initial->trades_rev == 44 && initial->trades_lifenum == 7, "initial native POS anchor");
    const auto settings = control.trade_open_settings();
    const auto life_setting = settings.find("lifenum=");
    require(life_setting != std::string::npos && settings.find("lifenum=", life_setting + 1) == std::string::npos,
            "TRADE replay duplicated an existing lifenum setting");
    const auto opens = control.opens(kFortsTradeRepl);
    auto add_request = test_support::make_add_order();
    add_request.isin_id = 1001;
    const auto add = Plaza2TradeCodec{}.encode(add_request);
    auto move_request = test_support::make_move_order();
    move_request.isin_id = 1001;
    const auto move = Plaza2TradeCodec{}.encode(move_request);
    const auto posted_before = control.commands().size();
    require(session.post_command(add, 810001).certainty == cgate::Plaza2SubmissionCertainty::Posted,
            "valid POS anchor did not permit the baseline Add");
    clear(kFortsPosReplPosition);
    require(session.private_state().positions().empty(), "position clear did not retire its row");
    const auto unchanged = session.trade_replay_anchor_used();
    require(session.trade_replay_anchor_ready() && unchanged && unchanged->trades_rev == initial->trades_rev &&
                unchanged->trades_lifenum == initial->trades_lifenum && control.opens(kFortsTradeRepl) == opens,
            "ordinary POS position clear reopened TRADE or reset its independent info anchor");

    clear(kFortsPosReplInfo);
    require(!session.trade_replay_anchor_ready() && !session.runtime_health().private_active &&
                control.opens(kFortsTradeRepl) == opens,
            "absent POS info reopened TRADE at zero or remained entry-ready");
    const auto rejected = session.post_command(add, 810002);
    require(rejected.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent && !rejected.post_invoked &&
                session.post_command(move, 810003).certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent &&
                control.commands().size() == posted_before + 1,
            "absent POS info still admitted Add/Move to the native publisher");
    const auto cancel = Plaza2TradeCodec{}.encode(test_support::make_del_order());
    require(session.post_command(cancel, 810004).certainty == cgate::Plaza2SubmissionCertainty::Posted,
            "absent POS info blocked risk-reducing cancellation");
    info(100, 0, 0);
    require(!session.trade_replay_anchor_ready() && control.opens(kFortsTradeRepl) == opens,
            "zero POS anchor values opened/re-anchored TRADE");
    info(101, 55, 8);
    ready();
    require(control.opens(kFortsTradeRepl) == opens + 1 && session.trade_replay_anchor_used()->trades_rev == 55 &&
                session.trade_replay_anchor_used()->trades_lifenum == 8,
            "valid new POS info did not reopen TRADE exactly once at its committed anchor");

    control.enqueue({.kind = fake::EventKind::LifeNum, .stream_code = kFortsPosRepl, .value = 100});
    poll();
    control.enqueue({.kind = fake::EventKind::LifeNum, .stream_code = kFortsPosRepl, .value = 101});
    poll();
    require(!session.trade_replay_anchor_ready() && !session.runtime_health().private_active &&
                control.opens(kFortsTradeRepl) == opens + 1,
            "POS LifeNum loss kept stale anchor readiness or reopened without info");
    info(102, 66, 9, true);
    ready();
    require(control.opens(kFortsTradeRepl) == opens + 2 && session.trade_replay_anchor_used()->trades_rev == 66 &&
                session.trade_replay_anchor_used()->trades_lifenum == 9,
            "new POS epoch did not bind TRADE to fresh positive info");
    const auto posted = control.commands();
    require(posted.size() == posted_before + 2 && posted[posted_before].name == "AddOrder" &&
                posted.back().name == "DelOrder",
            "invalid POS anchor allowed an additional Add/Move publisher call");
    require(!session.stop(), "POS anchor regression stop");
    for (auto& stream : config.private_streams)
        if (stream.stream_code == kFortsTradeRepl)
            stream.open_settings += ";lifenum=999";
    control.configure({});
    const auto duplicate_opens = control.opens(kFortsTradeRepl);
    CgateSession duplicate(config);
    require(!duplicate.start(), "duplicate lifenum regression startup");
    for (int i = 0; i < 30 && duplicate.last_callback_error().empty(); ++i)
        require(!duplicate.poll(), "duplicate lifenum recovery poll");
    require(duplicate.last_callback_error().find("duplicate lifenum") != std::string::npos &&
                control.opens(kFortsTradeRepl) == duplicate_opens,
            "ambiguous duplicate lifenum did not stay down with a visible configuration error");
    require(!duplicate.stop(), "duplicate lifenum regression stop");
}
} // namespace moex::plaza2::test
