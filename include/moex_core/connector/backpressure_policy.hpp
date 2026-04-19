#pragma once

#include "moex_core/phase0_core.hpp"

namespace moex::connector {

struct BackpressurePolicy {
    moex::phase0::EventLossPolicy private_trade_policy = moex::phase0::EventLossPolicy::Lossless;
    moex::phase0::EventLossPolicy order_status_policy = moex::phase0::EventLossPolicy::Lossless;
    moex::phase0::EventLossPolicy full_order_log_policy = moex::phase0::EventLossPolicy::Lossless;
    moex::phase0::EventLossPolicy public_l1_policy = moex::phase0::EventLossPolicy::DropOldest;
    moex::phase0::EventLossPolicy diagnostics_policy = moex::phase0::EventLossPolicy::DropOldest;
};

} // namespace moex::connector
