#include "plaza2_runtime_test_support.hpp"
#include "command_socket.hpp"
#include "scope_exit.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
namespace test = moex::plaza2::test;
namespace host = moex::connector_host;
namespace fs = std::filesystem;

std::string contents(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}
class Process {
  public:
    Process(const std::vector<std::string>& arguments, const fs::path& label) : label_(label) {
        const auto output = ::open((label.string() + ".out").c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        const auto errors = ::open((label.string() + ".err").c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        const auto input = ::open("/dev/null", O_RDONLY);
        test::require(output >= 0 && errors >= 0 && input >= 0, "cannot open child fixture streams");
        pid_ = ::fork();
        test::require(pid_ >= 0, "cannot fork fake FOL executable");
        if (!pid_) {
            std::signal(SIGPIPE, SIG_DFL);
            std::signal(SIGHUP, SIG_DFL);
            ::dup2(input, STDIN_FILENO);
            ::dup2(output, STDOUT_FILENO);
            ::dup2(errors, STDERR_FILENO);
            for (const auto fd : {input, output, errors})
                if (fd > STDERR_FILENO)
                    ::close(fd);
            std::vector<char*> argv;
            for (const auto& argument : arguments)
                argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);
            ::execv(argv.front(), argv.data());
            ::_exit(127);
        }
        for (const auto fd : {input, output, errors})
            ::close(fd);
    }
    ~Process() {
        if (alive()) {
            ::kill(pid_, SIGTERM);
            const auto end = std::chrono::steady_clock::now() + 3s;
            while (alive() && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(10ms);
            if (alive()) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status_, 0);
            }
        }
    }
    bool alive() {
        if (!reaped_ && ::waitpid(pid_, &status_, WNOHANG) == pid_)
            reaped_ = true;
        return !reaped_;
    }
    int wait() {
        const auto end = std::chrono::steady_clock::now() + 15s;
        while (alive() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(10ms);
        test::require(!alive(), "fake FOL executable did not exit");
        return WIFEXITED(status_) ? WEXITSTATUS(status_) : 128 + WTERMSIG(status_);
    }
    std::string output() const {
        return contents(label_.string() + ".out");
    }
    std::string errors() const {
        return contents(label_.string() + ".err");
    }

  private:
    fs::path label_;
    pid_t pid_{};
    int status_{};
    bool reaped_{};
};

std::vector<std::string> arguments(const std::string& executable, const test::RuntimeFixturePaths& fixture,
                                   const fs::path& log, std::string_view instance) {
    return {executable,
            "plaza2",
            "run",
            "--offline-fake",
            "--environment",
            "test",
            "--runtime-root",
            fixture.root.string(),
            "--scheme-dir",
            fixture.scheme_dir.string(),
            "--config-dir",
            fixture.config_dir.string(),
            "--library-path",
            fixture.library_path.string(),
            "--env-settings-var",
            "MOEX_FOL_RUNNER_SETTINGS",
            "--credentials-env",
            "MOEX_FOL_RUNNER_CREDENTIALS",
            "--software-key-env",
            "MOEX_FOL_RUNNER_KEY",
            "--broker-code-env",
            "MOEX_FOL_RUNNER_BROKER",
            "--client-code-env",
            "MOEX_FOL_RUNNER_CLIENT",
            "--login-env",
            "MOEX_FOL_RUNNER_LOGIN",
            "--ext-id-range",
            "100:109",
            "--max-commands-per-second",
            "5",
            "--isin-id",
            "1001",
            "--instance-id",
            std::string(instance),
            "--max-quantity",
            "2",
            "--max-open-orders",
            "5",
            "--max-notional",
            "1001=1000000",
            "--max-position",
            "1001=2",
            "--clock-offset-us",
            "0",
            "--log",
            log.string(),
            "--allow-orders"};
}
void erase_option(std::vector<std::string>& argv, std::string_view option) {
    const auto found = std::ranges::find(argv, option);
    test::require(found != argv.end() && found + 1 != argv.end(), "missing option in test fixture");
    argv.erase(found, found + 2);
}
std::string command(const std::string& executable, const fs::path& log, const std::string& line,
                    const fs::path& label) {
    Process client({executable, "plaza2", "cmd", "--offline-fake", "--log", log.string(), line}, label);
    const auto exit = client.wait();
    const auto response = client.output();
    test::require(exit == (response.find("\"ok\":false") == std::string::npos ? 0 : 2),
                  "actual FOL command client returned wrong status: " + client.errors());
    test::require(response.find("\"ok\":") != std::string::npos, "actual FOL command response missing");
    return response;
}
std::string wait_status(Process& owner, const fs::path& log, const auto& accept) {
    const auto end = std::chrono::steady_clock::now() + 10s;
    std::string status;
    do {
        test::require(owner.alive(), "actual FOL owner exited: " + owner.errors());
        try {
            status = host::send_command(log.string() + ".sock", "status");
            if (accept(status))
                return status;
        } catch (const std::runtime_error&) {
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < end);
    throw std::runtime_error("actual FOL owner status did not reach expected state: " + status + " " + owner.errors());
}
void private_online(std::string_view status) {
    for (const auto name : {"FORTS_REFDATA_REPL", "FORTS_PART_REPL", "FORTS_POS_REPL", "FORTS_TRADE_REPL",
                            "FORTS_USERORDERBOOK_REPL", "FORTS_SESSIONSTATE_REPL", "FORTS_INSTRUMENTSTATE_REPL"}) {
        const auto begin = status.find("\"stream\":\"" + std::string(name) + "\"");
        test::require(begin != std::string_view::npos, "required private/status stream absent");
        const auto state = status.substr(begin, status.find("\"error\":", begin) - begin);
        test::require(state.find("\"online\":true") != std::string_view::npos &&
                          state.find("\"snapshot_complete\":true") != std::string_view::npos,
                      "FOL test posted before all seven private/status snapshots were current");
    }
}
bool stale_exchange_moment(std::string_view status) {
    constexpr std::string_view field{"\"exchange_moment_ns\":"};
    const auto at = status.find(field);
    if (at == std::string_view::npos)
        return false;
    std::uint64_t moment{};
    const auto text = status.substr(at + field.size());
    const auto result = std::from_chars(text.data(), text.data() + text.size(), moment);
    const auto now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    return result.ec == std::errc{} && moment > 0 && now > 0 &&
           static_cast<std::uint64_t>(now) > moment + 30'000'000'000ULL;
}
std::size_t occurrences(std::string_view value, std::string_view needle) {
    std::size_t count{}, at{};
    while ((at = value.find(needle, at)) != std::string_view::npos) {
        ++count;
        at += needle.size();
    }
    return count;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    try {
        std::signal(SIGPIPE, SIG_IGN);
        const auto executable = fs::absolute(argv[1]).string();
        const auto library = fs::absolute(argv[2]);
        const auto root = test::make_temp_directory("fol_writable_runner_");
        const auto previous = fs::current_path();
        fs::current_path(root);
        host::ScopeExit restore([&] { fs::current_path(previous); });
        auto fixture =
            test::materialize_runtime_fixture(root / "runtime", library, moex::plaza2::cgate::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA 9.12.0", "9.12", "T1"));
        {
            std::ofstream profile(fixture.config_dir / "t1.ini", std::ios::app);
            profile << "\n[moex-fake-fol-runner]\nprofile=fresh\n";
        }
        const auto settings = "ini=" + (fixture.config_dir / "t1.ini").string() + ";key=00000000";
        ::setenv("MOEX_FOL_RUNNER_SETTINGS", settings.c_str(), 1);
        ::setenv("MOEX_FOL_RUNNER_CREDENTIALS", "offline-fixture-only", 1);
        ::setenv("MOEX_FOL_RUNNER_KEY", "00000000", 1);
        ::setenv("MOEX_FOL_RUNNER_BROKER", "BRK1", 1);
        ::setenv("MOEX_FOL_RUNNER_CLIENT", "C01", 1);
        ::setenv("MOEX_FOL_RUNNER_LOGIN", "fol-offline-owner", 1);
        unsigned index{};
        for (const auto option : {"--login-env", "--ext-id-range", "--max-commands-per-second", "--max-quantity",
                                  "--max-open-orders", "--max-notional", "--max-position", "--clock-offset-us"}) {
            const auto label = "missing-" + std::to_string(index++);
            const auto log = fs::path(label + ".ndjson");
            auto request = arguments(executable, fixture, log, label);
            erase_option(request, option);
            Process child(request, label);
            test::require(child.wait() == 2 && !child.errors().empty(),
                          "FOL --allow-orders accepted or misdiagnosed missing " + std::string(option) + ": " +
                              child.errors());
            test::require(contents(log).find("\"event\":\"command\"") == std::string::npos &&
                              contents(log).find("runtime_identity") == std::string::npos,
                          "missing trading scope reached CGate or a native post");
        }
        {
            const fs::path wrong_log{"wrong-source.ndjson"};
            host::CommandSocket wrong(wrong_log.string() + ".sock");
            Process client({executable, "plaza2", "cmd", "--offline-fake", "--log", wrong_log.string(),
                            "place forbidden 1001 buy 1 102500 day"},
                           "wrong-source-client");
            std::vector<std::string> received;
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (client.alive() && std::chrono::steady_clock::now() < deadline) {
                wrong.poll(
                    [&](std::string line) {
                        received.push_back(std::move(line));
                        return "{\"ok\":true,\"market_data_source\":\"aggr20\"}";
                    },
                    [](std::string_view, std::string_view) { return "{\"ok\":false}"; });
                std::this_thread::sleep_for(1ms);
            }
            test::require(client.wait() == 2 && received == std::vector<std::string>{"status"} &&
                              client.errors().find("not a Full Order Log") != std::string::npos,
                          "FOL command client sent a trading command to an AGGR endpoint");
        }
        for (const auto mode : {"fresh", "invalid", "stale"}) {
            // Each actual executable starts its own fake-runtime instance and isolated journal namespace.
            fs::remove(fixture.config_dir / "fol-control");
            fs::remove(fixture.config_dir / "fol-native-posts.log");
            const auto log = fs::path(std::string(mode) + ".ndjson");
            Process owner(arguments(executable, fixture, log, std::string("fol_") + mode),
                          std::string(mode) + "-owner");
            auto status = wait_status(owner, log, [](const auto& value) {
                return value.find("\"reconstructing\":false") != std::string::npos &&
                       value.find("\"source\":\"full_order_log\",\"valid\":true") != std::string::npos;
            });
            private_online(status);
            test::require(status.find("FORTS_AGGR") == std::string::npos &&
                              status.find("\"source_session_id\":321") != std::string::npos,
                          "actual FOL executable used AGGR or mismatched source session");
            test::require(command(executable, log, "place fol_order 1001 buy 1 102500 day", std::string(mode) + "-add")
                                  .find("\"ok\":true") != std::string::npos,
                          "fresh FOL Add was refused");
            wait_status(owner, log, [](const auto& value) {
                return value.find("\"client_order_id\":\"fol_order\"") != std::string::npos &&
                       value.find("\"state\":\"Working\"") != std::string::npos;
            });
            if (std::string_view(mode) == "fresh") {
                test::require(command(executable, log, "move fol_order 1 102000", "fresh-move").find("\"ok\":true") !=
                                  std::string::npos,
                              "fresh FOL Move was refused");
                wait_status(owner, log, [](const auto& value) {
                    return value.find("\"order_id\":20004") != std::string::npos &&
                           value.find("\"state\":\"Working\"") != std::string::npos;
                });
            } else {
                test::write_text_file(fixture.config_dir / "fol-control", std::string(mode) + '\n');
                status = wait_status(owner, log, [&](const auto& value) {
                    return value.find("\"source\":\"full_order_log\",\"valid\":false") != std::string::npos &&
                           (std::string_view(mode) == "invalid" ? value.find("\"crossed\":true") != std::string::npos
                                                                : stale_exchange_moment(value));
                });
                private_online(status);
                for (const auto& line : {"place blocked 1001 buy 1 102500 day", "move fol_order 1 102000"}) {
                    const auto response =
                        command(executable, log, line, std::string(mode) + "-blocked-" + std::to_string(index++));
                    test::require(response.find("\"ok\":false") != std::string::npos &&
                                      response.find("FullOrderLog") != std::string::npos,
                                  "invalid/stale FOL accepted Add/Move or refused for unrelated reason");
                }
            }
            test::require(
                command(executable, log, "cancel fol_order", std::string(mode) + "-cancel").find("\"ok\":true") !=
                    std::string::npos,
                "FOL Cancel was gated by market-data authority");
            wait_status(owner, log,
                        [](const auto& value) { return value.find("\"state\":\"Cancelled\"") != std::string::npos; });
            command(executable, log, "quit", std::string(mode) + "-quit");
            test::require(owner.wait() == 0, "FOL owner did not stop flat: " + owner.errors());
            const auto native = contents(fixture.config_dir / "fol-native-posts.log");
            const bool moved = std::string_view(mode) == "fresh";
            test::require(
                occurrences(native, "AddOrder wire_valid=1") == 1 &&
                    occurrences(native, "MoveOrder wire_valid=1") == (moved ? 1u : 0u) &&
                    occurrences(native, "DelOrder wire_valid=1") == 1 && occurrences(native, "wire_valid=0") == 0 &&
                    native.find("fol-offline-owner") == std::string::npos &&
                    native.find("offline-fixture-only") == std::string::npos &&
                    occurrences(native, "bytes=") == (moved ? 3u : 2u),
                "native fake post count/wire validation differs from exact authorized fixture sequence: " + native);
            const auto journal = contents(log);
            test::require(occurrences(journal, "\"event\":\"command\"") == (moved ? 3u : 2u) &&
                              occurrences(journal, "\"post_invoked\":true") == (moved ? 3u : 2u) &&
                              journal.find("\"name\":\"AddOrder\"") != std::string::npos &&
                              journal.find("\"name\":\"DelOrder\"") != std::string::npos &&
                              journal.find("\"msgid\":179") != std::string::npos &&
                              journal.find("\"msgid\":177") != std::string::npos &&
                              journal.find("offline-fixture-only") == std::string::npos,
                          "FOL journal omitted native posts/replies or exposed fixture credentials");
        }
        std::cout << "PASS actual FOL executable scoped fake Add/Move/Cancel, private7/current-FOL gates, "
                     "stale/invalid refusals\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
