#pragma once

#include "moex/twime_trade/twime_recovery_state.hpp"
#include "moex/twime_trade/twime_session_health.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace moex::twime_trade {

struct TwimeSessionPersistenceSnapshot {
    TwimeRecoveryState recovery_state{};
    bool reject_seen{false};
    std::int64_t last_reject_code{0};
    std::uint32_t active_keepalive_interval_ms{0};
    std::uint64_t reconnect_attempt_count{0};
    std::uint64_t last_transition_time_ms{0};
};

class TwimeSessionPersistenceStore {
  public:
    virtual ~TwimeSessionPersistenceStore() = default;
    [[nodiscard]] virtual std::optional<TwimeSessionPersistenceSnapshot> load(std::string_view session_id) const = 0;
    virtual void save(std::string_view session_id, const TwimeSessionPersistenceSnapshot& snapshot) = 0;
};

class TwimeInMemorySessionPersistenceStore final : public TwimeSessionPersistenceStore {
  public:
    [[nodiscard]] std::optional<TwimeSessionPersistenceSnapshot> load(std::string_view session_id) const override;
    void save(std::string_view session_id, const TwimeSessionPersistenceSnapshot& snapshot) override;

  private:
    std::unordered_map<std::string, TwimeSessionPersistenceSnapshot> snapshots_;
};

class TwimeFileSessionPersistenceStore final : public TwimeSessionPersistenceStore {
  public:
    explicit TwimeFileSessionPersistenceStore(std::filesystem::path root_dir);

    [[nodiscard]] std::optional<TwimeSessionPersistenceSnapshot> load(std::string_view session_id) const override;
    void save(std::string_view session_id, const TwimeSessionPersistenceSnapshot& snapshot) override;

  private:
    [[nodiscard]] std::filesystem::path snapshot_path(std::string_view session_id) const;

    std::filesystem::path root_dir_;
};

class TwimePersistentRecoveryStateStore final : public TwimeRecoveryStateStore {
  public:
    explicit TwimePersistentRecoveryStateStore(TwimeSessionPersistenceStore& store);

    [[nodiscard]] std::optional<TwimeRecoveryState> load(std::string_view session_id) const override;
    void save(std::string_view session_id, const TwimeRecoveryState& state) override;

  private:
    TwimeSessionPersistenceStore& store_;
};

TwimeSessionPersistenceSnapshot make_twime_session_persistence_snapshot(const TwimeSessionHealthSnapshot& health,
                                                                        const TwimeRecoveryState& recovery_state);

} // namespace moex::twime_trade
