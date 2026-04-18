#pragma once

#include <string>

namespace moex::cert {

struct CertLogPolicy {
    bool deterministic_output = true;
    bool redact_secrets = true;
    std::string textual_format = "message_like_text";
};

}  // namespace moex::cert
