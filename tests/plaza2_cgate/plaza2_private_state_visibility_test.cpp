#include "moex/plaza2/cgate/plaza2_private_state.hpp"

#include "plaza2_fake_scenarios.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using moex::plaza2::fake::CommitListener;
using moex::plaza2::fake::EventSpec;
using moex::plaza2::fake::FindScenarioById;
using moex::plaza2::fake::Plaza2FakeEngine;
using moex::plaza2::fake::RowSpec;
using moex::plaza2::fake::ScenarioSpec;
using moex::plaza2::fake::ViewForScenario;
using moex::plaza2::private_state::Plaza2PrivateStateProjector;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool has_committed_projection(const Plaza2PrivateStateProjector& projector) {
    return !projector.sessions().empty() || !projector.instruments().empty() || !projector.matching_map().empty() ||
           !projector.limits().empty() || !projector.positions().empty() || !projector.own_orders().empty() ||
           !projector.own_trades().empty();
}

struct ObservingListener final : CommitListener {
    Plaza2PrivateStateProjector projector;
    bool visible_during_rows{false};
    bool visible_before_commit{false};
    std::size_t row_callbacks{0};
    std::size_t commit_callbacks{0};
    std::size_t committed_sessions{0};
    std::size_t committed_instruments{0};
    std::size_t committed_orders{0};
    std::size_t committed_trades{0};

    void on_event(const ScenarioSpec& scenario, const EventSpec& event,
                  const moex::plaza2::fake::EngineState& state) override {
        if (event.kind == moex::plaza2::fake::EventKind::kTransactionCommit && has_committed_projection(projector)) {
            visible_before_commit = true;
        }
        projector.on_event(scenario, event, state);
        if (event.kind == moex::plaza2::fake::EventKind::kTransactionCommit && has_committed_projection(projector)) {
            visible_before_commit = true;
        }
    }

    void on_stream_row(const ScenarioSpec& scenario, const EventSpec& event, const RowSpec& row,
                       std::span<const moex::plaza2::fake::FieldValueSpec> fields,
                       const moex::plaza2::fake::EngineState& state) override {
        if (has_committed_projection(projector)) {
            visible_during_rows = true;
        }
        projector.on_stream_row(scenario, event, row, fields, state);
        if (has_committed_projection(projector)) {
            visible_during_rows = true;
        }
        row_callbacks += 1;
    }

    void on_transaction_commit(const ScenarioSpec& scenario, const EventSpec& commit_event,
                               const moex::plaza2::fake::EngineState& state) override {
        if (has_committed_projection(projector)) {
            visible_before_commit = true;
        }
        projector.on_transaction_commit(scenario, commit_event, state);
        commit_callbacks += 1;
        committed_sessions = projector.sessions().size();
        committed_instruments = projector.instruments().size();
        committed_orders = projector.own_orders().size();
        committed_trades = projector.own_trades().size();
    }
};

} // namespace

int main() {
    try {
        const auto* scenario = FindScenarioById("private_state_projection");
        require(scenario != nullptr, "private_state_projection scenario is missing");

        ObservingListener listener;
        Plaza2FakeEngine engine;
        const auto result = engine.run(ViewForScenario(*scenario), &listener);
        if (result.error) {
            throw std::runtime_error("private_state_projection replay failed: " + result.error.message);
        }

        require(listener.row_callbacks > 0, "projection scenario should exercise row callbacks");
        require(listener.commit_callbacks == 1, "projection scenario should exercise one commit callback");
        require(!listener.visible_during_rows, "committed projection must remain hidden during row staging");
        require(!listener.visible_before_commit, "committed projection must remain hidden until TN_COMMIT");
        require(listener.committed_sessions == 1, "session state should become visible on commit");
        require(listener.committed_instruments == 3, "instrument state should become visible on commit");
        require(listener.committed_orders == 2, "own-order state should become visible on commit");
        require(listener.committed_trades == 1, "own-trade state should become visible on commit");

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
