#pragma once

#include <functional>
#include <utility>

namespace moex::connector_host {
class ScopeExit {
  public:
    explicit ScopeExit(std::function<void()> cleanup) : cleanup_(std::move(cleanup)) {}
    ~ScopeExit() noexcept {
        try {
            if (cleanup_)
                cleanup_();
        } catch (...) {
        }
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    void release() noexcept {
        cleanup_ = nullptr;
    }

  private:
    std::function<void()> cleanup_;
};
} // namespace moex::connector_host
