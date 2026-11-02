#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"
#include "plaza2_runtime_test_support.hpp"
#include <array>
#include <iostream>

using namespace moex::plaza2::cgate;
using moex::plaza2::test::require;
using F = moex::plaza2::generated::FieldCode;
using T = moex::plaza2::generated::TableCode;
Plaza2DecodedFieldValue integer(F field, std::int64_t value) {
    return {.field_code = field, .kind = Plaza2DecodedValueKind::SignedInteger, .signed_value = value};
}
int main() {
    try {
        Plaza2Aggr20BookProjector projector;
        Plaza2Aggr20ListenerBridge bridge(projector, 321);
        const auto event = [&](Plaza2ListenerEventKind kind) {
            require(!bridge.on_plaza2_listener_event({.kind = kind}), "listener event succeeds");
        };
        const auto sys = [&](int session, int type, int revision) {
            const std::array fields{integer(F::kFortsAggrReplSysEventsReplId, revision),
                                    integer(F::kFortsAggrReplSysEventsReplRev, revision),
                                    integer(F::kFortsAggrReplSysEventsSessId, session),
                                    integer(F::kFortsAggrReplSysEventsEventType, type)};
            require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                      .table_code = T::kFortsAggrReplSysEvents,
                                                      .fields = fields}),
                    "sys event decoded");
        };
        const auto book_row = [&] {
            const std::array fields{integer(F::kFortsAggrReplOrdersAggrReplId, 1),
                                    integer(F::kFortsAggrReplOrdersAggrReplRev, 1),
                                    integer(F::kFortsAggrReplOrdersAggrIsinId, 1001),
                                    integer(F::kFortsAggrReplOrdersAggrVolume, 2),
                                    integer(F::kFortsAggrReplOrdersAggrDir, 1),
                                    Plaza2DecodedFieldValue{.field_code = F::kFortsAggrReplOrdersAggrPrice,
                                                            .kind = Plaza2DecodedValueKind::Decimal,
                                                            .text_value = "100.00000"}};
            require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                      .table_code = T::kFortsAggrReplOrdersAggr,
                                                      .fields = fields}),
                    "fresh snapshot book row decoded");
        };
        event(Plaza2ListenerEventKind::Open);
        event(Plaza2ListenerEventKind::TransactionBegin);
        book_row();
        event(Plaza2ListenerEventKind::Online);
        require(!bridge.valid() && !bridge.snapshot_complete(), "ONLINE cannot authorize an open snapshot transaction");
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(!bridge.valid(), "committed snapshot still requires ONLINE");
        event(Plaza2ListenerEventKind::Online);
        require(bridge.valid() && !bridge.session_data_ready() && !bridge.status().ready_event &&
                    projector.snapshot_for_isin(1001)->row_count == 1,
                "fresh committed snapshot plus ONLINE authorizes late start without session_data_ready");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(321, 5, 2);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(!bridge.valid(), "clearing revokes snapshot authority even when no ready event was received");
        event(Plaza2ListenerEventKind::Online);
        require(!bridge.valid(), "duplicate ONLINE cannot undo a subsequent online clearing event");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .table_code = T::kFortsAggrReplSysEvents,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "clearing annotation can be pruned");
        require(!bridge.valid(), "pruning sys_events cannot undo explicit clearing revocation");
        event(Plaza2ListenerEventKind::Close);
        require(!bridge.valid() && !bridge.snapshot_complete() && !projector.snapshot_for_isin(1001),
                "CLOSE discards the prior generation snapshot");
        event(Plaza2ListenerEventKind::Online);
        require(!bridge.valid(), "ONLINE before reopen cannot reuse the prior snapshot");
        event(Plaza2ListenerEventKind::Open);
        event(Plaza2ListenerEventKind::TransactionBegin);
        book_row();
        sys(320, 5, 1);
        sys(321, 5, 2);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(!bridge.valid(), "fresh reconnect snapshot remains blocked before ONLINE");
        event(Plaza2ListenerEventKind::Online);
        require(bridge.valid() && !bridge.session_data_ready(),
                "fresh reconnect ONLINE supersedes archived clearing history without session_data_ready");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::LifeNum, .unsigned_value = 99}),
                "new no-ready replication life succeeds");
        require(!bridge.valid() && !bridge.snapshot_complete() && !projector.snapshot_for_isin(1001),
                "LifeNum change discards a no-ready synchronized snapshot");
        event(Plaza2ListenerEventKind::TransactionBegin);
        book_row();
        event(Plaza2ListenerEventKind::TransactionCommit);
        event(Plaza2ListenerEventKind::Online);
        require(bridge.valid() && !bridge.session_data_ready(),
                "new LifeNum fresh snapshot plus ONLINE needs no ready event");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(320, 5, 2);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(bridge.valid(), "online clearing for a known different session does not revoke the current book");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(0, 5, 3);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(!bridge.valid(), "online clearing with unknown session fails closed without a ready event");
        event(Plaza2ListenerEventKind::Close);
        event(Plaza2ListenerEventKind::Open);
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(321, 1, 1);
        require(!bridge.session_data_ready(), "uncommitted ready event cannot make book valid");
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(!bridge.valid(), "snapshot needs ONLINE");
        event(Plaza2ListenerEventKind::Online);
        require(bridge.valid(), "snapshot ready row matching current day validates late join");
        // Quiet books remain valid: no timestamp of the last price change is used.
        event(Plaza2ListenerEventKind::Timeout);
        require(bridge.valid(), "idle stream does not make a quiet book stale");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(321, 5, 2);
        require(!bridge.status().valid, "open transaction is not exposed as current");
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(!bridge.valid(), "later clearing_started revokes current-day book");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(321, 1, 1);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(!bridge.valid(), "older replayed ready event cannot undo later clearing");
        event(Plaza2ListenerEventKind::Close);
        event(Plaza2ListenerEventKind::Open);
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(321, 5, 2);
        sys(321, 1, 1);
        event(Plaza2ListenerEventKind::TransactionCommit);
        event(Plaza2ListenerEventKind::Online);
        require(bridge.valid(), "fresh snapshot ONLINE supersedes historical clearing regardless of row order");
        event(Plaza2ListenerEventKind::Close);
        event(Plaza2ListenerEventKind::Open);
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(320, 1, 3);
        event(Plaza2ListenerEventKind::TransactionCommit);
        event(Plaza2ListenerEventKind::Online);
        require(bridge.valid() && !bridge.session_data_ready(),
                "fresh snapshot ONLINE authorizes reconnect despite an archived previous-day ready row");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(321, 1, 4);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(bridge.valid(), "current committed ready event restores book validity");
        bridge.set_session_id(322);
        require(!bridge.valid(), "session change invalidates previous ready event");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(322, 1, 5);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(bridge.valid(), "new trading day resumes without process restart");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::LifeNum, .unsigned_value = 7}),
                "new replication life succeeds");
        require(!bridge.valid() && !bridge.online(), "new replication life invalidates the previous snapshot");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(322, 1, 1);
        event(Plaza2ListenerEventKind::TransactionCommit);
        event(Plaza2ListenerEventKind::Online);
        require(bridge.valid(), "revision one is valid in a new replication life");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::LifeNum, .unsigned_value = 7}),
                "duplicate replication life succeeds");
        require(bridge.valid(), "duplicate LifeNum must preserve the committed snapshot");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .table_code = T::kFortsAggrReplSysEvents,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "sys_events MAX ClearDeleted succeeds without listener teardown");
        require(bridge.valid() && bridge.online() && !bridge.session_data_ready(),
                "sys_events MAX drops ready annotation without revoking the synchronized snapshot");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(322, 1, 1);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(bridge.valid(), "revision one ready event is accepted after sys_events MAX");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .table_code = T::kFortsAggrReplSysEvents,
                                                  .signed_value = 1}),
                "normal sys_events clear succeeds");
        require(bridge.valid(), "normal sys_events clear preserves rows at its revision boundary");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(322, 5, 2);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(!bridge.valid(), "revision two clearing event invalidates after sys_events MAX");
        event(Plaza2ListenerEventKind::TransactionBegin);
        sys(322, 1, 8);
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .table_code = T::kFortsAggrReplSysEvents,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "transaction-scoped sys_events MAX succeeds");
        sys(322, 1, 1);
        sys(322, 5, 2);
        event(Plaza2ListenerEventKind::TransactionCommit);
        require(!bridge.valid(), "MAX revision epoch is applied in source order within one transaction");
        event(Plaza2ListenerEventKind::LifeNum);
        require(!bridge.valid() && !bridge.online(), "LifeNum requires a fresh snapshot");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
