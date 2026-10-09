#include "plaza2_trade_test_support.hpp"
#include "plaza2_trade_official_wire_test_support.hpp"

#include <iostream>

namespace {

using moex::plaza2_trade::bytes_to_hex;
using moex::plaza2_trade::Plaza2TradeCodec;
using moex::plaza2_trade::Plaza2TradeCommandRequest;
using moex::plaza2_trade::test_support::fixture_text;
using moex::plaza2_trade::test_support::make_add_order;
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
}

void test_deterministic_repeated_encoding() {
    const Plaza2TradeCodec codec;
    const auto first = codec.encode(Plaza2TradeCommandRequest{make_add_order()});
    const auto second = codec.encode(Plaza2TradeCommandRequest{make_add_order()});
    require(first.payload == second.payload, "same command should encode byte-identically");
    require(first.msgid == 474, "AddOrder msgid must match official CGate");
}

void test_post_send_wire_journal_fields() {
    using moex::plaza2_trade::command_fields_json;
    const Plaza2TradeCodec codec;
    const auto has = [](const auto& json, std::string_view field) { return json.find(field) != std::string::npos; };
    const auto add = command_fields_json(codec.encode(make_add_order()));
    require(has(add, "\"isin_id\":123456") && has(add, "\"amount\":10") && has(add, "\"price\":\"101.25\"") &&
                has(add, "\"ext_id\":501") && has(add, "\"comment\":\"offline\"") && has(add, "\"dont_check_money\":0"),
            "post-send Add journal lost actual packed values/defaults");
    const auto del = command_fields_json(codec.encode(make_del_order()));
    require(has(del, "\"order_id\":9001") && has(del, "\"client_code\":\"C01\"") && has(del, "\"isin_id\":123456"),
            "post-send cancel journal lost ownership or official order identity");
    auto move_request = make_move_order();
    move_request.regime = 3;
    const auto move = command_fields_json(codec.encode(move_request));
    require(has(move, "\"regime\":3") && has(move, "\"order_id1\":9001") && has(move, "\"order_id2\":9002") &&
                has(move, "\"amount1\":5") && has(move, "\"price2\":\"103.00\"") && has(move, "\"ext_id2\":602"),
            "post-send Move journal lost packed regime/replacement values");
    const auto mass = command_fields_json(codec.encode(make_del_user_orders()));
    require(has(mass, "\"buy_sell\":3") && has(mass, "\"code\":\"C01\"") && has(mass, "\"ext_id\":777") &&
                has(mass, "\"instrument_mask\":1"),
            "post-send mass-cancel journal lost packed scope");
    auto truncated = codec.encode(make_add_order());
    truncated.payload.resize(8);
    bool rejected{};
    try {
        (void)command_fields_json(truncated);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "command journal read past a truncated payload");
}

void test_all_official_command_layouts() {
    using namespace moex::plaza2_trade::test_support;
    const Plaza2TradeCodec codec;
    const std::int32_t expected_ids[] = {official_cgate99::AddOrder_msgid, official_cgate99::DelOrder_msgid,
                                         official_cgate99::MoveOrder_msgid, official_cgate99::DelUserOrders_msgid};
    std::size_t index = 0;
    for (const auto& request : std::vector<Plaza2TradeCommandRequest>{make_add_order(), make_del_order(),
                                                                      make_move_order(), make_del_user_orders()}) {
        const auto encoded = codec.encode(request);
        require(encoded.validation.ok(), "official-layout command must validate");
        require(encoded.msgid == expected_ids[index++], "declared command message id differs from official scheme");
        require(encoded.payload == std::visit([](const auto& value) { return official_wire(value); }, request),
                "four declared command bytes must match official layout");
    }
}

} // namespace

int main() {
    try {
        test_golden_encodings();
        test_deterministic_repeated_encoding();
        test_post_send_wire_journal_fields();
        test_all_official_command_layouts();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
