#pragma once
#include "moex/plaza2_trade/cgate_session.hpp"
#include "fake_cgate_control.hpp"
#include "plaza2_runtime_test_support.hpp"
#include <dlfcn.h>

namespace moex::plaza2::test {
inline void publisher_prewarm_regression(plaza2_trade::CgateSessionConfig config, const fake::Control& control,
                                         const std::filesystem::path& library) {
    using plaza2_trade::CgateSession;
    auto now = std::chrono::steady_clock::time_point(std::chrono::hours(1));
    config.recovery_now = [&] { return now; };
    config.process_timeout_ms = 0;
    config.event_log = {};
    config.listener_event_log = {};
    void* dso = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(dso, "load prewarm native counters");
    const auto count = reinterpret_cast<std::uint64_t (*)(std::uint32_t)>(::dlsym(dso, "moex_fake_publisher_count"));
    require(count, "load prewarm allocation/free counter");
    control.configure({});
    const auto frees_before = count(2);
    const auto commands_before = control.commands().size();
    {
        CgateSession session(config);
        require(!session.start(), "publisher prewarm start");
        for (int i = 0; i < 20; ++i)
            require(!session.poll(false), "publisher prewarm bootstrap");
        const auto warm = session.publisher_call_counts();
        require(session.publisher_prepared() && session.runtime_health().publisher == 3 && warm.msgnew == 1 &&
                    warm.post == 0 && count(2) == frees_before + 1 && control.commands().size() == commands_before,
                "publisher startup did not allocate/free exactly one warm-up message without posting");
        for (int i = 0; i < 20; ++i)
            require(!session.poll(false), "publisher prewarm stable poll");
        require(session.publisher_call_counts().msgnew == 1 && count(2) == frees_before + 1,
                "active publisher repeatedly prewarmed on every owner poll");
        control.set(fake::Option::PublisherClosed, "1");
        require(!session.poll(false), "publisher close observation");
        control.clear(fake::Option::PublisherClosed);
        now += std::chrono::seconds(2);
        for (int i = 0; i < 10; ++i)
            require(!session.poll(false), "publisher rewarm poll");
        require(session.publisher_call_counts().msgnew == 2 && session.publisher_call_counts().post == 0 &&
                    count(2) == frees_before + 2 && control.commands().size() == commands_before,
                "publisher reopen did not repeat its allocate/free-only warm-up");
        require(!session.stop(), "publisher prewarm stop");
    }
    control.set(fake::Option::PubMsgnewResult, "internal");
    {
        CgateSession failed(config);
        require(!failed.start(), "failed prewarm start");
        for (int i = 0; i < 5; ++i)
            require(!failed.poll(false), "failed prewarm bootstrap");
        require(!failed.publisher_prepared() && failed.publisher_call_counts().post == 0 &&
                    control.commands().size() == commands_before,
                "failed publisher warm-up allowed order entry or an exchange post");
        control.clear(fake::Option::PubMsgnewResult);
        now += std::chrono::seconds(2);
        for (int i = 0; i < 10; ++i)
            require(!failed.poll(false), "failed prewarm recovery");
        require(failed.publisher_prepared() && failed.publisher_call_counts().post == 0,
                "publisher did not recover its warm-up without posting");
        require(!failed.stop(), "failed prewarm stop");
    }
    config.read_only_market_data = true;
    config.allow_orders = false;
    config.publisher_settings.clear();
    config.p2mqreply_settings.clear();
    const auto read_only_frees = count(2);
    {
        CgateSession session(config);
        require(!session.start(), "read-only no-prewarm start");
        for (int i = 0; i < 20; ++i)
            require(!session.poll(false), "read-only no-prewarm bootstrap");
        require(!session.publisher_prepared() && session.publisher_call_counts().msgnew == 0 &&
                    session.publisher_call_counts().post == 0 && count(2) == read_only_frees &&
                    control.commands().size() == commands_before,
                "read-only session created/allocated/freed a publisher message");
        require(!session.stop(), "read-only no-prewarm stop");
    }
    ::dlclose(dso);
    control.configure({});
}
} // namespace moex::plaza2::test
