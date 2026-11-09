#pragma once
#include "moex/connector_host/full_order_log_dtc.hpp"
#include "moex/connector_host/dtc_read_only_server.hpp"
#include <algorithm>
#include <iomanip>
#include <memory>
#include <ostream>
#include <tuple>

namespace moex::connector_host {
// Counts all native callback types serviced during measured owner polls. Storage is fixed;
// process_ns is cumulative cg_conn_process wall time in ONE poll, not one native call.
struct FullOrderLogDtcMetrics {
    static constexpr auto report_interval = std::chrono::seconds(60);
    std::uint64_t callbacks{}, full_order_log_attributed_callbacks{}, unattributed_callbacks{}, process_calls{};
    std::uint64_t cg_conn_process_loop_max_ns{};
    void observe(const plaza2_trade::CgatePollMetrics& sample) noexcept {
        if (!sample.sequence || sample.sequence == sequence_)
            return;
        sequence_ = sample.sequence;
        callbacks += sample.unattributed_callbacks;
        unattributed_callbacks += sample.unattributed_callbacks;
        for (const auto& callback : sample.callbacks) {
            callbacks += callback.count;
            if (callback.stream_code == plaza2::cgate::kFullOrderLogStreamCode)
                full_order_log_attributed_callbacks += callback.count;
        }
        process_calls += sample.process_calls;
        cg_conn_process_loop_max_ns = std::max(cg_conn_process_loop_max_ns, sample.process_ns);
    }
    void reset_interval() noexcept {
        callbacks = full_order_log_attributed_callbacks = unattributed_callbacks = process_calls = 0;
        cg_conn_process_loop_max_ns = 0;
    }

  private:
    std::uint64_t sequence_{};
};
inline void log_full_order_log_listener_event(std::ostream& out, const plaza2::cgate::Plaza2ListenerEvent& event) {
    out << "FullOrderLog listener event " << static_cast<unsigned>(event.kind) << " life=" << event.unsigned_value
        << " revision=" << event.signed_value << " table=" << (event.raw_table ? event.raw_table->name : "unspecified");
    if (event.kind == plaza2::cgate::Plaza2ListenerEventKind::Close) {
        const auto flags = out.flags();
        out << " close_reason=" << std::dec << event.close_reason << " close_reason_hex=0x" << std::uppercase
            << std::hex << event.close_reason;
        out.flags(flags);
        if (event.close_reason == 0xA006)
            out << " close_reason_name=REPL:CHANNEL_DISCONNECT_ON_TIMEOUT";
    }
    out << '\n';
}
// The standalone process and offline benchmark share the same owner loop.
class FullOrderLogDtcLoop {
  public:
    FullOrderLogDtcLoop(ConnectorHost& host, plaza2::cgate::Plaza2FullOrderLog& book,
                        dtc::DtcReadOnlyServerConfig config)
        : host_(host), book_(book) {
        const auto base_port = config.port;
        for (const auto isin : book.instruments()) {
            sources_.push_back(std::make_unique<dtc::DtcFullOrderLogSource>(book, isin, dtc::DtcMarketDataSnapshot{},
                                                                            config.max_depth_levels * 2));
            config.port = base_port ? base_port + servers_.size() : 0;
            config.symbol_id = servers_.size() + 1;
            servers_.push_back(std::make_unique<dtc::DtcReadOnlyServer>(*sources_.back(), config));
        }
        book_.on_commit = [&](const auto&) {
            refresh_metadata();
            for (std::size_t i = 0; i < sources_.size(); ++i) {
                sources_[i]->committed();
                servers_[i]->publish_depth_commit();
            }
            if (on_queued_commit)
                on_queued_commit();
            for (auto& server : servers_)
                server->poll();
        };
        book_.on_invalidate = [&] {
            refresh_metadata();
            for (auto& server : servers_)
                server->publish_depth_commit();
        };
    }
    ~FullOrderLogDtcLoop() {
        book_.on_commit = {};
        book_.on_invalidate = {};
    }
    void start() {
        if (const auto e = host_.start(); e)
            throw std::runtime_error(e.message);
        std::string error;
        for (auto& server : servers_)
            if (!server->start(error))
                throw std::runtime_error(error);
    }
    void poll() {
        const auto error = host_.poll();
        metrics.observe(host_.last_poll_metrics());
        if (error)
            throw std::runtime_error(error.message);
        ++polls;
        refresh_metadata();
        for (auto& server : servers_)
            server->poll();
    }
    void stop() {
        for (auto& server : servers_)
            server->stop();
        if (const auto e = host_.stop(); e)
            throw std::runtime_error(e.message);
    }
    dtc::DtcReadOnlyServer& server(std::size_t i = 0) {
        return *servers_.at(i);
    }
    dtc::DtcFullOrderLogSource& source(std::size_t i = 0) {
        return *sources_.at(i);
    }
    std::function<void()> on_queued_commit;
    std::function<void(const ConnectorHostMarketDataSnapshot&)> on_metadata;
    std::uint64_t polls{}, metadata_refreshes{};
    FullOrderLogDtcMetrics metrics;

  private:
    ConnectorHost& host_;
    plaza2::cgate::Plaza2FullOrderLog& book_;
    std::vector<std::unique_ptr<dtc::DtcFullOrderLogSource>> sources_;
    std::vector<std::unique_ptr<dtc::DtcReadOnlyServer>> servers_;
    std::tuple<std::uint64_t, std::uint64_t, bool> metadata_key_{UINT64_MAX, UINT64_MAX, false};
    std::chrono::steady_clock::time_point refresh_at_{};
    void refresh_metadata() {
        const auto key = std::tuple{host_.market_data_metadata_revision(), book_.epoch(), book_.valid()};
        const auto now = std::chrono::steady_clock::now();
        if (key == metadata_key_ && now < refresh_at_)
            return;
        metadata_key_ = key;
        refresh_at_ = now + std::chrono::seconds(1);
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            const auto snapshot = host_.market_data_snapshot(book_.instruments()[i]);
            if (on_metadata)
                on_metadata(snapshot);
            sources_[i]->update_metadata(dtc::make_dtc_market_data_snapshot(snapshot));
        }
        ++metadata_refreshes;
    }
};
} // namespace moex::connector_host
