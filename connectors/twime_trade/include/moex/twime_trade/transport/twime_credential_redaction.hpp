#pragma once

#include <string>
#include <string_view>

namespace moex::twime_trade::transport {

[[nodiscard]] std::string redact_twime_credentials(std::string_view credentials);
[[nodiscard]] std::string redact_twime_account_like_value(std::string_view value);
[[nodiscard]] std::string format_twime_test_network_banner(bool armed);

} // namespace moex::twime_trade::transport
