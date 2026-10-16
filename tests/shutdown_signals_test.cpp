#include "shutdown_signals.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <poll.h>
#include <thread>

namespace {
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void wait_for(moex::connector_host::ShutdownSignals& signals, bool second = false) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    do {
        signals.poll();
        if (second ? signals.second() : signals.first() != 0)
            return;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("blocked shutdown signal was not consumed");
}
} // namespace

int main() {
    try {
        // Use real process-directed signals while the owner and a newly
        // created worker are in non-restarting poll(2). A handler-based
        // implementation interrupts these syscalls even with SA_RESTART.
        for (unsigned repetition = 0; repetition < 20; ++repetition) {
            moex::connector_host::ShutdownSignals signals;
            sigset_t owner_mask{};
            require(::pthread_sigmask(SIG_SETMASK, nullptr, &owner_mask) == 0 && sigismember(&owner_mask, SIGINT) &&
                        sigismember(&owner_mask, SIGTERM),
                    "owner shutdown mask was not installed");
            std::promise<bool> worker_mask;
            std::promise<int> worker_poll;
            std::thread worker([&] {
                sigset_t inherited{};
                const bool masked = ::pthread_sigmask(SIG_SETMASK, nullptr, &inherited) == 0 &&
                                    sigismember(&inherited, SIGINT) && sigismember(&inherited, SIGTERM);
                worker_mask.set_value(masked);
                worker_poll.set_value(::poll(nullptr, 0, 100));
            });
            const bool inherited = worker_mask.get_future().get();
            std::thread sender([] {
                std::this_thread::sleep_for(20ms);
                ::kill(::getpid(), SIGTERM);
            });
            const auto start = std::chrono::steady_clock::now();
            const auto polled = ::poll(nullptr, 0, 100);
            sender.join();
            worker.join();
            require(inherited && polled == 0 && worker_poll.get_future().get() == 0,
                    "SIGTERM interrupted owner or inherited worker poll");
            require(std::chrono::steady_clock::now() - start >= 80ms, "owner poll returned early on SIGTERM");
            wait_for(signals);
            require(signals.first() == SIGTERM && !signals.second(), "first shutdown signal was misrecorded");
            require(::kill(::getpid(), SIGINT) == 0, "second shutdown signal delivery failed");
            wait_for(signals, true);
            require(signals.first() == SIGTERM, "second signal overwrote the first reason");
        }
        std::cout << "20 real blocked-signal owner/worker poll cases PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
