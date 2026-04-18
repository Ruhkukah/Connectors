#pragma once

#include <string>

namespace moex::recovery {

struct RecoveryStateKey {
    std::string connector_id;
    std::string recovery_epoch;
    std::string source_stream;
};

struct RecoveryStateStoreSpec {
    std::string journal_root;
    std::string checkpoint_root;
    bool crash_safe_append_only = true;
};

}  // namespace moex::recovery
