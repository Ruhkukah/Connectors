#pragma once

#include "full_order_log_dtc_identity.hpp"
#include "full_order_log_dtc_worker.hpp"
#include "moex/connector_host/full_order_log_dtc.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_deals.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace moex::connector_host {

class FullOrderLogTradingDtc {
  public:
    FullOrderLogTradingDtc(const TradingHostConfig& config, std::uint16_t port, const std::filesystem::path& secret,
                           bool cancel_after_disconnect)
        : port_(port), cancel_after_disconnect_(cancel_after_disconnect), isin_(config.isin_ids.front()),
          account_(config.orders.broker_code + config.orders.client_code),
          account_type_(config.orders.client_code.empty() ? 1 : 2), login_(config.orders.login_from),
          secret_(read_secret(secret)), identity_(config) {}
    ~FullOrderLogTradingDtc() {
        stop();
        join();
    }

    void start(CgateTradingHost& host, plaza2::cgate::Plaza2FullOrderLog& book,
               const plaza2::cgate::FullOrderLogDeals* deals) {
        host_ = &host;
        identity_.start();
        source_ = std::make_unique<dtc::DtcFullOrderLogSource>(book, isin_, dtc::DtcMarketDataSnapshot{}, 40);
        source_->configure_depth_limit(20);
        if (deals)
            source_->set_public_deals_reader(
                [deals, isin = isin_](auto after, auto session) { return deals->snapshot(isin, after, session); });
        worker_.start(port_, std::move(secret_), account_, deals != nullptr);
    }
    void stop() noexcept {
        worker_.request_stop();
        identity_.request_stop();
        if (host_)
            try {
                host_->fence_dtc_entry();
            } catch (...) {
            }
        host_ = nullptr;
    }
    void join() noexcept {
        worker_.join();
        identity_.join();
    }
    void poll(std::uint64_t version, std::uint64_t epoch) {
        try {
            poll_owner(version, epoch);
        } catch (...) {
            failed_ = true;
            worker_.request_stop();
            identity_.request_stop();
            try {
                host_->fence_dtc_entry();
            } catch (...) {
            }
            try {
                host_->set_kill_switch(true);
            } catch (...) {
            }
            std::cerr << "FOL DTC owner failure; entering normal SIGTERM cancel drain\n";
            if (::kill(::getpid(), SIGTERM) != 0)
                throw;
        }
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }
    [[nodiscard]] bool entry_connection_current() const noexcept {
        return host_ && !failed_ && worker_.running() && !disconnected_at_ && worker_.connection() == connection_ &&
               worker_.reconciled() == connection_ &&
               worker_.reconciled_generation() == host_->dtc_snapshot_generation();
    }

  private:
    using Clock = FullOrderLogDtcWorker::Clock;
    using Command = dtc::DtcTradeCommand;
    static constexpr std::size_t max_report_ = 4096;
    struct PendingAdd {
        std::int32_t ext_id;
        std::uint64_t connection, generation, epoch;
        std::string line;
    };

    std::uint16_t port_;
    bool cancel_after_disconnect_;
    std::int32_t isin_;
    std::string account_;
    std::int8_t account_type_;
    std::string login_, secret_;
    FullOrderLogDtcIdentity identity_;
    FullOrderLogDtcWorker worker_;
    CgateTradingHost* host_{};
    std::unique_ptr<dtc::DtcFullOrderLogSource> source_;
    std::optional<PendingAdd> pending_add_;
    std::optional<Clock::time_point> disconnected_at_;
    std::map<std::int32_t, Clock::time_point> disconnect_cancel_attempts_;
    std::shared_ptr<const std::vector<dtc::DtcOwnFill>> cached_fills_;
    Clock::time_point refresh_at_{}, private_at_{}, observed_loss_{};
    std::uint64_t version_{}, epoch_{}, metadata_{}, connection_{1};
    plaza2::cgate::Plaza2PublicDealsSnapshot deals_header_;
    bool retry_market_{true}, history_current_{}, failed_{};
    std::tuple<std::size_t, std::int32_t, std::int64_t, std::int64_t> history_key_{};

