#pragma once

#include "moex/connector_host/order_manager.hpp"

namespace moex::connector_host::dtc {

struct DtcOwnOrder {
    std::string client_id, server_id, native_id, price, error;
    OrderState state{};
    plaza2_trade::Plaza2TradeSide side{};
    bool ioc{};
    std::int64_t quantity{}, filled{}, remaining{};
    bool fill_baseline_known{}, dtc_owned{}, automated{};
    friend bool operator==(const DtcOwnOrder&, const DtcOwnOrder&) = default;
};

struct DtcOwnFill {
    std::string server_id, native_id, execution_id, price;
    std::int64_t timestamp_seconds{}, quantity{};
    plaza2_trade::Plaza2TradeSide side{};
};

struct DtcTradingSnapshot {
    std::vector<DtcOwnOrder> orders;
    std::optional<std::int64_t> position;
    bool orders_current{}, fills_current{};
    std::uint64_t generation{};
};

struct DtcOwnBalance {
    std::int64_t free_kopecks{}, blocked_kopecks{}, total_kopecks{};
    friend bool operator==(const DtcOwnBalance&, const DtcOwnBalance&) = default;
};

struct DtcTradingHooks {
    std::string account;
    std::function<DtcTradingSnapshot()> snapshot;
    std::function<std::optional<std::vector<DtcOwnFill>>()> fills;
    std::function<std::optional<DtcOwnBalance>()> balance;
    std::function<std::string(std::string, plaza2_trade::Plaza2TradeSide, bool, std::string, std::int32_t, bool)> place;
    std::function<std::string(std::string_view, std::string, std::int32_t)> move;
    std::function<std::string(std::string_view)> cancel;
    std::function<void()> disconnected, reconciled;
};

} // namespace moex::connector_host::dtc
