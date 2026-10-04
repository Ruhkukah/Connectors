#pragma once
#include "moex/connector_host/full_order_log_dtc.hpp"
#include "moex/connector_host/dtc_read_only_server.hpp"
#include <memory>

namespace moex::connector_host {
// The standalone process and offline benchmark share the same owner loop.
class FullOrderLogDtcLoop {
  public:
    FullOrderLogDtcLoop(ConnectorHost& host, plaza2::cgate::Plaza2FullOrderLog& book, std::uint16_t base_port,
                        std::uint32_t depth, dtc::DtcSourceMode mode, dtc::DtcReplayDefinitionTerms replay = {})
        : host_(host), book_(book) {
        for (const auto isin : book.instruments()) {
            sources_.push_back(
                std::make_unique<dtc::DtcFullOrderLogSource>(book, isin, dtc::DtcMarketDataSnapshot{}, depth * 2));
            dtc::DtcReadOnlyServerConfig config;
            config.port = base_port ? base_port + servers_.size() : 0;
            config.symbol_id = servers_.size() + 1;
            config.source_mode = mode;
            config.currency = replay.currency;
            config.description = replay.description;
            config.contract_size = replay.contract_size;
            config.currency_value_per_increment = replay.currency_value_per_increment;
            config.max_depth_levels = depth;
            config.max_queued_bytes = 4 * 1024 * 1024;
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
        for (auto& server : servers_) {
            std::string error;
            if (!server->start(error))
                throw std::runtime_error(error);
        }
    }
    void poll() {
        if (const auto e = host_.poll(); e)
            throw std::runtime_error(e.message);
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
    std::uint64_t polls{}, metadata_refreshes{};

  private:
    ConnectorHost& host_;
    plaza2::cgate::Plaza2FullOrderLog& book_;
    std::vector<std::unique_ptr<dtc::DtcFullOrderLogSource>> sources_;
    std::vector<std::unique_ptr<dtc::DtcReadOnlyServer>> servers_;
    std::uint64_t metadata_revision_{UINT64_MAX}, epoch_{UINT64_MAX};
    bool valid_{};
    std::chrono::steady_clock::time_point refresh_at_{};
    void refresh_metadata() {
        const auto revision = host_.market_data_metadata_revision(), epoch = book_.epoch();
        const auto now = std::chrono::steady_clock::now();
        if (revision == metadata_revision_ && epoch == epoch_ && book_.valid() == valid_ && now < refresh_at_)
            return;
        metadata_revision_ = revision;
        epoch_ = epoch;
        valid_ = book_.valid();
        refresh_at_ = now + std::chrono::seconds(1);
        for (std::size_t i = 0; i < sources_.size(); ++i)
            sources_[i]->update_metadata(
                dtc::make_dtc_market_data_snapshot(host_.market_data_snapshot(book_.instruments()[i])));
        ++metadata_refreshes;
    }
};
} // namespace moex::connector_host
