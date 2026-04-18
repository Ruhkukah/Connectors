#pragma once

#include "moex/twime_sbe/twime_schema.hpp"
#include "moex/twime_sbe/twime_types.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace moex::twime_sbe {

class TwimeCodec {
public:
    [[nodiscard]] TwimeDecodeError encode_message(
        const TwimeEncodeRequest& request,
        std::vector<std::byte>& out_bytes) const;

    [[nodiscard]] TwimeDecodeError decode_message(
        std::span<const std::byte> bytes,
        DecodedTwimeMessage& out_message) const;
};

}  // namespace moex::twime_sbe
