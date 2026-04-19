#pragma once

#include "moex/twime_trade/transport/twime_tcp_config.hpp"
#include "moex/twime_trade/transport/twime_transport_events.hpp"

#include <string>

namespace moex::twime_trade::transport {

struct TwimeEndpointValidationResult {
    bool allowed{false};
    bool loopback{false};
    TwimeTransportErrorCode error_code{TwimeTransportErrorCode::None};
    TwimeTransportEvent event{TwimeTransportEvent::OpenStarted};
    std::string summary;
};

[[nodiscard]] TwimeEndpointValidationResult validate_twime_endpoint(const TwimeTcpConfig& config,
                                                                    const TwimeRuntimeArmState& arm_state);

class TwimeTestNetworkGate {
  public:
    TwimeTestNetworkGate(TwimeRuntimeArmState arm_state, TwimeTcpConfig config);

    [[nodiscard]] TwimeEndpointValidationResult validate_before_open() const;

  private:
    [[nodiscard]] TwimeRuntimeArmState effective_arm_state() const;

    TwimeRuntimeArmState arm_state_{};
    TwimeTcpConfig config_{};
};

} // namespace moex::twime_trade::transport
