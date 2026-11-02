#pragma once
#include "late_move_host_regression.hpp"

namespace moex::connector_host {
inline void inflight_rebuild_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                             const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    using enum gen::StreamCode;
    using enum gen::TableCode;
    for (const int mode : {0, 1, 2}) {
        const bool history_reload = mode != 0, aborted = mode == 2;
        const auto utc = aborted ? std::int64_t{1699989000} : std::int64_t{1700000060};
        config.utc_now = [utc] { return utc; };
        config.journal_path = root / ("inflight-rebuild-" + std::to_string(mode) + ".ndjson");
        config.identity_state_path = config.journal_path.string() + ".state";
        control.configure({.suppress_auto_replies = true,
                           .suppress_initial_orders = true,
                           .zero_position = true,
                           .client_code = config.orders.broker_code + config.orders.client_code,
                           .session_id = 321});
        CgateTradingHost host(config);
        bootstrap(host);
        require(host.place(add("inflight", config.isin_ids.front())).empty(), "inflight seed refused");
        official_cgate99::FORTS_MSG179 accepted{};
        accepted.order_id = 99500;
        late_move_host_detail::reply(control, control.commands().back().user_id, 179, accepted);
        require(!host.poll(), "inflight reply poll");
        const auto posts = control.commands().size();
        if (history_reload) {
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsTradeRepl});
            control.enqueue({.kind = fake::EventKind::ClearDeleted,
                             .stream_code = kFortsTradeRepl,
                             .table_code = kFortsTradeReplOrdersLog,
                             .revision = INT64_MAX,
                             .flags = 8});
            control.enqueue(
                {.kind = aborted ? fake::EventKind::Close : fake::EventKind::Commit, .stream_code = kFortsTradeRepl});
        } else
            control.enqueue({.kind = fake::EventKind::Close, .stream_code = kFortsUserorderbookRepl});
        require(!host.poll(), "inflight recovery barrier poll");
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsUserorderbookRepl});
        control.enqueue({.stream_code = kFortsUserorderbookRepl,
                         .table_code = kFortsUserorderbookReplInfo,
                         .revision = 80000,
                         .fields = {integer(kFortsUserorderbookReplInfoPublicationState, 1),
                                    integer(kFortsUserorderbookReplInfoTradesRev, 1001),
                                    integer(kFortsUserorderbookReplInfoTradesLifenum, 7)}});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = kFortsUserorderbookRepl});
        control.enqueue({.kind = fake::EventKind::Online, .stream_code = kFortsUserorderbookRepl});
        control.enqueue({.kind = fake::EventKind::Online, .stream_code = kFortsTradeRepl});
        for (int i = 0; i < 10; ++i)
            require(!host.poll(), "inflight completed recovery poll");
        require(host.status().find("\"reconstructing\":false") != std::string::npos,
                "inflight recovery did not complete");
        require(control.commands().size() == posts &&
                    late_move_host_detail::logical_order(host, "inflight").find("\"state\":\"Working\"") !=
                        std::string::npos,
                "rebuild cancelled a confirmed order whose replication was still in flight");
        require(!host.stop(), "inflight recovery stop");
    }
}
} // namespace moex::connector_host
