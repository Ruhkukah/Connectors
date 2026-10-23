#pragma once

#include "field_read_contract.hpp"
#include "private_futures_scope_regression.hpp"

namespace moex::plaza2::test {
inline void private_field_read_contract_regression() {
    FieldReadContract reads;
    deletion::Harness h;
    for (const auto table : deletion::tables) {
        h.begin(table, true);
        h.row(table, 10, 1);
        h.commit();
        h.begin(table, true);
        h.row(table, 10, 2, 1, true);
        h.commit();
    }
    private_futures_scope_regression(); // Ignored rows must never access their removed fields.
    reads.verify("regular private projection", 170);
}
} // namespace moex::plaza2::test
