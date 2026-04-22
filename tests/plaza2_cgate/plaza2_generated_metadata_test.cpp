#include "plaza2_generated_metadata.hpp"

#include <iostream>

int main() {
    using namespace moex::plaza2::generated;

    if (TypeDescriptors().empty() || StreamDescriptors().empty() || TableDescriptors().empty() ||
        FieldDescriptors().empty()) {
        std::cerr << "generated metadata arrays must not be empty\n";
        return 1;
    }

    const auto* trade_stream = FindStreamByCode(StreamCode::kFortsTradeRepl);
    if (trade_stream == nullptr) {
        std::cerr << "FORTS_TRADE_REPL stream descriptor missing\n";
        return 1;
    }
    if (trade_stream->table_count < 4) {
        std::cerr << "FORTS_TRADE_REPL should expose multiple tables\n";
        return 1;
    }

    const auto* orders_log = FindTableByCode(TableCode::kFortsTradeReplOrdersLog);
    if (orders_log == nullptr) {
        std::cerr << "orders_log descriptor missing\n";
        return 1;
    }
    if (orders_log->field_count < 10) {
        std::cerr << "orders_log should expose multiple fields\n";
        return 1;
    }

    const auto fields = FieldsForTable(TableCode::kFortsTradeReplOrdersLog);
    if (fields.empty()) {
        std::cerr << "orders_log fields missing\n";
        return 1;
    }
    if (!fields.front().service_field || fields.front().field_code != FieldCode::kFortsTradeReplOrdersLogReplId) {
        std::cerr << "orders_log first field should be replID service field\n";
        return 1;
    }

    const auto* decimal_type = FindTypeByToken("d16.5");
    if (decimal_type == nullptr) {
        std::cerr << "d16.5 type descriptor missing\n";
        return 1;
    }
    if (decimal_type->value_class != ValueClass::kDecimal || decimal_type->decimal_scale != 5) {
        std::cerr << "d16.5 type descriptor invariants broken\n";
        return 1;
    }

    const auto trade_tables = TablesForStream(StreamCode::kFortsTradeRepl);
    if (trade_tables.size() != trade_stream->table_count) {
        std::cerr << "stream table span does not match descriptor count\n";
        return 1;
    }

    return 0;
}
