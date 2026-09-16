#pragma once

#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"
#include "moex/plaza2/cgate/plaza2_private_state.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2::cgate {

enum class Plaza2Aggr20AuthorityProbeResult : std::uint8_t {
    Yes = 0,
    No = 1,
    Inconclusive = 2,
};

[[nodiscard]] std::string_view
plaza2_aggr20_authority_probe_result_name(Plaza2Aggr20AuthorityProbeResult result) noexcept;

struct Plaza2Aggr20AuthorityProbeSysEvent {
    std::uint64_t source_repl_id{0};
    std::int64_t source_repl_rev{0};
    std::int64_t source_repl_act{0};
    std::int32_t event_type{0};
    std::int64_t event_id{0};
    std::int32_t sess_id{0};
    std::string message;
    std::uint64_t server_time{0};
    std::uint64_t transaction_id{0};
    std::uint64_t transaction_row_index{0};
    bool observed_before_online{false};
    bool transaction_committed{false};
};

struct Plaza2Aggr20AuthorityProbeSelection {
    std::int32_t sess_id{0};
    std::int32_t isin_id{0};
    std::string symbol;
    std::string short_symbol;
    std::string name;
    std::string base_contract_code;
    std::uint64_t refdata_lifenum{0};
    std::optional<private_state::SourceRowProvenance> fut_instruments_source;
    std::optional<private_state::SourceRowProvenance> fut_sess_contents_source;
    std::optional<private_state::SourceRowProvenance> session_source;
};

struct Plaza2Aggr20AuthorityProbeAttempt {
    std::uint32_t ordinal{0};
    std::string name;
    bool listener_created{false};
    bool listener_opened{false};
    bool online{false};
    bool snapshot_complete{false};
    bool session_data_ready_after_online{false};
    bool target_authoritative{false};
    bool experiment_complete{false};
    std::vector<Plaza2Aggr20AuthorityProbeSysEvent> sys_events;
    std::vector<std::string> event_trace;
    std::string error;
};

struct Plaza2Aggr20AuthorityProbeReport {
    Plaza2Aggr20AuthorityProbeResult result{Plaza2Aggr20AuthorityProbeResult::Inconclusive};
    bool read_only_contract_ok{false};
    bool refdata_listener_created{false};
    bool refdata_listener_opened{false};
    bool refdata_online{false};
    bool refdata_snapshot_complete{false};
    std::optional<bool> initial_open_session_data_ready_after_online;
    std::optional<bool> listener_reopen_session_data_ready_after_online;
    std::optional<Plaza2Aggr20AuthorityProbeSelection> selection;
    std::vector<Plaza2Aggr20AuthorityProbeAttempt> attempts;
    std::string error;
};

[[nodiscard]] Plaza2Aggr20AuthorityProbeResult plaza2_aggr20_authority_probe_classify_attempts(
    const std::vector<Plaza2Aggr20AuthorityProbeAttempt>& attempts) noexcept;

struct Plaza2Aggr20AuthorityProbeConfig {
    // This is a diagnostic identity, not an authorized order profile.
    std::string profile_id{"plaza2_aggr20_authority_probe"};
    std::string endpoint_host;
    std::uint16_t endpoint_port{0};
    Plaza2Settings runtime{};
    std::string connection_settings;
    std::string connection_open_settings;
    Plaza2Aggr20MdStreamConfig refdata_stream{
        .stream_code = generated::StreamCode::kFortsRefdataRepl,
        .settings = "p2repl://FORTS_REFDATA_REPL",
        .open_settings = "mode=snapshot+online",
    };
    Plaza2Aggr20MdStreamConfig aggr_stream{
        .stream_code = generated::StreamCode::kFortsAggrRepl,
        .settings = "p2repl://FORTS_AGGR20_REPL",
        .open_settings = "mode=snapshot+online",
    };
    Plaza2CredentialConfig credentials{};
    Plaza2CredentialConfig software_key{};
    Plaza2RuntimeArmState arm_state{};
    std::uint32_t process_timeout_ms{50};
    std::chrono::milliseconds observation_window{std::chrono::seconds(60)};
};

class Plaza2Aggr20AuthorityProbe final {
  public:
    explicit Plaza2Aggr20AuthorityProbe(Plaza2Aggr20AuthorityProbeConfig config);

    [[nodiscard]] Plaza2Aggr20AuthorityProbeReport run();

  private:
    Plaza2Aggr20AuthorityProbeConfig config_;
};

} // namespace moex::plaza2::cgate
