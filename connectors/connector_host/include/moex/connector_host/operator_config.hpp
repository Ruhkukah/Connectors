#pragma once
#include "moex/connector_host/connector_host.hpp"
#include <filesystem>

namespace moex::connector_host {
struct OperatorRequest {
    Plaza2HostConfig config;
    std::string command;
    bool help{false}, json{false};
    std::uint32_t wait_ms{10000};
};
struct Plaza2HostConfigInputs {
    HostPurpose purpose{HostPurpose::Qualify};
    bool read_only_market_data{false}, public_deals{false}, allow_orders{false};
    plaza2::cgate::Plaza2Environment environment{plaza2::cgate::Plaza2Environment::Test};
    std::filesystem::path runtime_root, library_path, scheme_dir, config_dir;
    std::string env_open_settings, credentials_env_var, software_key_env_var, local_pass_env_var;
    std::string expected_spectra_release{"SPECTRA9.9.0"}, expected_scheme_sha256;
    std::string broker_code, client_code;
    std::string router{"127.0.0.1:4101"};
    std::int64_t isin_id{};
    std::vector<std::int64_t> isin_ids;
    std::string publisher_name{"moex_connector"};
    std::uint32_t publisher_messages_per_second{30};
};
[[nodiscard]] Plaza2HostConfig build_plaza2_host_config(const Plaza2HostConfigInputs& inputs);
[[nodiscard]] OperatorRequest parse_operator_arguments(std::span<const std::string_view> arguments);
[[nodiscard]] std::string_view operator_help() noexcept;
} // namespace moex::connector_host
