#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace moex::plaza2;
using namespace moex::plaza2::cgate;
using enum generated::StreamCode;
using enum generated::FieldCode;
using enum generated::TableCode;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        private_state::Plaza2PrivateStateProjector projector;
        Plaza2PrivateStateBridge bridge(projector);
        std::array streams{kFortsTradeRepl, kFortsPosRepl};
        require(!bridge.reset(streams), "reset");
        require(!bridge.begin_run(), "begin run");
        const auto event = [&](Plaza2ListenerEventKind kind, generated::StreamCode stream) {
            require(!bridge.on_plaza2_listener_event({.kind = kind, .stream_code = stream}), "event rejected");
        };
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsPosRepl);
        std::array position{Plaza2DecodedFieldValue{.field_code = kFortsPosReplPositionIsinId,
                                                    .kind = Plaza2DecodedValueKind::SignedInteger,
                                                    .signed_value = 1001},
                            Plaza2DecodedFieldValue{.field_code = kFortsPosReplPositionXpos,
                                                    .kind = Plaza2DecodedValueKind::SignedInteger,
                                                    .signed_value = 3}};
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsPosRepl,
                                                  .table_code = kFortsPosReplPosition,
                                                  .fields = position}),
                "POS row");
        auto start = std::chrono::steady_clock::now();
        std::array fields{Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicOrderId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPrivateOrderId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogIsinId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 1001},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicAmount,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 5},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicAmountRest,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 5}};
        for (std::int64_t id = 1; id <= 150000; ++id) {
            fields[0].signed_value = id;
            fields[1].signed_value = id;
            require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                      .stream_code = kFortsTradeRepl,
                                                      .table_code = kFortsTradeReplOrdersLog,
                                                      .fields = fields,
                                                      .signed_value = id}),
                    "TRADE row");
        }
        require(projector.own_orders().empty() && projector.positions().empty(), "uncommitted rows became visible");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        require(projector.own_orders().size() == 150000, "order index lost rows");
        require(projector.positions().empty(), "TRADE commit leaked open POS transaction");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsPosRepl);
        require(projector.positions().size() == 1 && projector.positions()[0].xpos == 3,
                "POS transaction not independently committed");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "clear");
        event(Plaza2ListenerEventKind::Online, kFortsTradeRepl);
        require(projector.own_orders().empty() && projector.positions().size() == 1,
                "ClearDeleted affected wrong domain");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .fields = fields,
                                                  .signed_value = 1}),
                "row after MAX");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        require(projector.own_orders().size() == 1, "MAX revision reset did not accept new row");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        fields[0].signed_value = 150001;
        fields[1].signed_value = 150001;
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .fields = fields,
                                                  .signed_value = 2}),
                "second fresh row");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .signed_value = 2}),
                "normal clear");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        require(projector.own_orders().size() == 1 && projector.own_orders()[0].public_order_id == 150001,
                "normal ClearDeleted did not filter rows by revision");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsPosRepl,
                                                  .table_code = kFortsPosReplPosition,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "POS clear");
        event(Plaza2ListenerEventKind::Online, kFortsPosRepl);
        require(projector.positions().empty() && projector.own_orders().size() == 1, "POS clear crossed streams");
        std::cout << "150000 TRADE rows including commit: " << elapsed << " ms\n";
#if defined(__linux__) && defined(NDEBUG)
        require(elapsed < 1000, "150k TRADE snapshot exceeds Linux Release 1 second acceptance");
#endif
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
