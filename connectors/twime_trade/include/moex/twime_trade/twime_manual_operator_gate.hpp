#pragma once

#include "moex/twime_trade/transport/twime_tcp_config.hpp"
#include "moex/twime_trade/transport/twime_transport_errors.hpp"

#include <string>

namespace moex::twime_trade {

struct TwimeManualOperatorGateResult {
    bool allowed{false};
    transport::TwimeTransportErrorCode error_code{transport::TwimeTransportErrorCode::None};
    std::string reason;
};

class TwimeManualOperatorGate {
  public:
    static TwimeManualOperatorGateResult validate_transport_connect(
        const transport::TwimeTcpConfig& config, const transport::TwimeRuntimeArmState& arm_state);
    static TwimeManualOperatorGateResult validate_session_start(
        const transport::TwimeTcpConfig& config, const transport::TwimeRuntimeArmState& arm_state);
};

} // namespace moex::twime_trade
