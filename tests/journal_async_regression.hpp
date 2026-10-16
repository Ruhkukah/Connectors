#pragma once

#include "moex/connector_host/event_journal.hpp"

#include <atomic>
#include <dlfcn.h>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// Stall the real log write in this test binary, without production fault hooks.
namespace moex::connector_host::test {
inline std::atomic<bool> intercept_writes{}, hold_write{}, writer_entered{};
inline std::atomic<bool> hold_sync{}, sync_entered{};
inline std::atomic<unsigned> owner_writes{}, owner_syncs{};
inline std::uint64_t log_device{}, log_inode{};
inline std::thread::id journal_owner;
inline bool cut_boundary_publish{};
inline bool intercepted_log(int fd) {
    if (!intercept_writes.load(std::memory_order_acquire))
        return false;
    struct stat info {};
    return ::fstat(fd, &info) == 0 && std::uint64_t(info.st_dev) == log_device &&
           std::uint64_t(info.st_ino) == log_inode;
}
} // namespace moex::connector_host::test

extern "C" int rename(const char* from, const char* to) {
    static const auto real = reinterpret_cast<int (*)(const char*, const char*)>(dlsym(RTLD_NEXT, "rename"));
    const auto result = real(from, to);
    if (moex::connector_host::test::cut_boundary_publish && result == 0 && std::string_view(to).ends_with(".journal"))
        ::_exit(0);
    return result;
}

