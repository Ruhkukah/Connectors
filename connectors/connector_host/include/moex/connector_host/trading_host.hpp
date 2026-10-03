#pragma once
#include "moex/connector_host/connector_host.hpp"
#include "moex/connector_host/event_journal.hpp"
#include "moex/connector_host/order_manager.hpp"

#include <thread>
#include <iosfwd>

namespace moex::connector_host {
struct TradingHostConfig {
    plaza2_trade::CgateSessionConfig session;
    OrderManagerConfig orders;
    std::vector<std::int32_t> isin_ids;
    std::filesystem::path journal_path;
    std::filesystem::path identity_state_path;
    std::optional<std::int64_t> clock_offset_us;
    std::function<std::int64_t()> utc_now;
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
    void record_operator_input(std::string_view line, std::string_view channel = "stdin");
    void record_local_refusal(std::string_view line, std::string_view error, std::string_view channel = "stdin");
    [[nodiscard]] std::string status() const;
    void report_outstanding_orders(std::ostream& output) const;

  private:
    void assert_owner() const;
    void dispatch_commands();
    plaza2_trade::CgateSessionConfig session_config();
    void log_event(std::string_view kind, std::string_view fields) noexcept;
    void log_listener_event(const plaza2::cgate::Plaza2ListenerEvent& event) noexcept;
    void observe_link(const plaza2_trade::Plaza2TransportHealth& health);
    TradingHostConfig config_;
    std::thread::id owner_;
    EventJournal journal_;
    plaza2_trade::CgateSession session_;
    std::unique_ptr<OrderManager> orders_;
    bool rebuilding_{true};
    bool stopped_{};
    bool link_was_active_{}, link_lost_{};
    plaza2::cgate::Plaza2Error stop_error_;
    std::string log_error_;
};
} // namespace moex::connector_host
