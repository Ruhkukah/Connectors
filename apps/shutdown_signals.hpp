#pragma once

#include <atomic>
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#ifdef __linux__
#include <sys/signalfd.h>
#else
#include <thread>
#endif

namespace moex::connector_host {

// Construct on the owner thread before the host/runtime or any worker exists.
// Every subsequently created thread inherits the blocked shutdown signals, so
// SIGINT/SIGTERM cannot interrupt CGate's process call or one of its workers.
class ShutdownSignals {
  public:
    ShutdownSignals() {
        sigemptyset(&mask_);
        sigaddset(&mask_, SIGINT);
        sigaddset(&mask_, SIGTERM);
        if (const auto error = ::pthread_sigmask(SIG_BLOCK, &mask_, &previous_))
            throw std::system_error(error, std::generic_category(), "cannot block shutdown signals");
        try {
#ifdef __linux__
            fd_ = ::signalfd(-1, &mask_, SFD_NONBLOCK | SFD_CLOEXEC);
            if (fd_ < 0)
                throw std::system_error(errno, std::generic_category(), "cannot open shutdown signal descriptor");
#else
            waiter_ = std::thread([this] {
                for (;;) {
                    int signal{};
                    if (::sigwait(&mask_, &signal) != 0)
                        std::terminate();
                    if (closing_.load(std::memory_order_acquire))
                        return;
                    record(signal);
                }
            });
#endif
        } catch (...) {
            ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
            throw;
        }
    }
    ~ShutdownSignals() {
#ifdef __linux__
        ::close(fd_);
#else
        closing_.store(true, std::memory_order_release);
        // Wake only our sigwait thread; this does not run a signal handler.
        ::pthread_kill(waiter_.native_handle(), SIGTERM);
        waiter_.join();
#endif
        // Executable main owns this object until all host/runtime workers are
        // stopped. Keep the shutdown mask in force through process exit: a
        // late duplicate signal must not bypass the recorded drain result.
    }
    ShutdownSignals(const ShutdownSignals&) = delete;
    ShutdownSignals& operator=(const ShutdownSignals&) = delete;

    void poll() {
#ifdef __linux__
        signalfd_siginfo signal{};
        for (;;) {
            const auto count = ::read(fd_, &signal, sizeof(signal));
            if (count == static_cast<ssize_t>(sizeof(signal))) {
                record(static_cast<int>(signal.ssi_signo));
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            throw std::runtime_error("cannot read shutdown signal descriptor");
        }
#endif
    }
    [[nodiscard]] int first() const noexcept {
        return first_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool second() const noexcept {
        return count_.load(std::memory_order_acquire) > 1;
    }
    [[nodiscard]] bool requested() {
        poll();
        return first() != 0;
    }

  private:
    void record(int signal) noexcept {
        int empty{};
        first_.compare_exchange_strong(empty, signal, std::memory_order_release);
        count_.fetch_add(1, std::memory_order_release);
    }
    sigset_t mask_{}, previous_{};
    std::atomic_int first_{}, count_{};
#ifdef __linux__
    int fd_{-1};
#else
    std::atomic_bool closing_{};
    std::thread waiter_;
#endif
};

} // namespace moex::connector_host
