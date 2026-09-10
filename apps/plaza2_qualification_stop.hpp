#pragma once

#include <csignal>
#include <pthread.h>
#include <stdexcept>

// Qualification process only. Construct before vendor resources/threads. The
// mask deliberately stays blocked through teardown and process exit, including
// exception unwinding. Never deliver a pending stop inside a vendor syscall.
class Plaza2QualificationStop {
  public:
    Plaza2QualificationStop() {
        sigemptyset(&signals_);
        sigaddset(&signals_, SIGTERM);
        sigaddset(&signals_, SIGINT);
        if (pthread_sigmask(SIG_BLOCK, &signals_, nullptr) != 0)
            throw std::runtime_error("cannot block qualification stop signals");
    }
    bool requested() const {
        sigset_t pending;
        if (sigpending(&pending) != 0)
            throw std::runtime_error("cannot inspect qualification stop signals");
        if (!sigismember(&pending, SIGTERM) && !sigismember(&pending, SIGINT))
            return false;
        int signal{};
        if (sigwait(&signals_, &signal) != 0)
            throw std::runtime_error("cannot consume qualification stop signal");
        return true;
    }

  private:
    sigset_t signals_{};
};
