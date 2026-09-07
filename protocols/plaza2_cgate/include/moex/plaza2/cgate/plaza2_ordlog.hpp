#pragma once

#include "moex/plaza2/cgate/plaza2_public_decode.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include <vector>

namespace moex::plaza2::cgate {

enum class OrdlogState : std::uint8_t { Disabled, Opening, History, CatchingUp, Online, Stale, Recovering, Failed };
enum class OrdlogRevision : std::uint8_t { Increasing, Replay, Duplicate, Discontinuity, GenerationChange };

// All APIs run on the existing CGate connection owner. Borrowed records expire at acknowledge().
struct OrdlogRecord {
    Plaza2ListenerEventKind kind{};
    generated::TableCode table{kNoTableCode};
    std::uint16_t table_index{}, size{}, null_count{};
    OrdlogRevision revision{};
    std::uint64_t sequence{}, life{}, generation{}, received_ns{};
    std::int64_t repl_id{}, repl_rev{}, repl_act{};
    std::uint32_t flags{};
    std::array<std::byte, 148> wire{};
    std::array<std::uint8_t, 19> nulls{};
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {wire.data(), size};
    }
};

struct OrdlogMetrics {
    std::uint64_t records{}, acknowledged{}, decode_failures{}, revision_failures{}, discontinuities{};
    std::uint64_t duplicates{}, replays{}, generation_changes{}, mandatory_overflows{}, dropped{};
    std::uint64_t uncommitted_rolled_back{};
    std::uint64_t optional_overflows{}, optional_dropped{}, optional_first_lost_sequence{};
    std::size_t queued{}, high_water{}, optional_queued{};
    std::uint64_t oldest_age_ns{};
};

class Plaza2Ordlog final : public Plaza2ListenerEventHandler {
  public:
    explicit Plaza2Ordlog(std::size_t capacity = 65536, std::size_t optional_capacity = 0);
    [[nodiscard]] bool wants_raw_replication() const noexcept override {
        return true;
    }
    void on_plaza2_listener_error(const Plaza2Error&) noexcept override;
    [[nodiscard]] Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent&) override;
    [[nodiscard]] Plaza2Error create(Plaza2Connection&, std::string_view scheme_file);
    // Reopen only after mandatory committed output is drained. Failure forces a fresh history.
    [[nodiscard]] Plaza2Error open();
    [[nodiscard]] Plaza2Error close();
    // Call before pumping the shared connection: one timestamp sample per poll, never per record.
    void poll_time(std::uint64_t monotonic_ns) noexcept {
        now_ns_ = monotonic_ns;
    }
    [[nodiscard]] Plaza2Error supervise(std::uint64_t monotonic_ns);
    [[nodiscard]] const OrdlogRecord* front() const noexcept;
    [[nodiscard]] bool acknowledge() noexcept;
    [[nodiscard]] const OrdlogRecord* optional_front() const noexcept;
    [[nodiscard]] bool optional_acknowledge() noexcept;
    [[nodiscard]] bool optional_overflowed() const noexcept {
        return optional_failed_;
    }
    [[nodiscard]] OrdlogMetrics metrics() const noexcept;
    [[nodiscard]] OrdlogState state() const noexcept {
        return state_;
    }
    // This is eligibility only: no persistence is performed by the raw receiver.
    [[nodiscard]] std::string_view checkpoint() const noexcept;

  private:
    struct RevisionState {
        std::int64_t maximum{};
        OrdlogRecord last{};
        bool present{};
    };
    [[nodiscard]] Plaza2Error fail(std::string_view message, bool decode = false, bool revision = false);
    [[nodiscard]] Plaza2Error append(const OrdlogRecord&);
    void publish() noexcept;
    void invalidate_uncommitted() noexcept;
    std::vector<OrdlogRecord> queue_, optional_;
    std::array<RevisionState, 4> revisions_{}, committed_revisions_{};
    std::uint64_t read_{}, committed_{}, write_{}, optional_read_{}, optional_write_{};
    std::uint64_t sequence_{}, life_{}, generation_{}, now_ns_{}, retry_after_ns_{};
    bool have_commit_{};
    bool transaction_{}, source_online_{}, failed_{}, optional_failed_{}, enabled_{};
    OrdlogState state_{OrdlogState::Disabled};
    OrdlogMetrics metrics_{};
    std::string checkpoint_;
    Plaza2Connection* connection_{};
    Plaza2Listener listener_; // Destroyed before callback state/buffers.
};
} // namespace moex::plaza2::cgate
