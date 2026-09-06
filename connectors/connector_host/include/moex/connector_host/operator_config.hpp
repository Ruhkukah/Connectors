#pragma once
#include "moex/connector_host/connector_host.hpp"

#include <filesystem>

namespace moex::connector_host {
struct OperatorRequest {
    Plaza2HostConfig config;
    std::string command;
    bool help{false};
    bool json{false};
    std::uint32_t wait_ms{10000};
    std::filesystem::path canonical_plan_path;
    std::string authorized_sha256;
};

// Typed inputs shared by moexctl and the additive C ABI.  This is deliberately
// a narrow builder for the existing TEST ConnectorHost topology, not a second
// configuration format or a general profile framework.
struct Plaza2HostConfigInputs {
    HostPurpose purpose{HostPurpose::Qualify};
    std::filesystem::path runtime_root;
    std::filesystem::path library_path;
    std::filesystem::path scheme_dir;
    std::filesystem::path config_dir;
    std::string env_open_settings;
    std::string credentials_env_var;
    std::string software_key_env_var;
    std::string expected_spectra_release{"SPECTRA9.9.0"};
    std::string expected_scheme_sha256;
    std::string broker_code;
    std::string client_code;
    std::int64_t isin_id{0};
    std::int32_t session_id{0};
    plaza2::cgate::Plaza2RuntimeArmState arm_state{};
    std::string publisher_name;
    std::string profile_id{"connector-host-cabi-v2"};
    std::string profile_fingerprint{std::string(64, '0')};
    std::string policy_version{"connector-host-cabi-v2:qty1:distance4:age5000:zero"};
    std::string policy_sha256;
    std::string base_contract_code{"RTS"};
    std::string price{"103000"};
    std::string comment;
    plaza2_trade::Plaza2TradeSide side{plaza2_trade::Plaza2TradeSide::Sell};
    std::int32_t ext_id{79};
    std::uint32_t add_user_id{701};
    std::uint32_t cancel_user_id{702};
    std::uint32_t recovery_user_id{703};
    std::string run_id{"connector-host-cabi-v2"};
    std::filesystem::path journal_root;
    std::filesystem::path receipt_path;
};

[[nodiscard]] Plaza2HostConfig build_plaza2_host_config(const Plaza2HostConfigInputs& inputs);
[[nodiscard]] OperatorRequest parse_operator_arguments(std::span<const std::string_view> arguments);
[[nodiscard]] std::string_view operator_help() noexcept;
} // namespace moex::connector_host
