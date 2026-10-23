#pragma once
#include "moex/plaza2/cgate/plaza2_projection_types.hpp"
namespace moex::plaza2::fake {
using namespace moex::plaza2::projection;
class Plaza2FakeEngine {
  public:
    [[nodiscard]] RunResult run(const ScenarioDataView& view, CommitListener* listener = nullptr) const;

    [[nodiscard]] static const StreamState* find_stream_state(const EngineState& state,
                                                              generated::StreamCode stream_code);
};

} // namespace moex::plaza2::fake
