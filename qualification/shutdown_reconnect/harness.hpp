#pragma once
#ifndef MOEX_ISOLATED_SHUTDOWN_HARNESS
#error "Shutdown harness requires the separately linked no-post build"
#endif
#include "moex/connector_host/trading_host.hpp"
#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

namespace moex::connector_host {
enum class ShutdownCase { Healthy, RecoveredEmpty, Unavailable, SecondLoss };
struct ShutdownHarnessResult {
    bool admitted{}, loss_observed{}, deferred_observed{}, recovered{}, second_loss_observed{}, timed_out{};
    bool reconciliation_pending{}, pending_cancellations{}, working_orders{}, stop_ok{};
    int shutdown_exit{8};
    std::string failure;
};
struct HarnessFailure {
    const char* code;
};
class ShutdownReconnectHarness {
  public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    static ShutdownHarnessResult run(
        TradingHostConfig config, ShutdownCase scenario, Now now = [] { return Clock::now(); },
        std::function<void()> step = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }) {
        ShutdownHarnessResult result;
        // Fail before opening files/runtime. The caller cannot use this tool for
        // another venue, partial ownership, missing private reconstruction or entry.
        if (config.session.runtime.environment != plaza2::cgate::Plaza2Environment::Test ||
            !config.session.allow_orders || config.session.read_only_market_data || !config.orders.sole_instance ||
            config.session.private_streams.size() != 5 || config.session.status_streams.size() != 2 ||
            !config.session.trade_replay_from_pos_anchor || config.isin_ids.empty() ||
            config.orders.ext_id_end - static_cast<std::int64_t>(config.orders.ext_id_begin) + 1 < 7000 ||
            config.orders.risk.max_quantity > 2 || config.orders.risk.max_open_orders > 5 ||
            config.orders.max_commands_per_second > 5 ||
            std::any_of(config.orders.risk.max_position_by_isin.begin(), config.orders.risk.max_position_by_isin.end(),
                        [](const auto& cap) { return cap.second > 2; })) {
            result.failure = "invalid_isolated_test_profile";
            return result;
        }
        config.session.recovery_now = now;
        config.orders.risk.kill_switch = true;
        CgateTradingHost host(std::move(config));
        try {
            const auto reserved = host.journal_.reservations();
            const auto next =
                std::max({host.config_.orders.ext_id_begin, host.config_.orders.next_ext_id, reserved.next_ext_id});
            if (static_cast<std::int64_t>(host.config_.orders.ext_id_end) - next + 1 < 7000)
                throw HarnessFailure{"remaining_id_capacity_unproven"};
            if (host.start())
                throw HarnessFailure{"start_failed"};
            const auto admission_end = now() + std::chrono::seconds(15);
            while (!fresh_flat(host) && now() < admission_end) {
                if (host.poll())
                    throw HarnessFailure{"admission_poll_failed"};
                step();
            }
            if (!fresh_flat(host))
                throw HarnessFailure{"fresh_flat_admission_unproven"};
            result.admitted = true;
            if (scenario != ShutdownCase::Healthy) {
                if (host.session_.qualification_close_owned_connection(true) || host.poll())
                    throw HarnessFailure{"owned_connection_close_failed"};
                result.loss_observed = !host.cancellation_snapshot_ready();
                if (!result.loss_observed)
                    throw HarnessFailure{"loss_not_observed"};
            }
            host.set_kill_switch(true);
            const auto drain_end = now() + std::chrono::seconds(10);
            for (auto isin : host.config_.isin_ids)
                host.request_shutdown_cancellation(isin);
            result.deferred_observed = host.shutdown_reconciliation_pending() && !host.has_pending_cancellations();
            if (scenario != ShutdownCase::Healthy && !result.deferred_observed)
                throw HarnessFailure{"shutdown_not_deferred"};
            // Two real recovery polls under held reopen prove that cached
            // empty orders do not complete the shutdown request.
            if (scenario != ShutdownCase::Healthy) {
                for (int i = 0; i < 2; ++i) {
                    if (host.poll())
                        throw HarnessFailure{"held_recovery_poll_failed"};
                    if (!host.shutdown_reconciliation_pending() || host.has_pending_cancellations())
                        throw HarnessFailure{"unknown_history_accepted_or_cancel_queued"};
                    step();
                }
                if (scenario != ShutdownCase::Unavailable)
                    host.session_.qualification_allow_reopen();
            }
            while (now() < drain_end) {
                if (host.poll())
                    throw HarnessFailure{"drain_poll_failed"};
                // Private cancellation proof can arrive before reply/AGGR/status
                // recovery. Keep all fresh-flat gates, but give them the original
                // drain budget instead of treating their transient delay as failure.
                if (!host.shutdown_reconciliation_pending() && fresh_flat(host)) {
                    result.recovered = scenario != ShutdownCase::Healthy;
                    if (scenario == ShutdownCase::SecondLoss && !result.second_loss_observed) {
                        if (host.session_.qualification_close_owned_connection(true) || host.poll())
                            throw HarnessFailure{"second_close_failed"};
                        result.second_loss_observed = host.shutdown_reconciliation_pending();
                        if (!result.second_loss_observed)
                            throw HarnessFailure{"second_loss_retained_stale_proof"};
                    } else
                        break;
                }
                step();
            }
            if (!host.shutdown_reconciliation_pending() && !fresh_flat(host)) {
                result.timed_out = now() >= drain_end;
                throw HarnessFailure{"recovered_flatness_unproven"};
            }
            result.reconciliation_pending = host.shutdown_reconciliation_pending();
            result.pending_cancellations = host.has_pending_cancellations();
            result.working_orders = host.has_working_orders();
            result.timed_out = now() >= drain_end &&
                               (result.reconciliation_pending || result.pending_cancellations || result.working_orders);
            host.record_shutdown_drain("isolated_test", result.timed_out);
            result.shutdown_exit = result.timed_out || result.reconciliation_pending || result.pending_cancellations ||
                                           result.working_orders
                                       ? 8
                                       : 0;
        } catch (const HarnessFailure& error) {
            result.failure = error.code;
        } catch (...) {
            result.failure = "unexpected_host_exception";
        }
        result.stop_ok = !host.stop();
        if (!result.stop_ok) {
            result.failure = "stop_failed";
            result.shutdown_exit = 7;
        }
        return result;
    }

