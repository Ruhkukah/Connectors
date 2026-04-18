#pragma once

#include <string>

namespace moex::logging {

enum class SecretKind {
    Password,
    RefreshToken,
    SessionSecret,
    PrivateKey,
    AuthorizationHeader,
    AccountIdentifier
};

struct RedactionRule {
    SecretKind kind;
    bool deterministic = true;
    bool allow_partial_identifier = false;
};

std::string redact_value(SecretKind kind, const std::string& value);

}  // namespace moex::logging
