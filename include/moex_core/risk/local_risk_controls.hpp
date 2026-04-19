#pragma once

#include <string>

namespace moex::risk {

struct LocalRiskControls {
    bool max_order_qty = true;
    bool max_position = true;
    bool max_open_orders = true;
    bool order_rate_throttle = true;
    bool duplicate_cl_ord_id_guard = true;
    bool stale_price_guard = true;
    bool price_band_guard = true;
    bool kill_switch = true;
    bool cancel_all_on_shutdown = false;
};

struct BrokerTopology {
    std::string mode = "unknown";
    std::string router_location = "unknown";
    std::string market_data_path = "unknown";
    std::string order_path = "unknown";
};

} // namespace moex::risk
