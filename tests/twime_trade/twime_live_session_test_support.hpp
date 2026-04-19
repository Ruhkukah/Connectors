#pragma once

#include "moex/twime_trade/twime_live_session_runner.hpp"

#include "twime_session_tcp_test_support.hpp"

#include <cstdlib>
#include <filesystem>

namespace moex::twime_trade::test {

struct ScopedEnvVar {
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        const char* existing = std::getenv(name_);
        if (existing != nullptr) {
            had_old_value_ = true;
            old_value_ = existing;
        }

        if (value == nullptr) {
            ::unsetenv(name_);
        } else {
            ::setenv(name_, value, 1);
        }
    }

    ~ScopedEnvVar() {
        if (had_old_value_) {
            ::setenv(name_, old_value_.c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }

  private:
    const char* name_;
    bool had_old_value_{false};
    std::string old_value_;
};

struct ManualRunnerClock {
    using TimePoint = std::chrono::steady_clock::time_point;

    TimePoint now{std::chrono::steady_clock::now()};

    [[nodiscard]] TimePoint operator()() const {
        return now;
    }

    void advance(std::uint64_t milliseconds) {
        now += std::chrono::milliseconds(milliseconds);
    }
};

inline TwimeLiveSessionConfig
make_live_session_config(std::uint16_t port, std::string session_id = "twime_live_test",
                         std::string credentials_env_var = "MOEX_TWIME_TEST_CREDENTIALS") {
    TwimeLiveSessionConfig config;
    config.session.session_id = std::move(session_id);
    config.session.keepalive_interval_ms = 1000;
    config.tcp = make_local_tcp_config(port);
    config.credentials.source = transport::TwimeCredentialSource::Env;
    config.credentials.env_var = std::move(credentials_env_var);
    config.policy.reconnect_enabled = false;
    config.policy.max_reconnect_attempts = 3;
    config.policy.establish_deadline_ms = 10000;
    config.policy.graceful_terminate_timeout_ms = 3000;
    return config;
}

inline void pump_runner_until(TwimeLiveSessionRunner& runner, ManualRunnerClock& clock,
                              const std::function<bool(const TwimeLiveSessionRunner&)>& predicate, int max_polls = 256,
                              std::uint64_t step_ms = 25) {
    for (int attempt = 0; attempt < max_polls; ++attempt) {
        if (predicate(runner)) {
            return;
        }
        const auto result = runner.poll_once();
        moex::twime_sbe::test::require(result.ok, "live session runner poll failed unexpectedly");
        clock.advance(step_ms);
    }
    moex::twime_sbe::test::require(false, "live session runner did not reach expected state");
}

inline std::filesystem::path temp_state_dir(std::string_view name) {
    const auto path = std::filesystem::path(MOEX_SOURCE_ROOT) / "build" / "test-output" / std::string(name);
    std::filesystem::create_directories(path);
    return path;
}

} // namespace moex::twime_trade::test