  private:
    static bool fresh_flat(const CgateTradingHost& host) {
        if (!host.cancellation_snapshot_ready() || !host.log_error_.empty() || host.orders_->queued() ||
            host.has_pending_cancellations() || host.has_working_orders() || host.orders_->operator_action_required())
            return false;
        const auto& state = host.session_.private_state();
        const auto health = host.session_.runtime_health();
        if (!health.all_active() || health.private_count != 7 || !host.session_.aggr_online() ||
            !host.session_.aggr_snapshot_complete())
            return false;
        const auto streams = state.stream_health();
        for (const auto& required : host.config_.session.private_streams) {
            if (!complete(streams, required.stream_code))
                return false;
        }
        for (const auto& required : host.config_.session.status_streams) {
            if (!complete(streams, required.stream_code))
                return false;
        }
        const auto life = state.stream_lifenum(plaza2::generated::StreamCode::kFortsTradeRepl);
        const auto pos = std::find_if(streams.begin(), streams.end(), [](const auto& row) {
            return row.stream_code == plaza2::generated::StreamCode::kFortsPosRepl;
        });
        if (!life || pos == streams.end() || pos->last_trades_lifenum != static_cast<std::int64_t>(*life))
            return false;
        const auto account = host.config_.orders.broker_code + host.config_.orders.client_code;
        return std::none_of(state.positions().begin(), state.positions().end(), [&](const auto& position) {
            return position.account_code == account && position.xpos != 0;
        });
    }
    template <typename Streams, typename Code> static bool complete(const Streams& streams, Code code) {
        const auto row =
            std::find_if(streams.begin(), streams.end(), [&](const auto& s) { return s.stream_code == code; });
        return row != streams.end() && row->online && row->snapshot_complete;
    }
};
} // namespace moex::connector_host
