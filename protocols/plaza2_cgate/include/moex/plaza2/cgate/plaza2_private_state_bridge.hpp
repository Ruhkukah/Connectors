#pragma once

#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace moex::plaza2::cgate {

// Shared adapter from CGate listener callbacks to the private-state projector.
// Both live read-side sessions and the TEST pre-send transport use this bridge
// so LifeNum, CLEARDELETED, transaction and row-revision semantics stay exact.
class Plaza2PrivateStateBridge final : public Plaza2ListenerEventHandler {
  public:
    explicit Plaza2PrivateStateBridge(private_state::Plaza2PrivateStateProjector& projector);

    [[nodiscard]] Plaza2Error reset(std::span<const generated::StreamCode> streams);
    [[nodiscard]] Plaza2Error begin_run();
    [[nodiscard]] Plaza2Error end_run();

    [[nodiscard]] const fake::EngineState& state() const noexcept;
    [[nodiscard]] const std::string& last_resync_reason() const noexcept;
    [[nodiscard]] const std::string& callback_error() const noexcept;

    [[nodiscard]] Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override;

  private:
    struct PendingClearDeleted {
        generated::StreamCode stream_code{kNoStreamCode};
        generated::TableCode table_code{kNoTableCode};
        std::int64_t table_rev{0};
        std::uint32_t flags{0};
    };

    [[nodiscard]] Plaza2Error handle_event(const Plaza2ListenerEvent& event);
    [[nodiscard]] Plaza2Error handle_close(generated::StreamCode stream_code);
    [[nodiscard]] Plaza2Error handle_transaction_begin(generated::StreamCode stream_code);
    [[nodiscard]] Plaza2Error handle_transaction_commit(generated::StreamCode stream_code);
    [[nodiscard]] Plaza2Error handle_stream_data(const Plaza2ListenerEvent& event);
    [[nodiscard]] Plaza2Error handle_online(generated::StreamCode stream_code);
    [[nodiscard]] Plaza2Error handle_lifenum(generated::StreamCode stream_code, std::uint64_t life_number);
    [[nodiscard]] Plaza2Error handle_clear_deleted(const Plaza2ListenerEvent& event);
    [[nodiscard]] Plaza2Error handle_replstate(std::string_view replstate);
    [[nodiscard]] Plaza2Error ordering_error(std::string message);
    void recompute_online();

    private_state::Plaza2PrivateStateProjector& projector_;
    fake::ScenarioSpec scenario_{};
    fake::EngineState state_{};
    std::vector<std::uint64_t> pending_row_deltas_;
    std::vector<std::vector<PendingClearDeleted>> pending_clear_deleted_;
    std::vector<std::pair<generated::StreamCode, std::uint64_t>> stream_lifenums_;
    std::vector<std::string> text_storage_;
    std::vector<fake::FieldValueSpec> field_storage_;
    std::string last_resync_reason_;
    std::string callback_error_;
};

} // namespace moex::plaza2::cgate
