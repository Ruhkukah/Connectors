#include "../../apps/plaza2_qualification_stop.hpp"

#include <chrono>
#include <iostream>
#include <poll.h>
#include <string_view>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2)
        return 10;
    Plaza2QualificationStop stop;
    // Vendor-created threads must inherit the mask, not receive process signals.
    bool inherited{};
    std::thread vendor([&] {
        sigset_t mask;
        pthread_sigmask(SIG_SETMASK, nullptr, &mask);
        inherited = sigismember(&mask, SIGINT) && sigismember(&mask, SIGTERM);
    });
    vendor.join();
    if (!inherited)
        return 11;
    const std::string_view scenario(argv[1]);
    unsigned polls{}, callbacks{}, commits{}, teardown{};
    bool failed{}, observed{};
    while (!failed && !(observed = stop.requested())) {
        if (++polls > 1)
            return 12; // A stop must be consumed before another vendor poll.
        if (scenario == "commit")
            ++commits;
        std::cout << "ENTER " << scenario << std::endl;
        const auto start = std::chrono::steady_clock::now();
        if (scenario == "heavy") {
            // A long callback inside this owner poll; stop cannot tear down mid-callback.
            while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300))
                ++callbacks;
        }
        // A real blocking syscall, mirroring the vendor poll that failed on T1.
        // The parent sends its signal after ENTER, while this call is active.
        if (poll(nullptr, 0, 300) != 0)
            return 13; // EINTR is a failure, never converted to a timeout.
        if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(290))
            return 14;
        failed = scenario == "internal"; // Independent non-timeout process failure stays fatal.
    }
    ++teardown; // Same owner, after the simulated vendor call has returned.
    sigset_t mask;
    pthread_sigmask(SIG_SETMASK, nullptr, &mask);
    if (!sigismember(&mask, SIGTERM) || !sigismember(&mask, SIGINT))
        return 15;
    std::cout << "{\"polls\":" << polls << ",\"callbacks\":" << callbacks << ",\"commits\":" << commits
              << ",\"teardown\":" << teardown << ",\"stop_observed\":" << observed << ",\"failed\":" << failed
              << ",\"recovery_attempts\":0,\"publisher_posts\":0}\n";
    return failed ? 3 : 0;
}
