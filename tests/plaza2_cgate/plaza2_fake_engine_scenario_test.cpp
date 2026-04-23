#include "plaza2_fake_scenarios.hpp"

#include <iostream>

int main() {
    using namespace moex::plaza2::fake;

    Plaza2FakeEngine engine;
    for (const auto& scenario : FakeScenarioSpecs()) {
        const auto result = engine.run(ViewForScenario(scenario));
        if (result.error) {
            std::cerr << "scenario " << scenario.scenario_id << " failed: " << result.error.message << '\n';
            return 1;
        }
    }

    const auto* basic = FindScenarioById("forts_basic_snapshot_online");
    if (basic == nullptr) {
        std::cerr << "forts_basic_snapshot_online scenario missing\n";
        return 1;
    }
    const auto basic_result = engine.run(ViewForScenario(*basic));
    if (basic_result.error) {
        std::cerr << "basic scenario unexpectedly failed\n";
        return 1;
    }
    if (basic_result.state.commit_count != 1 || !basic_result.state.online) {
        std::cerr << "basic scenario state invariants drifted\n";
        return 1;
    }
    const auto* trade_stream =
        Plaza2FakeEngine::find_stream_state(basic_result.state, moex::plaza2::generated::StreamCode::kFortsTradeRepl);
    if (trade_stream == nullptr || !trade_stream->online || !trade_stream->snapshot_complete ||
        trade_stream->committed_row_count != 2) {
        std::cerr << "trade stream state drifted in basic scenario\n";
        return 1;
    }

    const auto* resume = FindScenarioById("resume_from_replstate");
    if (resume == nullptr) {
        std::cerr << "resume_from_replstate scenario missing\n";
        return 1;
    }
    const auto resume_result = engine.run(ViewForScenario(*resume));
    if (resume_result.error || resume_result.state.last_replstate != "lifenum=5;rev.orders_log=123") {
        std::cerr << "resume_from_replstate invariant drifted\n";
        return 1;
    }

    const auto* reset = FindScenarioById("lifenum_reset");
    if (reset == nullptr) {
        std::cerr << "lifenum_reset scenario missing\n";
        return 1;
    }
    const auto reset_result = engine.run(ViewForScenario(*reset));
    if (reset_result.error || !reset_result.state.has_lifenum || reset_result.state.last_lifenum != 11) {
        std::cerr << "lifenum_reset invariant drifted\n";
        return 1;
    }
    const auto* reset_stream =
        Plaza2FakeEngine::find_stream_state(reset_result.state, moex::plaza2::generated::StreamCode::kFortsTradeRepl);
    if (reset_stream == nullptr || reset_stream->committed_row_count != 0 || reset_stream->online) {
        std::cerr << "lifenum reset should invalidate committed stream state\n";
        return 1;
    }

    return 0;
}
