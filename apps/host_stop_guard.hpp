#pragma once

#include <functional>
#include <utility>

namespace moex::connector_host {
template <class Host> class HostStopGuard {
  public:
    using Error = decltype(std::declval<Host&>().stop());
    explicit HostStopGuard(Host& host, std::function<void()> report = {}) : host_(host), report_(std::move(report)) {}
    ~HostStopGuard() noexcept {
        try {
            (void)stop();
        } catch (...) {
        }
    }
    HostStopGuard(const HostStopGuard&) = delete;
    HostStopGuard& operator=(const HostStopGuard&) = delete;
    [[nodiscard]] Error stop() {
        if (stopped_)
            return result_;
        stopped_ = true;
        try {
            result_ = host_.stop();
        } catch (...) {
            report();
            throw;
        }
        report();
        return result_;
    }

  private:
    void report() noexcept {
        try {
            if (report_)
                report_();
        } catch (...) {
        }
    }
    Host& host_;
    std::function<void()> report_;
    Error result_{};
    bool stopped_{};
};
} // namespace moex::connector_host
