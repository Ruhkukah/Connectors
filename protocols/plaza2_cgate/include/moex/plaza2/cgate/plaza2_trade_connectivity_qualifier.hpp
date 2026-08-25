#pragma once

#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"
#include "moex/plaza2/cgate/plaza2_live_session_runner.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace moex::plaza2::cgate {

struct Plaza2QualificationTarget {
    // Exact isin_id, isin, or short_isin as supplied by the operator.  This is
    // target identity only; it is not an order intent.
    std::string isin;
    // Exact client-scope participant/client-code identity from FORTS_PART_REPL
    // and FORTS_POS_REPL.
    std::string participant;
    std::int8_t expected_position_account_type{2};
};

struct Plaza2TradeConnectivityQualifierConfig {
    Plaza2LiveSessionConfig session{};
    Plaza2QualificationTarget target{};
    bool test_market_data_armed{false};
    std::uint32_t max_aggr20_age_ms{5000};
};

enum class Plaza2QualificationState : std::uint8_t {
    Created = 0,
    Validated = 1,
    Started = 2,
    Ready = 3,
    Stopped = 4,
    Failed = 5,
};

enum class Plaza2QualificationTerminal : std::uint8_t {
    Ready = 0,
    NotReady = 1,
    Error = 2,
};

struct Plaza2QualificationSnapshot {
    Plaza2QualificationState state{Plaza2QualificationState::Created};
    Plaza2QualificationTerminal terminal{Plaza2QualificationTerminal::NotReady};
    bool connectivity_ready{false};
    bool market_state_ready{false};
    bool account_state_ready{false};
    bool publisher_ready{false};
    // Informational only.  This class has no send path and never submits an
    // order, even when this value is true.
    bool add_order_qualified{false};

    bool target_found{false};
    std::int32_t target_isin_id{0};
    std::int32_t target_sess_id{0};
    bool target_current_session_member{false};
    bool target_session_status_available{false};
    std::int32_t target_session_status{0};
    bool target_session_add_capable{false};
    bool target_instrument_status_available{false};
    std::int32_t target_instrument_status{0};
    bool target_instrument_add_capable{false};
    std::string target_min_step;
    std::int32_t target_trade_mode_id{0};
    bool target_refdata_present{false};
    bool target_aggr20_two_sided{false};
    std::uint32_t target_aggr20_age_ms{0};
    std::uint64_t target_aggr20_repl_id{0};
    std::int64_t target_aggr20_repl_rev{0};

    bool participant_limit_unique{false};
    bool participant_limits_set{false};
    std::size_t applicable_position_count{0};
    bool position_identity_exact{false};
    std::int8_t position_account_type{0};
    std::int64_t position_xpos{0};

    bool private_streams_ready{false};
    bool status_streams_ready{false};
    bool p2mqreply_open{false};
    bool publisher_open{false};
    bool runtime_trading_capable{false};
    std::vector<std::string> failure_reasons;
};

struct Plaza2TradeConnectivityQualificationResult {
    bool ok{false};
    std::string message;
};

[[nodiscard]] std::string_view plaza2_qualification_state_name(Plaza2QualificationState state) noexcept;
[[nodiscard]] std::string_view plaza2_qualification_terminal_name(Plaza2QualificationTerminal terminal) noexcept;

class Plaza2TradeConnectivityQualifier {
  public:
    explicit Plaza2TradeConnectivityQualifier(Plaza2TradeConnectivityQualifierConfig config);
    ~Plaza2TradeConnectivityQualifier();

    Plaza2TradeConnectivityQualifier(Plaza2TradeConnectivityQualifier&&) noexcept;
    Plaza2TradeConnectivityQualifier& operator=(Plaza2TradeConnectivityQualifier&&) noexcept;

    Plaza2TradeConnectivityQualifier(const Plaza2TradeConnectivityQualifier&) = delete;
    Plaza2TradeConnectivityQualifier& operator=(const Plaza2TradeConnectivityQualifier&) = delete;

    [[nodiscard]] Plaza2TradeConnectivityQualificationResult start();
    [[nodiscard]] Plaza2TradeConnectivityQualificationResult poll_once();
    [[nodiscard]] Plaza2TradeConnectivityQualificationResult stop();

    [[nodiscard]] const Plaza2QualificationSnapshot& qualification() const noexcept;
    [[nodiscard]] const Plaza2LiveSessionRunner& private_session() const noexcept;
    [[nodiscard]] const Plaza2Aggr20BookProjector& aggr20_projector() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moex::plaza2::cgate
