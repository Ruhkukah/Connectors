#pragma once

#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include <string>
#include <string_view>

namespace moex::plaza2::cgate {

struct Plaza2RuntimeArmState {
    bool test_network_armed{false};
    bool test_session_armed{false};
    bool test_plaza2_armed{false};
};

struct Plaza2ManualOperatorGateResult {
    bool allowed{false};
    Plaza2ErrorCode error_code{Plaza2ErrorCode::None};
    std::string reason;
};

class Plaza2ManualOperatorGate {
  public:
    [[nodiscard]] static Plaza2ManualOperatorGateResult
    validate_transport_connect(std::string_view endpoint_host, const Plaza2RuntimeArmState& arm_state);
    [[nodiscard]] static Plaza2ManualOperatorGateResult
    validate_session_start(std::string_view endpoint_host, const Plaza2RuntimeArmState& arm_state);
};

} // namespace moex::plaza2::cgate
