#include "plaza2_trade_test_support.hpp"
#include "plaza2_trade_official_wire_test_support.hpp"

#include <iostream>

namespace {

using moex::plaza2_trade::bytes_to_hex;
using moex::plaza2_trade::Plaza2TradeCodec;
using moex::plaza2_trade::Plaza2TradeCommandRequest;
using moex::plaza2_trade::test_support::fixture_text;
using moex::plaza2_trade::test_support::make_add_order;
using moex::plaza2_trade::test_support::make_cod_heartbeat;
using moex::plaza2_trade::test_support::make_del_order;
using moex::plaza2_trade::test_support::make_del_user_orders;
using moex::plaza2_trade::test_support::make_move_order;
using moex::plaza2_trade::test_support::require;

void assert_golden(const char* fixture, const Plaza2TradeCommandRequest& request) {
    const Plaza2TradeCodec codec;
    const auto encoded = codec.encode(request);
    require(encoded.validation.ok(), "golden command should encode");
    require(encoded.payload ==
                std::visit([](const auto& value) { return moex::plaza2_trade::test_support::official_wire(value); },
                           request),
            "payload must match independent official CGate pack(4) structure");
    require(bytes_to_hex(encoded.payload) == fixture_text(fixture), "encoded command bytes differ from golden fixture");
}

void test_golden_encodings() {
    assert_golden("add_order_minimal.golden.bin.hex", Plaza2TradeCommandRequest{make_add_order()});
    assert_golden("del_order_minimal.golden.bin.hex", Plaza2TradeCommandRequest{make_del_order()});
    assert_golden("move_order_minimal.golden.bin.hex", Plaza2TradeCommandRequest{make_move_order()});
    assert_golden("del_user_orders_minimal.golden.bin.hex", Plaza2TradeCommandRequest{make_del_user_orders()});
    assert_golden("cod_heartbeat_minimal.golden.bin.hex", Plaza2TradeCommandRequest{make_cod_heartbeat()});
}

void test_deterministic_repeated_encoding() {
    const Plaza2TradeCodec codec;
    const auto first = codec.encode(Plaza2TradeCommandRequest{make_add_order()});
    const auto second = codec.encode(Plaza2TradeCommandRequest{make_add_order()});
    require(first.payload == second.payload, "same command should encode byte-identically");
    require(first.msgid == 474, "AddOrder msgid should come from Phase 5A lock");
}

void test_all_official_command_layouts() {
    using namespace moex::plaza2_trade::test_support;
    const Plaza2TradeCodec codec;
    for (const auto& request : std::vector<Plaza2TradeCommandRequest>{
             make_add_order(), make_iceberg_add_order(), make_del_order(), make_iceberg_del_order(), make_move_order(),
             make_iceberg_move_order(), make_del_user_orders(), make_del_orders_by_bf_limit(), make_cod_heartbeat()}) {
        const auto encoded = codec.encode(request);
        require(encoded.validation.ok(), "official-layout command must validate");
        require(encoded.payload == std::visit([](const auto& value) { return official_wire(value); }, request),
                "all existing command bytes must match official layout");
    }
}

} // namespace

int main() {
    try {
        test_golden_encodings();
        test_deterministic_repeated_encoding();
        test_all_official_command_layouts();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
