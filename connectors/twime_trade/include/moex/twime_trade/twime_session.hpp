#pragma once

#include "moex/twime_trade/twime_fake_transport.hpp"
#include "moex/twime_trade/twime_rate_limit_model.hpp"
#include "moex/twime_trade/twime_recovery_state.hpp"
#include "moex/twime_trade/twime_sequence_state.hpp"
#include "moex/twime_trade/transport/itwime_byte_transport.hpp"
#include "moex/twime_sbe/twime_cert_log_formatter.hpp"
#include "moex/twime_sbe/twime_codec.hpp"
#include "moex/twime_sbe/twime_frame_assembler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace moex::twime_trade {

enum class TwimeRecoveryMode {
    NormalSessionRecovery,
    FullRecoveryService,
};

enum class TwimeSessionState {
    Created,
    ConnectingFake,
    Establishing,
    Active,
    Terminating,
    Terminated,
    Rejected,
    Faulted,
    Recovering,
};

enum class TwimeSessionCommandType {
    ConnectFake,
    SendTerminate,
    SendHeartbeat,
    RequestRetransmit,
    PollTransport,
};

struct TwimeSessionCommand {
    TwimeSessionCommandType type{TwimeSessionCommandType::PollTransport};
    std::uint64_t from_seq_no{0};
    std::uint32_t count{0};
};

enum class TwimeSessionEventType {
    StateChanged,
    OutboundMessage,
    InboundMessage,
    HeartbeatSent,
    HeartbeatReceived,
    MessageCounterResetDetected,
    SequenceGapDetected,
    RetransmitRequested,
    RetransmissionReceived,
    RetransmissionProtocolViolation,
    FloodRejectReceived,
    SessionRejectReceived,
    BusinessRejectReceived,
    EstablishmentRejected,
    TerminateReceived,
    KeepaliveIntervalRejected,
    HeartbeatRateViolation,
    RetransmitLimitViolation,
    ReconnectTooFast,
    PeerClosed,
    PeerClosedCleanAfterTerminateResponse,
    PeerClosedUnexpectedWhileTerminating,
    Faulted,
};

struct TwimeSessionEvent {
    TwimeSessionEventType type{TwimeSessionEventType::StateChanged};
    TwimeSessionState state{TwimeSessionState::Created};
    std::uint16_t template_id{0};
    std::uint64_t sequence_number{0};
    std::uint64_t gap_from{0};
    std::uint64_t gap_to{0};
    std::int64_t reason_code{0};
    std::string message_name;
    std::string summary;
    std::string cert_log_line;
};

struct TwimeSessionConfig {
    std::string session_id{"twime_fake_session"};
    std::string credentials{"LOGIN"};
    std::uint32_t keepalive_interval_ms{1000};
    std::size_t journal_capacity{64};
    std::size_t max_frame_size{4096};
    std::uint32_t rate_limit_window_ms{1000};
    std::uint32_t max_total_messages_per_window{1024};
    std::uint32_t max_trading_messages_per_window{1024};
    std::uint32_t max_heartbeats_per_second{3};
    bool auto_request_retransmit_on_gap{true};
    std::uint64_t initial_recovery_epoch{1};
    TwimeRecoveryMode recovery_mode{TwimeRecoveryMode::NormalSessionRecovery};
};

class TwimeFakeClock {
  public:
    explicit TwimeFakeClock(std::uint64_t now_ms = 0) noexcept : now_ms_(now_ms) {}

    [[nodiscard]] std::uint64_t now_ms() const noexcept {
        return now_ms_;
    }

    [[nodiscard]] std::uint64_t now_ns() const noexcept {
        return now_ms_ * 1'000'000ULL;
    }

    void advance(std::uint64_t milliseconds) noexcept {
        now_ms_ += milliseconds;
    }

  private:
    std::uint64_t now_ms_{0};
};

class TwimeSession {
  public:
    TwimeSession(TwimeSessionConfig config, TwimeFakeTransport& transport, TwimeRecoveryStateStore& recovery_store,
                 TwimeFakeClock& clock);
    TwimeSession(TwimeSessionConfig config, transport::ITwimeByteTransport& transport,
                 TwimeRecoveryStateStore& recovery_store, TwimeFakeClock& clock);

