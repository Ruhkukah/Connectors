#pragma once

#include "moex/connector_host/event_journal.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace moex::connector_host::test {
inline void journal_id_block_regression(const std::filesystem::path& root) {
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    struct Checkpoint {
        std::uint64_t offset{}, ext{}, user{};
    };
    const auto checkpoint = [&](const std::filesystem::path& path) {
        std::ifstream input(path);
        std::string magic;
        std::uint64_t device{}, inode{}, boundary{}, checksum{};
        Checkpoint saved;
        check(bool(input >> magic >> device >> inode >> saved.offset >> saved.ext >> saved.user >> boundary >>
                   checksum) &&
                  magic == "MOEXJ2",
              "read block reservation checkpoint");
        return saved;
    };
    const auto file_text = [](const std::filesystem::path& path) {
        std::ifstream input(path);
        std::ostringstream text;
        text << input.rdbuf();
        return text.str();
    };
    const auto crash_child = [&](auto exercise) {
        const auto child = ::fork();
        check(child >= 0, "fork block reservation regression");
        if (child == 0) {
            try {
                exercise();
                ::_exit(0);
            } catch (const std::exception& error) {
                std::cerr << "journal ID block regression: " << error.what() << '\n';
                ::_exit(1);
            }
        }
        int status{};
        pid_t waited;
        do {
            waited = ::waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "block reservation child failed");
    };
    const auto directory = root / "ID-blocks";
    std::filesystem::create_directories(directory);
    const auto path = directory / "crash.ndjson", state = directory / "crash.state";
    crash_child([&] {
        EventJournal journal(path, state);
        const auto first = checkpoint(state);
        check(first.ext == 1001 && first.user == 1001 && first.offset == 0,
              "startup did not durably reserve the first 1000 IDs before commands");
        check(journal.reservations().next_ext_id == 1 && journal.reservations().next_user_id == 1,
              "startup handed out the block high-water mark instead of its first ID");
        const auto saved = file_text(state);
        struct stat before {
        }, after{};
        check(::stat(state.c_str(), &before) == 0, "stat startup block checkpoint");
        for (int i = 2; i < 102; ++i)
            journal.append("reservation", "{\"next_ext_id\":" + std::to_string(i) +
                                              ",\"next_user_id\":" + std::to_string(i + 50) + "}");
        std::this_thread::sleep_for(std::chrono::milliseconds(270));
        journal.append("reservation", "{\"next_ext_id\":102,\"next_user_id\":152}");
        check(::stat(state.c_str(), &after) == 0 && before.st_ino == after.st_ino && file_text(state) == saved &&
                  std::filesystem::file_size(path) == 0,
              "within-block reservation performed a checkpoint or time-triggered journal write");
        // _exit deliberately bypasses the destructor and any group flush.
        ::_exit(0);
    });
    check(std::filesystem::file_size(path) == 0, "crash fixture ran a journal destructor flush");
    {
        EventJournal recovered(path, state);
        check(recovered.reservations().next_ext_id == 1001 && recovered.reservations().next_user_id == 1001,
              "crash reused an ID from an unflushed reservation block");
        check(checkpoint(state).ext == 2001 && checkpoint(state).user == 2001,
              "restart did not reserve its new block before commands");
    }
    const auto rollover_path = directory / "rollover.ndjson", rollover_state = directory / "rollover.state";
    crash_child([&] {
        EventJournal journal(rollover_path, rollover_state);
        journal.append("large_record", "{\"message\":\"" + std::string(65536, 'x') + "\"}");
        check(std::filesystem::file_size(rollover_path) > 0,
              "rollover fixture did not write an unsynced journal prefix");
        journal.append("reservation", "{\"next_ext_id\":1002,\"next_user_id\":2}");
        const auto saved = checkpoint(rollover_state);
        check(saved.ext == 2002 && saved.user == 1001 && saved.offset == 0,
              "block rollover checkpointed an unsynced journal prefix or failed to reserve the next block");
        journal.append("reservation", "{\"next_ext_id\":1002,\"next_user_id\":1002}");
        const auto user_rollover = checkpoint(rollover_state);
        check(user_rollover.ext == 2002 && user_rollover.user == 2002 && user_rollover.offset == 0,
              "user-ID rollover lost the existing ext-ID block or changed its durable journal boundary");
        ::_exit(0);
    });
    // Emulate loss of the unsynced prefix: the durable checkpoint must still
    // describe a valid boundary and preserve the newly reserved ID block.
    std::filesystem::resize_file(rollover_path, 0);
    {
        EventJournal recovered(rollover_path, rollover_state);
        check(recovered.reservations().next_ext_id == 2002 && recovered.reservations().next_user_id == 2002,
              "rollover crash reused reserved IDs or depended on unsynced journal bytes");
    }
    const auto failed_path = directory / "failed.ndjson", failed_state = directory / "failed.state";
    crash_child([&] {
        EventJournal journal(failed_path, failed_state);
        journal.append("reservation", "{\"next_ext_id\":2,\"next_user_id\":2}");
        const auto backup = directory / "saved.state";
        std::filesystem::rename(failed_state, backup);
        std::filesystem::create_directory(failed_state);
        bool refused{};
        try {
            journal.append("reservation", "{\"next_ext_id\":1002,\"next_user_id\":1002}");
        } catch (const std::runtime_error&) {
            refused = true;
        }
        check(refused && journal.reservations().next_ext_id == 2 && journal.reservations().next_user_id == 2 &&
                  std::filesystem::file_size(failed_path) == 0,
              "failed block extension handed out or recorded an unreserved ID");
        check(!journal.user_id_reserved(0) && journal.user_id_reserved(1000) && !journal.user_id_reserved(1001),
              "failed extension changed the durable cancellation ceiling");
        std::filesystem::remove(failed_state);
        std::filesystem::rename(backup, failed_state);
        ::_exit(0);
    });
    EventJournal recovered(failed_path, failed_state);
    check(recovered.reservations().next_ext_id == 1001 && recovered.reservations().next_user_id == 1001,
          "failed extension corrupted the previous durable block");
}
} // namespace moex::connector_host::test
