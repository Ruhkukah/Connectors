#pragma once

#include "moex/connector_host/trading_host.hpp"

namespace moex::connector_host {

struct OperatorProgramHooks {
    std::function<void(TradingHostConfig&)> configure;
    std::function<void(CgateTradingHost&)> after_poll;
    std::function<void(const std::filesystem::path&)> verify_command_endpoint;
};

// One operator loop for both executables: durable identities, command framing,
// order accounting, synchronous signals and the existing shutdown drain.
int operator_program(int argc, char** argv, const OperatorProgramHooks& hooks = {});

} // namespace moex::connector_host
