#pragma once

#include <string>

namespace moex::connector {

struct HealthSnapshot {
    std::string connector_id;
    std::string profile_id;
    std::string state;
    bool prod_armed = false;
    bool shadow_mode_enabled = false;
};

} // namespace moex::connector
