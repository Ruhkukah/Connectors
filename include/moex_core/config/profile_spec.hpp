#pragma once

#include <string>
#include <vector>

namespace moex::config {

struct ShadowModeSpec {
    bool enabled = false;
    bool native_read_only = true;
    std::vector<std::string> compare_surfaces;
};

struct ProfileSpec {
    std::string profile_id;
    std::string environment;
    bool armed = false;
    bool profile_neutral_coverage = true;
    ShadowModeSpec shadow_mode{};
};

}  // namespace moex::config
