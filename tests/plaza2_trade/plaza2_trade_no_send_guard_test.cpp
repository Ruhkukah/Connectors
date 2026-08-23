#include "plaza2_trade_test_support.hpp"

#include <iostream>

namespace {

using moex::plaza2_trade::is_sendable;
using moex::plaza2_trade::Plaza2TradeCodec;
using moex::plaza2_trade::Plaza2TradeCommandRequest;
using moex::plaza2_trade::test_support::make_add_order;
using moex::plaza2_trade::test_support::require;

void test_encoded_command_is_not_sendable() {
    const Plaza2TradeCodec codec;
    const auto encoded = codec.encode(Plaza2TradeCommandRequest{make_add_order()});
    require(encoded.validation.ok(), "sample command should encode");
    require(encoded.offline_only, "encoded command should carry offline-only marker");
    require(!is_sendable(encoded), "encoded command must not be sendable");
}

} // namespace

int main() {
    try {
        test_encoded_command_is_not_sendable();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
