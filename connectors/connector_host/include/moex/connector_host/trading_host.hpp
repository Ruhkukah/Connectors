#pragma once
#include "moex/connector_host/connector_host.hpp"
#include "moex/connector_host/event_journal.hpp"
#include "moex/connector_host/order_manager.hpp"

#include <thread>
#include <iosfwd>
#include <unordered_map>

namespace moex::connector_host {
struct TradingHostConfig {
    plaza2_trade::CgateSessionConfig session;
    OrderManagerConfig orders;
    std::vector<std::int32_t> isin_ids;
    std::filesystem::path journal_path;
    std::filesystem::path identity_state_path;
    std::optional<std::int64_t> clock_offset_us;
    std::function<std::int64_t()> utc_now;
    std::function<std::uintmax_t(const std::filesystem::path&)> storage_space_probe;
    std::string source_git_sha{"unknown"}, binary_sha256{"unknown"};
};
class CgateTradingHost {
  public:
    explicit CgateTradingHost(TradingHostConfig config);
    ~CgateTradingHost() noexcept;
    [[nodiscard]] plaza2::cgate::Plaza2Error start();
    [[nodiscard]] plaza2::cgate::Plaza2Error poll();
    [[nodiscard]] plaza2::cgate::Plaza2Error stop();
    [[nodiscard]] std::string place(OrderRequest request);
    [[nodiscard]] std::string cancel(std::string_view client_order_id);
    [[nodiscard]] std::string move(std::string_view client_order_id, std::string price, std::int32_t quantity);
    [[nodiscard]] std::string cancel_all(std::int32_t isin_id);
    void set_kill_switch(bool enabled);
    [[nodiscard]] std::string storage_ok();
    void record_operator_input(std::string_view line, std::string_view channel = "stdin");
    void record_local_refusal(std::string_view line, std::string_view error, std::string_view channel = "stdin");
    [[nodiscard]] std::string status() const;
    [[nodiscard]] bool has_pending_cancellations() const;
    [[nodiscard]] bool has_working_orders() const;
    void report_outstanding_orders(std::ostream& output) const;

  private:
    void assert_owner() const;
    void dispatch_commands();
    void storage_failure(std::string_view error, bool writer_failed = true);
    [[nodiscard]] std::string check_storage_space();
    plaza2_trade::CgateSessionConfig session_config();
    void log_event(std::string_view kind, std::string_view fields) noexcept;
    void log_listener_event(const plaza2::cgate::Plaza2ListenerEvent& event) noexcept;
    void observe_link(const plaza2_trade::Plaza2TransportHealth& health);
    void observe_exchange_messages();
    TradingHostConfig config_;
    std::thread::id owner_;
    EventJournal journal_;
    plaza2_trade::CgateSession session_;
    std::unique_ptr<OrderManager> orders_;
    bool rebuilding_{true};
    bool reconcile_reload_{};
    bool stopped_{};
    bool link_was_active_{}, link_lost_{};
    plaza2::cgate::Plaza2Error stop_error_;
    std::string log_error_;
    std::string identifier_reservation_;
    bool journal_failed_{};
    OrderManager::Clock::time_point next_space_check_{};
    std::uint64_t exchange_message_commit_{};
    std::unordered_map<std::int64_t, std::pair<std::uint64_t, std::int64_t>> exchange_message_revisions_;
    std::vector<plaza2::private_state::SystemMessageSnapshot> exchange_messages_;
};
} // namespace moex::connector_host
