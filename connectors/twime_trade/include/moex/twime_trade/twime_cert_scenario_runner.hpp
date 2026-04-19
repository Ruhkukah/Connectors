#pragma once

#include "moex/twime_trade/twime_session.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moex::twime_trade {

enum class TwimeCertScenarioActionKind {
    Connect,
    AdvanceClock,
    TimerTick,
    InjectInboundMessage,
    InjectPeerClose,
    CommandHeartbeat,
    CommandTerminate,
    CommandRetransmit,
};

struct TwimeCertScenarioAction {
    TwimeCertScenarioActionKind kind{TwimeCertScenarioActionKind::Connect};
    moex::twime_sbe::TwimeEncodeRequest message{};
    std::uint64_t sequence_number{0};
    bool consumes_sequence{false};
    std::uint64_t advance_ms{0};
    std::uint64_t retransmit_from_seq_no{0};
    std::uint32_t retransmit_count{0};
};

struct TwimeCertScenario {
    std::string scenario_id;
    std::string title;
    TwimeSessionConfig config{};
    std::vector<TwimeCertScenarioAction> actions;
    std::optional<TwimeSessionState> expected_final_state;
};

struct TwimeCertScenarioResult {
    std::string scenario_id;
    std::string title;
    TwimeSessionState final_state{TwimeSessionState::Created};
    std::vector<TwimeSessionEvent> events;
    std::vector<std::string> cert_log_lines;
    std::string error_message;
};

class TwimeCertScenarioRunner {
  public:
    [[nodiscard]] TwimeCertScenarioResult run(const TwimeCertScenario& scenario) const;
    [[nodiscard]] static std::optional<TwimeCertScenario> builtin(std::string_view scenario_id);
};

} // namespace moex::twime_trade
