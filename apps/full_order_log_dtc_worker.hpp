#pragma once

#include "moex/connector_host/dtc_read_only_server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <future>
#include <thread>

namespace moex::connector_host {

// Single-producer/single-consumer ring. The producer never waits for socket I/O.
template <class T, std::size_t Slots> class FolDtcChannel {
  public:
    bool push(T value) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) % Slots;
        if (next == read_.load(std::memory_order_acquire))
            return false;
        slots_[write] = std::move(value);
        write_.store(next, std::memory_order_release);
        return true;
    }
    std::optional<T> pop() {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire))
            return std::nullopt;
        T value = std::move(slots_[read]);
        read_.store((read + 1) % Slots, std::memory_order_release);
        return value;
    }

  private:
    std::array<T, Slots> slots_{};
    std::atomic_size_t read_{}, write_{};
};

class FullOrderLogDtcWorker {
  public:
    using Clock = std::chrono::steady_clock;
    struct Market {
        dtc::DtcMarketDataSnapshot snapshot;
        std::vector<dtc::DtcMarketDataLevel> changes;
        plaza2::cgate::Plaza2PublicDealsSnapshot deals;
        std::uint64_t generation{};
    };
    struct Private {
        dtc::DtcTradingSnapshot snapshot;
        std::shared_ptr<const std::vector<dtc::DtcOwnFill>> fills;
        std::optional<dtc::DtcOwnBalance> balance;
    };
    struct Command {
        dtc::DtcTradeCommand value{};
        std::uint64_t connection{};
    };

