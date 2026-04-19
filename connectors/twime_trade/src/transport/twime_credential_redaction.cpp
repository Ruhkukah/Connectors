#include "moex/twime_trade/transport/twime_credential_redaction.hpp"

#include "moex_core/logging/redaction.hpp"

namespace moex::twime_trade::transport {

std::string redact_twime_credentials(std::string_view credentials) {
    return moex::logging::redact_value(moex::logging::SecretKind::SessionSecret, std::string(credentials));
}

std::string redact_twime_account_like_value(std::string_view value) {
    return moex::logging::redact_value(moex::logging::SecretKind::AccountIdentifier, std::string(value));
}

std::string format_twime_test_network_banner(bool armed) {
    return armed ? "[TEST-NETWORK-ARMED]" : "[LOCAL-ONLY]";
}

} // namespace moex::twime_trade::transport
