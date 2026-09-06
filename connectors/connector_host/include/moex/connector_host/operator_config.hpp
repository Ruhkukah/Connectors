#pragma once
#include "moex/connector_host/connector_host.hpp"

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
[[nodiscard]] OperatorRequest parse_operator_arguments(std::span<const std::string_view> arguments);
[[nodiscard]] std::string_view operator_help() noexcept;
} // namespace moex::connector_host
