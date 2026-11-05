#pragma once

#include "moex/connector_host/dtc_read_only_server.hpp"
#include "moex/connector_host/full_order_log_dtc.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_deals.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
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
          ext_begin_(config.orders.ext_id_begin), ext_end_(config.orders.ext_id_end), secret_(read_secret(secret)) {
        auto path = config.identity_state_path;
        if (path.empty())
            path = config.journal_path.parent_path() / (config.session.publisher_name + ".state");
        ledger_ = path.string() + ".dtc";
    }
    ~FullOrderLogTradingDtc() {
        stop();
        if (ledger_fd_ >= 0)
            ::close(ledger_fd_);
    }

    void start(CgateTradingHost& host, plaza2::cgate::Plaza2FullOrderLog& book,
               const plaza2::cgate::FullOrderLogDeals* deals) {
        host_ = &host;
        open_ledger();
        source_ = std::make_unique<dtc::DtcFullOrderLogSource>(book, isin_, dtc::DtcMarketDataSnapshot{}, 40);
        if (deals)
            source_->set_public_deals_reader(
                [deals, isin = isin_](auto after, auto session) { return deals->snapshot(isin, after, session); });
        dtc::DtcTradingHooks hooks;
        hooks.account = account_;
        hooks.snapshot = [&] { return snapshot(); };
        hooks.fills = [&] { return fills(); };
        hooks.balance = [&]() -> std::optional<dtc::DtcOwnBalance> {
            const auto value = host_->dtc_balance();
            if (!value)
                return std::nullopt;
            return dtc::DtcOwnBalance{value->free_kopecks, value->blocked_kopecks, value->total_kopecks};
        };
        hooks.place = [&](std::string id, auto side, bool ioc, std::string price, std::int32_t qty, bool automated) {
            if (!ready())
                return std::string("DTC entry is fenced");
            if (keys_.contains(id))
                return std::string("duplicate ClientOrderID");
            const auto key = "dtc:" + id;
            const auto line = "place " + key;
            host_->record_operator_input(line, automated ? "dtc_automated" : "dtc");
            return report(line, host_->place({.client_order_id = key,
                                              .isin_id = isin_,
                                              .side = side,
                                              .type = ioc ? plaza2_trade::Plaza2TradeOrderType::Ioc
                                                          : plaza2_trade::Plaza2TradeOrderType::Limit,
                                              .price = std::move(price),
                                              .quantity = qty,
                                              .dtc_origin = true,
                                              .automated = automated}));
        };
        hooks.move = [&](std::string_view server_id, std::string price, std::int32_t qty) {
            if (!ready())
                return std::string("DTC entry is fenced");
            const auto* order = find(server_id);
            if (!order)
                return std::string("DTC order identity unavailable");
            if (ids_.at(order->ext_id).automated)
                return std::string("DTC cancel-replace lacks automation provenance; cancel remains available");
            const auto line = "move " + order->request.client_order_id;
            host_->record_operator_input(line, "dtc");
            return report(line, host_->move(order->request.client_order_id, std::move(price), qty, true));
        };
        hooks.cancel = [&](std::string_view server_id) {
            const auto* order = find(server_id);
            if (!order || order->order_id <= 0)
                return std::string("native DTC order identity unavailable");
            return cancel(*order);
        };
        hooks.disconnected = [&] {
            if (!host_)
                return;
            try {
                host_->fence_dtc_entry();
                if (cancel_after_disconnect_ && !disconnected_at_)
                    disconnected_at_ = Clock::now();
            } catch (...) {
                // Do not let socket teardown interrupt normal host shutdown.
                try {
                    host_->set_kill_switch(true);
                } catch (...) {
                }
            }
        };
        hooks.reconciled = [&] {
            if (disconnected_at_ && Clock::now() - *disconnected_at_ < std::chrono::seconds(10))
                disconnected_at_.reset();
        };
        server_ = std::make_unique<dtc::DtcReadOnlyServer>(
            *source_, dtc::DtcReadOnlyServerConfig{.port = port_,
                                                   .source_mode = dtc::DtcSourceMode::LiveTest,
                                                   .symbol_id = 1,
                                                   .max_queued_bytes = 1024 * 1024,
                                                   .require_local_auth = true,
                                                   .local_username = "kairos",
                                                   .local_password = std::move(secret_),
                                                   .trading = std::move(hooks)});
        std::string error;
        if (!server_->start(error))
            throw std::runtime_error(error);
    }
    void stop() noexcept {
        if (server_)
            server_->stop();
        host_ = nullptr;
    }
    void poll(std::uint64_t version, std::uint64_t epoch) {
        if (disconnected_at_ && Clock::now() - *disconnected_at_ >= std::chrono::seconds(10) &&
            host_->dtc_snapshot_ready()) {
            bool outstanding = false;
            for (const auto& [_, order] : host_->dtc_orders()) {
                if (!owns(order) || terminal(order.state))
                    continue;
                outstanding = true;
                if (!order.cancel_requested && order.order_id > 0 &&
                    (order.state == OrderState::Working || order.state == OrderState::PartFilled) &&
                    disconnect_cancel_attempts_.insert(order.ext_id).second)
                    (void)cancel(order);
            }
            if (!outstanding) {
                disconnected_at_.reset();
                disconnect_cancel_attempts_.clear();
            }
        }
        const auto now = Clock::now();
        const auto metadata = host_->dtc_private_state().metadata_revision();
        if (metadata != metadata_ || epoch != epoch_ || now >= refresh_at_) {
            source_->update_metadata(dtc::make_dtc_market_data_snapshot(host_->dtc_market_data(isin_)));
            metadata_ = metadata;
            refresh_at_ = now + std::chrono::seconds(1);
        }
        if (version != version_ || epoch != epoch_) {
            source_->committed();
            version_ = version;
            epoch_ = epoch;
        }
        server_->poll();
    }
    void reserve(std::string_view key, std::int32_t ext, bool automated) {
        const std::string id(key.substr(4));
        if (ledger_fd_ < 0 || keys_.contains(id) || ids_.contains(ext) || ids_.size() >= capacity_)
            throw std::invalid_argument("DTC identity already used or identity ledger full");
        const auto fd = ::openat(ledger_fd_, id.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            throw std::invalid_argument("DTC identity could not be reserved");
        const auto line = ledger_record(ext, automated);
        const bool synced = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()) &&
                            ::fsync(fd) == 0 && ::fsync(ledger_fd_) == 0;
        ::close(fd);
        if (!synced)
            throw std::invalid_argument("DTC identity could not be durably reserved");
        keys_.insert(id);
        ids_.emplace(ext, Identity{id, automated});
    }

  private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t capacity_ = 65536;
    static constexpr std::size_t max_report_ = 4096;
    std::uint16_t port_;
    bool cancel_after_disconnect_;
    std::int32_t isin_;
    std::string account_;
    std::int8_t account_type_;
    std::string login_;
    std::int32_t ext_begin_, ext_end_;
    std::string secret_;
    std::filesystem::path ledger_;
    int ledger_fd_{-1};
    std::set<std::string> keys_;
    struct Identity {
        std::string client_id;
        bool automated;
    };
    std::map<std::int32_t, Identity> ids_;
    CgateTradingHost* host_{};
    std::unique_ptr<dtc::DtcFullOrderLogSource> source_;
    std::unique_ptr<dtc::DtcReadOnlyServer> server_;
    std::optional<Clock::time_point> disconnected_at_;
    std::set<std::int32_t> disconnect_cancel_attempts_;
    Clock::time_point refresh_at_{};
    std::uint64_t version_{}, epoch_{}, metadata_{};

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
    std::string ledger_record(std::int32_t ext, bool automated) const {
        return std::to_string(ext) + '\n' + std::to_string(isin_) + '\n' + account_ + '\n' + login_ +
               (automated ? "\nR\n" : "\nM\n");
    }
    void open_ledger() {
        const auto parent = ledger_.parent_path().empty() ? std::filesystem::path(".") : ledger_.parent_path();
        if (::mkdir(ledger_.c_str(), 0700) != 0 && errno != EEXIST)
            throw std::runtime_error("cannot create DTC identity ledger");
        const auto parent_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const bool parent_synced = parent_fd >= 0 && ::fsync(parent_fd) == 0;
        if (parent_fd >= 0)
            ::close(parent_fd);
        ledger_fd_ = ::open(ledger_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat info {};
        if (!parent_synced || ledger_fd_ < 0 || ::fstat(ledger_fd_, &info) != 0 || info.st_uid != ::geteuid() ||
            (info.st_mode & 0777) != 0700)
            throw std::runtime_error("DTC identity ledger must be owned and mode 0700");
        for (const auto& entry : std::filesystem::directory_iterator(ledger_)) {
            const auto id = entry.path().filename().string();
            if (ids_.size() >= capacity_ || id.empty() || id.size() > 64 ||
                id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                    std::string::npos)
                throw std::runtime_error("invalid DTC identity ledger entry");
            const auto fd = ::openat(ledger_fd_, id.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            struct stat stat {};
            std::array<char, 256> bytes{};
            const bool valid = fd >= 0 && ::fstat(fd, &stat) == 0 && S_ISREG(stat.st_mode) &&
                               stat.st_uid == ::geteuid() && (stat.st_mode & 0777) == 0600;
            const auto size = valid ? ::read(fd, bytes.data(), bytes.size()) : -1;
            if (fd >= 0)
                ::close(fd);
            if (size <= 0 || size >= static_cast<ssize_t>(bytes.size()))
                throw std::runtime_error("invalid or incomplete DTC identity record");
            const std::string_view line(bytes.data(), static_cast<std::size_t>(size));
            const auto end = line.find('\n');
            const auto automated = line.ends_with("\nR\n");
            std::int32_t ext{};
            if (end == std::string_view::npos ||
                std::from_chars(line.data(), line.data() + end, ext).ec != std::errc{} || ext < ext_begin_ ||
                ext > ext_end_ || ledger_record(ext, automated) != line ||
                !ids_.emplace(ext, Identity{id, automated}).second)
                throw std::runtime_error("DTC identity ledger does not match account, login or ID range");
            keys_.insert(id);
        }
    }
    static std::string native_view_id(std::int32_t session, std::int64_t native) {
        return "native:" + std::to_string(session) + ":" + std::to_string(native);
    }
    bool owns(const ManagedOrder& order) const {
        return order.instance_owned && order.request.isin_id == isin_ && ids_.contains(order.ext_id);
    }
    const ManagedOrder* find(std::string_view server_id) const {
        for (const auto& [_, order] : host_->dtc_orders())
            if (owns(order) && server_id == std::to_string(order.ext_id))
                return &order;
        return nullptr;
    }
    bool ready() const {
        return !disconnected_at_ && host_->dtc_entry_ready(isin_);
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
            out.orders.push_back({.client_id = dtc_owned ? ids_.at(order.ext_id).client_id : std::string{},
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
                                  .automated = dtc_owned && ids_.at(order.ext_id).automated});
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
                const bool dtc_owned = login == login_ && ids_.contains(ext);
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
