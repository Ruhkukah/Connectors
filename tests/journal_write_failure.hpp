#pragma once

#include "moex/connector_host/event_journal.hpp"

#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace moex::connector_host::test {
inline void journal_write_failure_regression(const std::filesystem::path& root) {
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    const auto directory = root / "partial-journal-write";
    std::filesystem::create_directories(directory);
    const auto path = directory / "events.ndjson";
    const auto state = directory / "identity.state";
    const std::string fields =
        "{\"next_ext_id\":42,\"next_user_id\":72,\"message\":\"" + std::string(16384, 'x') + "\"}";
    const auto child = ::fork();
    check(child >= 0, "fork partial journal write regression");
    if (child == 0) {
        try {
            EventJournal journal(path, state);
            rlimit original{};
            check(::getrlimit(RLIMIT_FSIZE, &original) == 0, "read child file-size limit");
            check(original.rlim_cur == RLIM_INFINITY || original.rlim_cur > fields.size() + 512,
                  "existing file-size limit cannot accommodate journal retry");
            check(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR, "ignore child file-size signal");
            auto limited = original;
            limited.rlim_cur = 4096;
            check(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "set child file-size limit");
            bool write_failed{};
            try {
                journal.append("reservation", fields);
                journal.flush();
            } catch (const std::runtime_error&) {
                write_failed = true;
            }
            check(write_failed && std::filesystem::file_size(path) == limited.rlim_cur,
                  "journal did not exercise a partial write followed by file-size failure");
            check(::setrlimit(RLIMIT_FSIZE, &original) == 0, "lift child file-size limit");
            journal.flush();
            std::ifstream checkpoint(state.string() + ".journal");
            std::string magic;
            std::uint64_t device{}, inode{}, offset{}, ext{}, user{}, boundary{}, checksum{};
            check(static_cast<bool>(checkpoint >> magic >> device >> inode >> offset >> ext >> user >> boundary >>
                                    checksum) &&
                      magic == "MOEXJ2" && offset == std::filesystem::file_size(path),
                  "retry did not immediately checkpoint the complete bytes");
            std::ifstream ids(state);
            check(static_cast<bool>(ids >> magic >> device >> inode >> offset >> ext >> user >> boundary >> checksum) &&
                      ext == 1001 && user == 1001,
                  "writer retry replaced the owner's reserved identifiers");
            // Bypass all destructors: restart must depend on the explicit
            // group flush, rather than a successful destructor retry.
            ::_exit(0);
        } catch (const std::exception& error) {
            std::cerr << "partial journal write regression: " << error.what() << '\n';
            ::_exit(1);
        }
    }
    int status{};
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "partial journal write child failed");
    std::ifstream input(path);
    std::string line, extra;
    check(static_cast<bool>(std::getline(input, line)) && !std::getline(input, extra),
          "retry dropped or duplicated an NDJSON record");
    const auto event = line.find(",\"event\":");
    check(line.starts_with("{\"utc\":") && line.find("{\"utc\":", 1) == std::string::npos &&
              event != std::string::npos &&
              line.substr(event) == ",\"event\":\"reservation\",\"data\":" + fields + "}" &&
              std::filesystem::file_size(path) == line.size() + 1,
          "retry duplicated a written prefix or lost the original record suffix");
    EventJournal recovered(path, state);
    check(recovered.reservations().next_ext_id == 1001 && recovered.reservations().next_user_id == 1001 &&
              recovered.recovery_read_bytes() == 0,
          "explicit retry flush did not survive restart without tail repair");
}
} // namespace moex::connector_host::test