    ~FullOrderLogDtcWorker() {
        request_stop();
        join();
        delete private_.exchange(nullptr);
    }
    void start(std::uint16_t port, std::string secret, std::string account, bool deals) {
        std::promise<std::string> startup;
        auto result = startup.get_future();
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this, port, secret = std::move(secret), account = std::move(account), deals,
                               startup = std::move(startup)]() mutable {
            run(port, std::move(secret), std::move(account), deals, std::move(startup));
        });
        if (const auto error = result.get(); !error.empty())
            throw std::runtime_error(error);
    }
    void request_stop() noexcept {
        running_.store(false, std::memory_order_release);
    }
    void join() noexcept {
        if (thread_.joinable())
            thread_.join();
    }
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }
    bool publish(Market market) {
        market.generation = market_generation_.load(std::memory_order_relaxed);
        if (market.deals.trades.size() <= 2048 && market_.push(std::move(market)))
            return true;
        market_generation_.fetch_add(1, std::memory_order_release);
        return false;
    }
    void publish(std::unique_ptr<const Private> state) {
        delete private_.exchange(state.release(), std::memory_order_acq_rel);
    }
    [[nodiscard]] std::optional<Command> take_command() {
        return commands_.pop();
    }
    void complete(std::string result) {
        // One command is allowed in flight until the consumer reads its reply.
        (void)results_.push(std::move(result));
    }
    [[nodiscard]] std::uint64_t connection() const noexcept {
        return connection_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t reconciled() const noexcept {
        return reconciled_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t reconciled_generation() const noexcept {
        return reconciled_generation_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] Clock::time_point reconciled_at() const noexcept {
        return Clock::time_point{Clock::duration{reconciled_at_.load(std::memory_order_relaxed)}};
    }
    [[nodiscard]] Clock::time_point disconnected_at() const noexcept {
        return Clock::time_point{Clock::duration{disconnected_at_.load(std::memory_order_acquire)}};
    }

  private:
    class Source final : public dtc::DtcMarketDataSource {
      public:
        explicit Source(bool deals) : deals_enabled_(deals) {}
        void clear() {
            snapshot_ = {};
            changes_.clear();
            deals_ = {};
            trades_.clear();
        }
        void update(Market market) {
            snapshot_ = std::move(market.snapshot);
            changes_ = std::move(market.changes);
            std::erase_if(changes_, [&](const auto& row) { return row.depth_level > depth_; });
            if (deals_.stream_epoch != market.deals.stream_epoch || !market.deals.valid)
                trades_.clear();
            for (auto& row : market.deals.trades) {
                if (trades_.empty() || row.sequence > trades_.back().sequence)
                    trades_.push_back(std::move(row));
                if (trades_.size() > 8192)
                    trades_.pop_front();
            }
            market.deals.trades.clear();
            deals_ = std::move(market.deals);
            if (!trades_.empty())
                deals_.first_sequence = trades_.front().sequence;
        }
        [[nodiscard]] dtc::DtcMarketDataSnapshot snapshot() const override {
            return snapshot_;
        }
        [[nodiscard]] dtc::DtcMarketDataSnapshot status_snapshot() const override {
            auto status = snapshot_;
            status.levels.clear();
            return status;
        }
        [[nodiscard]] dtc::DtcReadOnlyCapabilities capabilities() const noexcept override {
            return {.market_data = deals_enabled_, .market_depth = true, .security_definitions = true};
        }
        [[nodiscard]] bool incremental_depth() const noexcept override {
            return true;
        }
        [[nodiscard]] std::span<const dtc::DtcMarketDataLevel> depth_changes() const noexcept override {
            return changes_;
        }
        void configure_depth_limit(std::size_t depth) override {
            depth_ = depth;
        }
        [[nodiscard]] plaza2::cgate::Plaza2PublicDealsSnapshot public_deals(std::uint64_t after) const override {
            auto result = deals_;
            if (after < result.last_sequence)
                for (const auto& row : trades_)
                    if (row.sequence > after)
                        result.trades.push_back(row);
            return result;
        }

      private:
        bool deals_enabled_;
        std::size_t depth_{20};
        dtc::DtcMarketDataSnapshot snapshot_;
        std::vector<dtc::DtcMarketDataLevel> changes_;
        plaza2::cgate::Plaza2PublicDealsSnapshot deals_;
        std::deque<plaza2::cgate::Plaza2PublicDeal> trades_;
    };

    FolDtcChannel<Market, 129> market_;
    FolDtcChannel<Command, 2> commands_;
    FolDtcChannel<std::string, 2> results_;
    std::atomic<const Private*> private_{};
    std::atomic_bool running_{};
    std::atomic_uint64_t market_generation_{}, connection_{1}, reconciled_{}, reconciled_generation_{};
    std::atomic<Clock::duration::rep> disconnected_at_{}, reconciled_at_{};
    std::thread thread_;

    void run(std::uint16_t port, std::string secret, std::string account, bool deals,
             std::promise<std::string> startup) noexcept {
        bool started = false;
        try {
            Source source(deals);
            bool command_in_flight = false;
            std::unique_ptr<const Private> state = std::make_unique<Private>();
            const auto refresh_private = [&] {
                if (auto* pending = private_.exchange(nullptr, std::memory_order_acq_rel))
                    state.reset(pending);
            };
            dtc::DtcTradingHooks hooks;
            hooks.account = std::move(account);
            hooks.snapshot = [&] { return state->snapshot; };
            hooks.fills = [&]() -> std::optional<std::vector<dtc::DtcOwnFill>> {
                if (!state->fills)
                    return std::nullopt;
                return *state->fills;
            };
            hooks.balance = [&] { return state->balance; };
            hooks.client_lost = [&] {
                disconnected_at_.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
            };
            hooks.disconnected = [&] { connection_.fetch_add(1, std::memory_order_release); };
            hooks.reconciled = [&] {
                reconciled_generation_.store(state->snapshot.generation, std::memory_order_relaxed);
                reconciled_at_.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
                reconciled_.store(connection(), std::memory_order_release);
            };
            hooks.submit = [&](dtc::DtcTradeCommand command) {
                if (command_in_flight || !running_.load(std::memory_order_acquire))
                    return false;
                command_in_flight = commands_.push({std::move(command), connection()});
                return command_in_flight;
            };
            hooks.take_result = [&] {
                auto result = results_.pop();
                if (result) {
                    refresh_private();
                    command_in_flight = false;
                }
                return result;
            };
            dtc::DtcReadOnlyServer server(source, {.port = port,
                                                   .source_mode = dtc::DtcSourceMode::LiveTest,
                                                   .symbol_id = 1,
                                                   .max_queued_bytes = 1024 * 1024,
                                                   .require_local_auth = true,
                                                   .local_username = "kairos",
                                                   .local_password = std::move(secret),
                                                   .trading = std::move(hooks)});
            std::string error;
            if (!server.start(error)) {
                startup.set_value(error);
                return;
            }
            startup.set_value({});
            started = true;
            std::uint64_t generation{};
            while (running_.load(std::memory_order_acquire)) {
                const auto newest = market_generation_.load(std::memory_order_acquire);
                if (generation != newest) {
                    server.disconnect_client();
                    source.clear();
                    generation = newest;
                }
                for (std::size_t count = 0; count < 32; ++count) {
                    auto market = market_.pop();
                    if (!market)
                        break;
                    if (market->generation != generation)
                        continue;
                    source.update(std::move(*market));
                    server.publish_depth_commit();
                }
                refresh_private();
                server.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            server.stop();
        } catch (...) {
            if (!started) {
                try {
                    startup.set_value("FOL DTC worker startup failed");
                } catch (...) {
                }
            }
        }
        running_.store(false, std::memory_order_release);
    }
};

} // namespace moex::connector_host
