#pragma once

#include "moex/twime_sbe/twime_types.hpp"

#include <string>

namespace moex::twime_sbe {

class TwimeCertLogFormatter {
  public:
    [[nodiscard]] std::string format(const DecodedTwimeMessage& message) const;
};

} // namespace moex::twime_sbe