    [[nodiscard]] TwimeSessionState state() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> last_reject_code() const noexcept;
    [[nodiscard]] const TwimeSequenceState& sequence_state() const noexcept;
    [[nodiscard]] const TwimeRecoveryState& recovery_state() const noexcept;
    [[nodiscard]] const TwimeOutboundJournal& outbound_journal() const noexcept;
    [[nodiscard]] const TwimeInboundJournal& inbound_journal() const noexcept;
    [[nodiscard]] const std::vector<std::string>& cert_log_lines() const noexcept;
    [[nodiscard]] std::uint32_t active_keepalive_interval_ms() const noexcept;

    void apply_command(const TwimeSessionCommand& command);
    void on_timer_tick();
    void poll_transport();
    [[nodiscard]] std::vector<TwimeSessionEvent> drain_events();

  private:
    void connect_fake();
    void send_terminate();
    void send_heartbeat();
    void send_retransmit_request(std::uint64_t from_seq_no, std::uint32_t count);

    void poll_fake_transport();
    void poll_byte_transport();
    void process_transport_event(const TwimeFakeTransportEvent& event);
    void process_inbound_frame(const TwimeFakeTransportFrame& frame);
    void process_inbound_message(const moex::twime_sbe::DecodedTwimeMessage& message,
                                 const TwimeFakeTransportFrame& frame);
    void handle_peer_close(std::string peer_summary);
    bool request_missing_messages(std::uint64_t from_seq_no, std::uint64_t to_seq_no, std::string summary);
    [[nodiscard]] bool validate_keepalive_interval(std::uint64_t value);
    [[nodiscard]] bool is_recoverable_message(const moex::twime_sbe::DecodedTwimeMessage& message) const noexcept;
    [[nodiscard]] bool consumes_inbound_sequence(const moex::twime_sbe::DecodedTwimeMessage& message) const noexcept;
    [[nodiscard]] std::uint32_t max_retransmit_request_count() const noexcept;
    [[nodiscard]] bool is_reconnect_too_fast() const noexcept;
    void mark_connect_attempt() noexcept;
    void start_pending_retransmission(std::uint64_t from_seq_no, std::uint32_t count);
    void clear_pending_retransmission() noexcept;
    [[nodiscard]] bool validate_retransmission_window(std::uint64_t next_seq_no, std::uint32_t count,
                                                      const moex::twime_sbe::DecodedTwimeMessage& message,
                                                      const TwimeFakeTransportFrame& frame);

    void transition_to(TwimeSessionState new_state, std::string summary);
    void append_event(TwimeSessionEvent event);
    void append_cert_log(const std::string& direction, const std::string& formatted_message);
    void persist_recovery_state();

    bool write_outbound_bytes(std::span<const std::byte> bytes);
    bool send_message(const moex::twime_sbe::TwimeEncodeRequest& request, TwimeSessionEventType event_type,
                      std::string summary);
    [[nodiscard]] std::uint64_t next_heartbeat_due_ms() const noexcept;

    TwimeSessionConfig config_;
    TwimeFakeTransport* fake_transport_{nullptr};
    transport::ITwimeByteTransport* byte_transport_{nullptr};
    TwimeRecoveryStateStore& recovery_store_;
    TwimeFakeClock& clock_;
    moex::twime_sbe::TwimeCodec codec_;
    moex::twime_sbe::TwimeCertLogFormatter formatter_;
    moex::twime_sbe::TwimeFrameAssembler frame_assembler_;
    TwimeSequenceState sequence_state_{};
    TwimeRecoveryState recovery_state_{};
    TwimeOutboundJournal outbound_journal_;
    TwimeInboundJournal inbound_journal_;
    TwimeRateLimitModel rate_limit_model_;
    TwimeSessionState state_{TwimeSessionState::Created};

    struct TwimePendingRetransmissionWindow {
        bool active{false};
        bool metadata_received{false};
        std::uint64_t requested_from_seq{0};
        std::uint32_t requested_count{0};
        std::uint32_t received_retransmit_count{0};
        std::uint64_t expected_next_seq{0};
        std::uint64_t expected_final_next_seq{0};
    };

    std::uint32_t active_keepalive_interval_ms_{1000};
    std::uint64_t heartbeat_due_ms_{0};
    std::uint64_t last_connect_attempt_ms_{0};
    bool recovering_from_dirty_snapshot_{false};
    bool terminate_response_received_{false};
    std::optional<std::int64_t> last_reject_code_;
    TwimePendingRetransmissionWindow pending_retransmission_{};
    std::vector<TwimeSessionEvent> pending_events_;
    std::vector<std::string> cert_log_lines_;
    std::vector<std::byte> transport_read_buffer_;
};

} // namespace moex::twime_trade
