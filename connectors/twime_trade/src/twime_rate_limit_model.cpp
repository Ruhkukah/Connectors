#include "moex/twime_trade/twime_rate_limit_model.hpp"

namespace moex::twime_trade {

TwimeRateLimitModel::TwimeRateLimitModel(std::uint32_t max_total_messages_per_window,
                                         std::uint32_t max_trading_messages_per_window,
                                         std::uint32_t max_heartbeats_per_window, std::uint64_t window_ms) noexcept
    : max_total_messages_per_window_(max_total_messages_per_window == 0 ? 1 : max_total_messages_per_window),
      max_trading_messages_per_window_(max_trading_messages_per_window == 0 ? 1 : max_trading_messages_per_window),
      max_heartbeats_per_window_(max_heartbeats_per_window == 0 ? 1 : max_heartbeats_per_window),
      window_ms_(window_ms == 0 ? 1 : window_ms) {}

TwimeRateLimitDecision TwimeRateLimitModel::observe_send(std::uint64_t now_ms,
                                                         TwimeOutboundRateClassification classification) noexcept {
    if (total_used_in_window_ == 0 || now_ms >= window_start_ms_ + window_ms_) {
        window_start_ms_ = now_ms;
        total_used_in_window_ = 0;
        trading_used_in_window_ = 0;
        heartbeat_used_in_window_ = 0;
    }

    TwimeRateLimitDecision decision;
    decision.total_used_in_window = total_used_in_window_;
    decision.trading_used_in_window = trading_used_in_window_;
    decision.heartbeat_used_in_window = heartbeat_used_in_window_;

    if (classification.counts_total_rate && total_used_in_window_ >= max_total_messages_per_window_) {
        decision.allowed = false;
        decision.total_rate_violation = true;
        decision.total_used_in_window = total_used_in_window_;
        decision.trading_used_in_window = trading_used_in_window_;
        decision.heartbeat_used_in_window = heartbeat_used_in_window_;
        decision.retry_after_ms = (window_start_ms_ + window_ms_) - now_ms;
        return decision;
    }

    if (classification.counts_trading_rate && trading_used_in_window_ >= max_trading_messages_per_window_) {
        decision.allowed = false;
        decision.trading_rate_violation = true;
        decision.total_used_in_window = total_used_in_window_;
        decision.trading_used_in_window = trading_used_in_window_;
        decision.heartbeat_used_in_window = heartbeat_used_in_window_;
        decision.retry_after_ms = (window_start_ms_ + window_ms_) - now_ms;
        return decision;
    }

    if (classification.counts_heartbeat_rate && heartbeat_used_in_window_ >= max_heartbeats_per_window_) {
        decision.allowed = false;
        decision.heartbeat_rate_violation = true;
        decision.total_used_in_window = total_used_in_window_;
        decision.trading_used_in_window = trading_used_in_window_;
        decision.heartbeat_used_in_window = heartbeat_used_in_window_;
        decision.retry_after_ms = (window_start_ms_ + window_ms_) - now_ms;
        return decision;
    }

    if (classification.counts_total_rate) {
        ++total_used_in_window_;
    }
    if (classification.counts_trading_rate) {
        ++trading_used_in_window_;
    }
    if (classification.counts_heartbeat_rate) {
        ++heartbeat_used_in_window_;
    }

    decision.allowed = true;
    decision.total_used_in_window = total_used_in_window_;
    decision.trading_used_in_window = trading_used_in_window_;
    decision.heartbeat_used_in_window = heartbeat_used_in_window_;
    return decision;
}

void TwimeRateLimitModel::reset() noexcept {
    window_start_ms_ = 0;
    total_used_in_window_ = 0;
    trading_used_in_window_ = 0;
    heartbeat_used_in_window_ = 0;
}

TwimeOutboundRateClassification
classify_outbound_message(const moex::twime_sbe::TwimeMessageMetadata& metadata) noexcept {
    if (metadata.template_id == 5006) {
        return {
            .category = TwimeOutboundRateCategory::SessionHeartbeat,
            .counts_total_rate = true,
            .counts_trading_rate = false,
            .counts_heartbeat_rate = true,
        };
    }

    if (metadata.layer == moex::twime_sbe::TwimeLayer::Application &&
        metadata.direction == moex::twime_sbe::TwimeDirection::ClientToServer) {
        return {
            .category = TwimeOutboundRateCategory::TradingMessage,
            .counts_total_rate = true,
            .counts_trading_rate = true,
            .counts_heartbeat_rate = false,
        };
    }

    return {
        .category = TwimeOutboundRateCategory::SessionControl,
        .counts_total_rate = true,
        .counts_trading_rate = false,
        .counts_heartbeat_rate = false,
    };
}

} // namespace moex::twime_trade
