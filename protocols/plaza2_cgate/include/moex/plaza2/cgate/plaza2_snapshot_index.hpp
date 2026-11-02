#pragma once

#include <cstddef>

namespace moex::plaza2::private_state::detail {

template <typename Map, typename Key> bool rebind_snapshot_slot(Map& index, const Key& key, std::size_t slot) noexcept {
    const auto found = index.find(key);
    if (found == index.end())
        return false;
    found->second = slot;
    return true;
}

} // namespace moex::plaza2::private_state::detail
