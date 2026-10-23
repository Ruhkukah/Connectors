#pragma once

#include "moex/plaza2/cgate/plaza2_snapshot_index.hpp"
#include "private_state_deletion_regression.hpp"

#include <unordered_map>

namespace moex::plaza2::test {

inline void private_commit_index_regression() {
    using deletion::require;
    std::unordered_map<int, std::size_t> slots{{10, 0}, {20, 1}};
    require(!private_state::detail::rebind_snapshot_slot(slots, 30, 0),
            "a missing moved snapshot key must request rebuilding without insertion");
    require(slots.size() == 2 && slots.at(10) == 0 && slots.at(20) == 1,
            "failed slot rebinding modified the committed index");
    require(private_state::detail::rebind_snapshot_slot(slots, 20, 0) && slots.at(20) == 0,
            "a retained moved snapshot key did not receive its compacted slot");
    require(noexcept(private_state::detail::rebind_snapshot_slot(slots, 30, 0)),
            "snapshot slot rebinding permits an invariant exception inside commit");
    deletion::native_batch_deletion();
}

} // namespace moex::plaza2::test