    void poll_owner(std::uint64_t version, std::uint64_t epoch) {
        if (failed_)
            return;
        if (!worker_.running())
            throw std::runtime_error("FOL DTC worker stopped");
        const auto now = Clock::now();
        const auto connection = worker_.connection();
        const auto loss = worker_.disconnected_at();
        if (connection != connection_ ||
            (pending_add_ &&
             (pending_add_->epoch != epoch || pending_add_->generation != host_->dtc_snapshot_generation()))) {
            host_->fence_dtc_entry();
            connection_ = connection;
        }
        if (loss != observed_loss_) {
            observed_loss_ = loss;
            if (cancel_after_disconnect_ && !disconnected_at_)
                disconnected_at_ = loss;
        }
        if (disconnected_at_ && worker_.reconciled() == connection_) {
            const auto reconciled = worker_.reconciled_at();
            if (reconciled >= *disconnected_at_ && reconciled - *disconnected_at_ < std::chrono::seconds(10)) {
                disconnected_at_.reset();
                disconnect_cancel_attempts_.clear();
            }
        }
        sweep(now);
        if (auto ack = identity_.take()) {
            if (!pending_add_ || pending_add_->ext_id != ack->ext_id)
                throw std::logic_error("DTC identity reply has no matching owner command");
            auto pending = std::exchange(pending_add_, std::nullopt);
            if (pending->connection != worker_.connection() || pending->epoch != epoch ||
                pending->generation != host_->dtc_snapshot_generation() || !ready()) {
                host_->fence_dtc_entry();
                complete(report(pending->line, "DTC authority changed before Add was posted"), now);
            } else {
                complete(report(pending->line, host_->finish_dtc_add(ack->ext_id, ack->persisted)), now);
            }
        }
        if (auto pending = worker_.take_command())
            apply(std::move(*pending), epoch, now);
        if (now >= private_at_)
            publish_private(now);
        publish_market(version, epoch, now);
    }
    void apply(FullOrderLogDtcWorker::Command pending, std::uint64_t epoch, Clock::time_point now) {
        auto& command = pending.value;
        if (pending.connection != worker_.connection()) {
            complete("DTC session closed before dispatch", now);
            return;
        }
        if (command.kind != Command::Kind::Cancel &&
            (!ready() || command.source_epoch != epoch ||
             command.snapshot_generation != host_->dtc_snapshot_generation())) {
            complete("DTC entry is fenced", now);
            return;
        }
        if (command.kind == Command::Kind::Add) {
            if (identity_.contains(command.client_id)) {
                complete("duplicate ClientOrderID", now);
                return;
            }
            const auto key = "dtc:" + command.client_id;
            const auto line = "place " + key;
            host_->record_operator_input(line, command.automated ? "dtc_automated" : "dtc");
            auto prepared = host_->prepare_dtc_add({.client_order_id = key,
                                                    .isin_id = isin_,
                                                    .side = command.side,
                                                    .type = command.ioc ? plaza2_trade::Plaza2TradeOrderType::Ioc
                                                                        : plaza2_trade::Plaza2TradeOrderType::Limit,
                                                    .price = std::move(command.price),
                                                    .quantity = command.quantity,
                                                    .dtc_origin = true,
                                                    .automated = command.automated});
            if (!prepared.error.empty()) {
                complete(report(line, std::move(prepared.error)), now);
                return;
            }
            if (auto error = identity_.submit(key, prepared.ext_id, command.automated); !error.empty()) {
                (void)host_->finish_dtc_add(prepared.ext_id, false);
                complete(report(line, std::move(error)), now);
                return;
            }
            pending_add_ = PendingAdd{prepared.ext_id, pending.connection, command.snapshot_generation, epoch, line};
            return;
        }
        const auto* order = find(command.server_id, command.client_id);
        if (!order || order->order_id <= 0) {
            complete("native DTC order identity unavailable", now);
            return;
        }
        if (command.kind == Command::Kind::Cancel) {
            complete(cancel(*order), now);
            return;
        }
        if (identity_.by_ext(order->ext_id)->automated) {
            complete("DTC cancel-replace lacks automation provenance; cancel remains available", now);
            return;
        }
        const auto line = "move " + order->request.client_order_id;
        host_->record_operator_input(line, "dtc");
        complete(
            report(line, host_->move(order->request.client_order_id, std::move(command.price), command.quantity, true)),
            now);
    }
    void complete(std::string result, Clock::time_point now) {
        publish_private(now);
        worker_.complete(std::move(result));
    }
    void sweep(Clock::time_point now) {
        if (!disconnected_at_ || now - *disconnected_at_ < std::chrono::seconds(10) || !host_->dtc_snapshot_ready())
            return;
        bool outstanding = false;
        for (const auto& [_, order] : host_->dtc_orders()) {
            if (!owns(order) || terminal(order.state))
                continue;
            outstanding = true;
            if (order.cancel_requested || order.order_id <= 0 ||
                (order.state != OrderState::Working && order.state != OrderState::PartFilled))
                continue;
            auto& retry = disconnect_cancel_attempts_[order.ext_id];
            if (now < retry)
                continue;
            retry = now + std::chrono::seconds(1);
            if (auto error = cancel(order); !error.empty()) {
                host_->record_local_refusal("DTC disconnect cancel needs operator attention", error, "dtc");
                host_->set_dtc_cancel_alert(std::move(error));
            }
        }
        if (!outstanding) {
            disconnected_at_.reset();
            disconnect_cancel_attempts_.clear();
            host_->set_dtc_cancel_alert({});
        }
    }
    void publish_private(Clock::time_point now) {
        auto state = std::make_unique<FullOrderLogDtcWorker::Private>();
        state->snapshot = snapshot();
        const auto trades = host_->dtc_private_state().own_trades();
        const auto key = trades.empty() ? decltype(history_key_){}
                                        : decltype(history_key_){trades.size(), trades.back().sess_id,
                                                                 trades.back().id_deal, trades.back().repl_rev};
        if (state->snapshot.fills_current != history_current_ || key != history_key_) {
            auto rows = fills();
            cached_fills_ = rows ? std::make_shared<const std::vector<dtc::DtcOwnFill>>(std::move(*rows)) : nullptr;
            history_current_ = state->snapshot.fills_current;
            history_key_ = key;
        }
        state->fills = cached_fills_;
        if (const auto balance = host_->dtc_balance())
            state->balance =
                dtc::DtcOwnBalance{balance->free_kopecks, balance->blocked_kopecks, balance->total_kopecks};
        worker_.publish(std::move(state));
        private_at_ = now + std::chrono::milliseconds(25);
    }
    void publish_market(std::uint64_t version, std::uint64_t epoch, Clock::time_point now) {
        const auto metadata = host_->dtc_private_state().metadata_revision();
        const bool metadata_refresh = metadata != metadata_ || epoch != epoch_ || now >= refresh_at_;
        if (metadata_refresh) {
            source_->update_metadata(dtc::make_dtc_market_data_snapshot(host_->dtc_market_data(isin_)));
            metadata_ = metadata;
            refresh_at_ = now + std::chrono::seconds(1);
        }
        const auto header = source_->public_deals(UINT64_MAX);
        const bool deals_changed = header.stream_epoch != deals_header_.stream_epoch ||
                                   header.last_sequence != deals_header_.last_sequence ||
                                   header.online != deals_header_.online || header.valid != deals_header_.valid ||
                                   header.lifenum != deals_header_.lifenum;
        const bool book_changed = version != version_ || epoch != epoch_;
        if (!book_changed && !metadata_refresh && !deals_changed && !retry_market_)
            return;
        auto deals = deals_changed && header.valid
                         ? source_->public_deals(
                               header.stream_epoch == deals_header_.stream_epoch ? deals_header_.last_sequence : 0)
                         : header;
        FullOrderLogDtcWorker::Market frame;
        if (book_changed) {
            source_->committed();
            const auto changes = source_->depth_changes();
            frame.changes.assign(changes.begin(), changes.end());
        }
        frame.snapshot = source_->snapshot();
        const bool too_many_deals = deals.trades.size() > 2048;
        frame.deals = std::move(deals);
        version_ = version;
        epoch_ = epoch;
        retry_market_ = !worker_.publish(std::move(frame));
        if (!retry_market_ || too_many_deals)
            deals_header_ = header;
    }
    static std::string read_secret(const std::filesystem::path& path) {
        const auto fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat info {};
        if (fd < 0 || ::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & 0777) != 0600 ||
            info.st_uid != ::geteuid()) {
            if (fd >= 0)
                ::close(fd);
            throw std::invalid_argument("DTC secret requires an owned regular mode-0600 file");
        }
        std::array<char, 258> buffer{};
        const auto count = ::read(fd, buffer.data(), buffer.size());
        ::close(fd);
        if (count <= 0 || count >= static_cast<ssize_t>(buffer.size()))
            throw std::invalid_argument("invalid DTC secret length");
        std::string result(buffer.data(), static_cast<std::size_t>(count));
        if (result.ends_with('\n'))
            result.pop_back();
        if (result.empty() || result.size() > 256 || result.find_first_of("\r\n") != std::string::npos ||
            result.find('\0') != std::string::npos)
            throw std::invalid_argument("invalid DTC secret");
        return result;
    }
    static std::string native_view_id(std::int32_t session, std::int64_t native) {
        return "native:" + std::to_string(session) + ":" + std::to_string(native);
    }
    bool owns(const ManagedOrder& order) const {
        return order.instance_owned && order.request.isin_id == isin_ && identity_.by_ext(order.ext_id);
    }
    const ManagedOrder* find(std::string_view server_id, std::string_view client_id) const {
        for (const auto& [_, order] : host_->dtc_orders())
            if (owns(order) && server_id == std::to_string(order.ext_id) &&
                client_id == identity_.by_ext(order.ext_id)->client_id)
                return &order;
        return nullptr;
    }
    bool ready() const {
        return entry_connection_current() && host_->dtc_entry_ready(isin_);
    }
    std::string report(std::string_view line, std::string error) {
        if (!error.empty())
            host_->record_local_refusal(line, error, "dtc");
        return error;
    }
    std::string cancel(const ManagedOrder& order) {
        const auto key = order.request.client_order_id;
        const auto line = "cancel " + key;
        host_->record_operator_input(line, "dtc");
        return report(line, host_->cancel(key));
    }
    dtc::DtcTradingSnapshot snapshot() const {
        dtc::DtcTradingSnapshot out;
        out.orders_current = host_->dtc_snapshot_ready();
        out.fills_current = host_->dtc_fill_history_current();
        out.generation = host_->dtc_snapshot_generation();
        if (out.orders_current) {
            const auto* row = host_->dtc_private_state().find_position(account_, isin_, account_type_);
            const auto qty = row ? row->xpos : 0;
            if (qty >= -9007199254740991LL && qty <= 9007199254740991LL)
                out.position = qty;
        }
        for (const auto& [key, order] : host_->dtc_orders()) {
            const bool dtc_owned = owns(order);
            if (order.request.isin_id != isin_ || (!dtc_owned && terminal(order.state)))
                continue;
            if (out.orders.size() == max_report_ || order.remaining > 9007199254740991LL ||
                order.executed > 9007199254740991LL) {
                out.orders_current = false;
                break;
            }
            out.orders.push_back({.client_id = dtc_owned ? identity_.by_ext(order.ext_id)->client_id : std::string{},
                                  .server_id = dtc_owned            ? std::to_string(order.ext_id)
                                               : order.order_id > 0 ? native_view_id(order.sess_id, order.order_id)
                                                                    : "view:" + key,
                                  .native_id = order.order_id > 0 ? std::to_string(order.order_id) : std::string{},
                                  .price = order.request.price,
                                  .error = order.last_error,
                                  .state = order.state,
                                  .side = order.request.side,
                                  .ioc = order.request.type == plaza2_trade::Plaza2TradeOrderType::Ioc,
                                  .quantity = order.request.quantity,
                                  .filled = order.executed,
                                  .remaining = order.remaining,
                                  .fill_baseline_known = order.execution_baseline_known,
                                  .dtc_owned = dtc_owned,
                                  .automated = dtc_owned && identity_.by_ext(order.ext_id)->automated});
        }
        return out;
    }
    std::optional<std::vector<dtc::DtcOwnFill>> fills() const {
        if (!host_->dtc_fill_history_current())
            return std::nullopt;
        std::vector<dtc::DtcOwnFill> rows;
        bool incomplete = false;
        for (const auto& fill : host_->dtc_private_state().own_trades()) {
            if (fill.multileg || fill.isin_id != isin_ || fill.id_deal <= 0)
                continue;
            const auto append = [&](std::int32_t ext, std::int64_t native, std::string_view account,
                                    std::string_view login, plaza2_trade::Plaza2TradeSide side) {
                if (account != account_)
                    return;
                if (fill.amount <= 0 || fill.amount > 9007199254740991LL || !fill.moment_ns ||
                    !plaza2::private_state::parse_session_decimal(fill.price)) {
                    incomplete = true;
                    return;
                }
                const bool dtc_owned = login == login_ && identity_.by_ext(ext);
                if (!dtc_owned && native <= 0) {
                    incomplete = true;
                    return;
                }
                rows.push_back({.server_id = dtc_owned ? std::to_string(ext) : native_view_id(fill.sess_id, native),
                                .native_id = native > 0 ? std::to_string(native) : std::string{},
                                .execution_id = std::to_string(fill.sess_id) + ":" + std::to_string(fill.id_deal) +
                                                ":" + std::to_string(static_cast<unsigned>(side)),
                                .price = fill.price,
                                .timestamp_seconds = static_cast<std::int64_t>(fill.moment_ns / 1000000000ULL),
                                .quantity = fill.amount,
                                .exchange_time_ns = fill.moment_ns,
                                .side = side});
            };
            append(fill.ext_id_buy, fill.private_order_id_buy, fill.code_buy, fill.login_buy,
                   plaza2_trade::Plaza2TradeSide::Buy);
            append(fill.ext_id_sell, fill.private_order_id_sell, fill.code_sell, fill.login_sell,
                   plaza2_trade::Plaza2TradeSide::Sell);
            if (incomplete || rows.size() > max_report_)
                return std::nullopt;
        }
        return rows;
    }
};

} // namespace moex::connector_host
