#pragma once
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include <optional>

namespace moex::plaza2::cgate {
struct FullOrderLogOpenInterestValue {
    std::int32_t isin{}, session{};
    std::int64_t quantity{}, revision{}, row_id{}, deal_id{};
    std::uint64_t moment_ns{};
};
// Restricted FORTS_DEALS_REPL.deal consumer. No subscriptions or own-order logic here.
class FullOrderLogOpenInterest final : public Plaza2ListenerEventHandler {
  public:
    explicit FullOrderLogOpenInterest(std::span<const std::int32_t> isins);
    ~FullOrderLogOpenInterest();
    bool wants_raw_replication(std::string_view) const noexcept override {
        return true;
    }
    bool should_log_listener_event(const Plaza2ListenerEvent& event) const noexcept override;
    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent&) override;
    void on_plaza2_listener_error(const Plaza2Error&) noexcept override;
    bool online() const noexcept;
    std::optional<FullOrderLogOpenInterestValue> value(std::int32_t isin) const;
    std::function<Plaza2Error(const Plaza2ListenerEvent&)> before_event;
    std::function<void(const Plaza2Error&)> before_error;
    std::function<void(const FullOrderLogOpenInterestValue&)> on_update;
    std::function<void()> on_invalidate;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace moex::plaza2::cgate