extern "C" ssize_t write(int fd, const void* data, size_t size) {
    static const auto real = reinterpret_cast<ssize_t (*)(int, const void*, size_t)>(dlsym(RTLD_NEXT, "write"));
    using namespace moex::connector_host::test;
    if (intercepted_log(fd)) {
        if (std::this_thread::get_id() == journal_owner)
            ++owner_writes;
        writer_entered = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (hold_write && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return real(fd, data, size);
}

extern "C" int fsync(int fd) {
    static const auto real = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "fsync"));
    using namespace moex::connector_host::test;
    if (intercepted_log(fd)) {
        if (std::this_thread::get_id() == journal_owner)
            ++owner_syncs;
        sync_entered = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (hold_sync && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return real(fd);
}

namespace moex::connector_host::test {
inline void journal_constructor_cut_regression(const std::filesystem::path& root) {
    const auto log = root / "constructor-cut.ndjson", state = root / "constructor-cut.state";
    std::ofstream(log) << "{\"event\":\"reservation\",\"data\":{\"next_ext_id\":42,\"next_user_id\":72}}\n";
    const auto child = ::fork();
    if (child < 0)
        throw std::runtime_error("fork constructor checkpoint cut");
    if (child == 0) {
        cut_boundary_publish = true;
        EventJournal journal(log, state);
        ::_exit(2);
    }
    int status{};
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || !std::filesystem::exists(state))
        throw std::runtime_error("journal boundary published before recovered ID ceiling");
    EventJournal recovered(log, state);
    if (recovered.reservations().next_ext_id != 1042 || recovered.reservations().next_user_id != 1072 ||
        recovered.recovery_read_bytes() != 0)
        throw std::runtime_error("constructor crash reused recovered IDs or skipped their durable ceiling");
}
inline void journal_missing_identity_regression(const std::filesystem::path& root) {
    const auto log = root / "missing-identity.ndjson", state = root / "missing-identity.state";
    {
        EventJournal journal(log, state);
        journal.append("reservation", "{\"next_ext_id\":42,\"next_user_id\":72}");
        journal.flush();
    }
    std::filesystem::remove(state);
    bool refused{};
    try {
        EventJournal unsafe(log, state);
    } catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find("identity checkpoint missing") != std::string::npos;
    }
    if (!refused || std::filesystem::exists(state))
        throw std::runtime_error("boundary sidecar skipped unrecoverable IDs after identity state loss");
}
inline void journal_async_regression(const std::filesystem::path& root) {
    const auto check = [](bool condition, const char* text) {
        if (!condition)
            throw std::runtime_error(text);
    };
    const auto wait = [&](auto ready, const char* text) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!ready() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(ready(), text);
    };
    const auto path = root / "async.ndjson";
    std::size_t accepted{};
    {
        EventJournal journal(path, {}, 4096);
        struct Reset {
            ~Reset() {
                hold_write = false;
                hold_sync = false;
                intercept_writes = false;
            }
        } reset;
        struct stat info {};
        check(::stat(path.c_str(), &info) == 0, "stat asynchronous journal");
        log_device = info.st_dev;
        log_inode = info.st_ino;
        journal_owner = std::this_thread::get_id();
        owner_writes = 0;
        owner_syncs = 0;
        writer_entered = false;
        hold_write = true;
        intercept_writes.store(true, std::memory_order_release);
        const auto fields = "{\"message\":\"" + std::string(800, 'x') + "\"}";
        const auto start = std::chrono::steady_clock::now();
        journal.append("first", fields);
        ++accepted;
        check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50),
              "owner waited for the stalled journal write");
        wait([] { return writer_entered.load(); }, "writer never entered stalled log write");
        bool overflow{};
        const auto enqueue_start = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; ++i) {
            try {
                journal.append("queued", fields);
                ++accepted;
            } catch (const std::runtime_error& error) {
                overflow = std::string(error.what()).find("queue capacity exceeded") != std::string::npos;
                break;
            }
        }
        check(std::chrono::steady_clock::now() - enqueue_start < std::chrono::milliseconds(50),
              "enqueue or overflow handling waited behind writer I/O");
        check(overflow && accepted > 1 && accepted < 10, "queue did not bound queued and in-flight records");
        bool failed_health{};
        try {
            journal.flush_if_due();
        } catch (const std::runtime_error&) {
            failed_health = true;
        }
        check(failed_health, "queue overflow was hidden from storage protection");
        journal.request_recovery();
        check(journal.recovery_pending(), "held writer recovery completed without draining");
        hold_write = false;
        wait([&] { return !journal.recovery_pending(); }, "asynchronous recovery did not finish");
        journal.flush_if_due();
        journal.append("recovered");
        ++accepted;
        journal.flush();
        // Hold the group fsync while extending the independent durable ID
        // ceiling; its newer value must survive the writer's checkpoint.
        hold_sync = true;
        sync_entered = false;
        journal.append("before_sync");
        ++accepted;
        journal.request_recovery();
        wait([] { return sync_entered.load(); }, "writer never entered stalled group fsync");
        const auto sync_start = std::chrono::steady_clock::now();
        journal.append("while_sync");
        ++accepted;
        journal.flush_if_due();
        check(std::chrono::steady_clock::now() - sync_start < std::chrono::milliseconds(50),
              "ordinary append or health check waited behind group fsync");
        journal.append("reservation", "{\"next_ext_id\":1002,\"next_user_id\":1002}");
        ++accepted;
        hold_write = true;
        writer_entered = false;
        hold_sync = false;
        wait([] { return writer_entered.load(); }, "recovery did not drain records queued during fsync");
        check(journal.recovery_pending(), "recovery cleared protection before queued records were durable");
        hold_write = false;
        wait([&] { return !journal.recovery_pending(); }, "held fsync recovery did not finish");
        journal.flush();
        check(owner_writes == 0 && owner_syncs == 0, "ordinary log writes or fsync still ran on the owner thread");
        // Destructor must drain the final queued record even without flush.
        journal.append("shutdown_tail");
        ++accepted;
    }
    std::ifstream input(path);
    std::string line;
    std::size_t lines{};
    while (std::getline(input, line))
        ++lines;
    check(lines == accepted, "overflow, recovery or shutdown dropped/duplicated an accepted record");
    EventJournal restarted(path);
    check(restarted.recovery_read_bytes() == 0, "writer boundary did not avoid a full restart scan");
    check(restarted.reservations().next_ext_id == 2002 && restarted.reservations().next_user_id == 2002,
          "writer checkpoint replaced a concurrently extended durable ID block");
}
} // namespace moex::connector_host::test
