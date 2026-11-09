#include "plaza2_runtime_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"
#include "command_socket.hpp"
#include "scope_exit.hpp"
#include "dtc_test_wire.hpp"
#include "moex/connector_host/dtc_market_data.hpp"
#include "moex/connector_host/operator_config.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <poll.h>
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
    void terminate() {
        test::require(alive() && ::kill(pid_, SIGTERM) == 0, "signal owned offline fixture");
    }
    void crash() {
        test::require(alive() && ::kill(pid_, SIGKILL) == 0, "cannot crash owned offline FOL fixture");
        test::require(wait() == 128 + SIGKILL, "offline FOL crash did not preserve signal exit");
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
void check_environment_authority_cli(const std::string& executable, const test::RuntimeFixturePaths& fixture) {
    const auto flag = host::kFolConfigurationAuthorityOption;
    std::vector<std::string_view> parsed_args{"plaza2",
                                              "run",
                                              "--allow-orders",
                                              "--runtime-root",
                                              fixture.root.native(),
                                              "--scheme-dir",
                                              fixture.scheme_dir.native(),
                                              "--config-dir",
                                              fixture.config_dir.native(),
                                              "--env-settings-var",
                                              "MOEX_FOL_RUNNER_SETTINGS",
                                              "--broker-code-env",
                                              "MOEX_FOL_RUNNER_BROKER",
                                              "--client-code-env",
                                              "MOEX_FOL_RUNNER_CLIENT",
                                              "--isin-id",
                                              "1001"};
    const auto configured = [&] {
        return host::parse_operator_arguments(parsed_args, true)
            .config.transport.host.full_order_log_legacy_configuration_authority;
    };
    test::require(!configured(), "configuration authority must be opt-in");
    parsed_args.push_back(flag);
    test::require(configured(), "live TEST FOL parser must carry the explicit opt-in");
    parsed_args.insert(parsed_args.end() - 1, "--instance-id");
    test::require(!configured(), "an option-looking argument value must not grant authority");

    const auto refuse = [&](std::vector<std::string> request, const std::string& label,
                            std::string_view reason = "live TEST FOL writer") {
        Process child(request, label);
        test::require(child.wait() == 2 && child.errors().find(reason) != std::string::npos,
                      "authority option did not fail closed in " + label + ": " + child.errors());
        test::require(!fs::exists(label + ".ndjson") && !fs::exists(fixture.config_dir / "fol-native-posts.log"),
                      "authority refusal reached the journal or native post path");
    };
    auto request = arguments(executable, fixture, "authority-offline.ndjson", "fol_authority");
    request.emplace_back(flag);
    refuse(request, "authority-offline");

    request = arguments(executable, fixture, "authority-cancel.ndjson", "fol_authority");
    *std::ranges::find(request, "--offline-fake") = "--live";
    request.insert(request.end(), {"--trading-instance-id", "aggr_authority", "--cancel-only", std::string(flag)});
    refuse(request, "authority-cancel");

    request = arguments(executable, fixture, "authority-prod.ndjson", "fol_authority");
    *std::ranges::find(request, "--offline-fake") = "--live";
    *(std::ranges::find(request, "--environment") + 1) = "prod";
    request.insert(request.end(), {"--trading-instance-id", "aggr_authority", std::string(flag)});
    refuse(request, "authority-prod");

    refuse({executable, "plaza2", "qualify", "--offline-fake", std::string(flag)}, "authority-qualify");
    refuse({executable, "plaza2", "cmd", "--log", "authority-cmd.ndjson", std::string(flag)}, "authority-cmd");
    request = arguments(executable, fixture, "authority-malformed.ndjson", "fol_authority");
    request.emplace_back("--environment-authority=venue");
    refuse(request, "authority-malformed", "unknown option");
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
std::string wait_status(Process& owner, const fs::path& log, const auto& accept, std::chrono::seconds timeout = 10s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
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

#include "full_order_log_command_cases.hpp"
#include "full_order_log_recovery_cases.hpp"
#include "full_order_log_dtc_worker_case.hpp"

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4)
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
        if (argc == 4) {
            const std::string_view test_case = argv[3];
            if (test_case == "dtc-worker")
                test_fol_dtc_worker(executable, fixture);
            else if (test_case == "outages")
                test_fol_outages(executable, fixture);
            else if (test_case == "restart")
                test_fol_restart(executable, fixture);
            else if (!run_fol_command_case(test_case, executable, fixture))
                throw std::invalid_argument("unknown FOL writer fixture case");
            std::cout << "PASS offline FOL writer " << test_case << '\n';
            return 0;
        }
        check_environment_authority_cli(executable, fixture);
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
            const fs::path misleading_log{"--clock-offset-us"};
            const fs::path identity{"clock-value.state"};
            auto request = arguments(executable, fixture, "clock-value.ndjson", "fol_clock_value");
            erase_option(request, "--clock-offset-us");
            const auto log = std::ranges::find(request, "--log");
            *(log + 1) = misleading_log.string();
            request.insert(request.end(), {"--state", identity.string()});
            Process child(request, "clock-value");
            test::require(child.wait() == 2 && child.errors().find("--clock-offset-us") != std::string::npos,
                          "option-looking log value bypassed parsed clock requirement");
            for (const auto& path :
                 {misleading_log, fs::path(misleading_log.string() + ".sock"), identity,
                  fs::path(identity.string() + ".lock"), fixture.config_dir / "fol-native-posts.log"})
                test::require(!fs::exists(path), "clock refusal created journal/socket/ID/native-post resources");
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
        {
            const fs::path log{"capture-writer.ndjson"};
            auto request = arguments(executable, fixture, log, "fol_capture_writer");
            const auto capture = root / "writer-capture";
            request.insert(request.end(),
                           {"--capture-dir", capture.string(), "--capture-min-free-bytes", "0", "--open-interest"});
            Process owner(request, "capture-writer");
            wait_status(owner, log, [](const auto& status) {
                return status.find("\"order_entry_ready\":true") != std::string::npos;
            });
            command(executable, log, "kill on", "capture-kill");
            command(executable, log, "quit", "capture-quit");
            test::require(owner.wait() == 0, "recorder writer did not drain: " + owner.errors());
            const auto replay = moex::plaza2::cgate::replay_full_order_log_capture(capture);
            test::require(replay.valid_commits > 0 && replay.open_interest_updates > 0 &&
                              owner.output().find("capture_failed=0") != std::string::npos,
                          "actual writer recording/replay failed");
        }
        {
            fs::remove(fixture.config_dir / "fol-native-posts.log");
            const fs::path log{"capture-failure.ndjson"};
            const auto capture = root / "writer-capture-failure";
            auto request = arguments(executable, fixture, log, "fol_capture_failure");
            request.insert(request.end(), {"--capture-dir", capture.string(), "--capture-min-free-bytes", "0"});
            Process owner(request, "capture-failure");
            auto status = wait_status(
                owner, log, [](const auto& s) { return s.find("\"order_entry_ready\":true") != std::string::npos; });
            test::require(status.find("FORTS_DEALS_REPL") == std::string::npos,
                          "default writer must not open OI listener");
            for (const auto id : {"failure_one", "failure_two"}) {
                test::require(command(executable, log, std::string("place ") + id + " 1001 buy 1 102500 day", id)
                                      .find("\"ok\":true") != std::string::npos,
                              "failure fixture Add");
                wait_status(owner, log, [&](const auto& s) {
                    return occurrences(s, "\"state\":\"Working\"") == (std::string_view(id) == "failure_one" ? 1 : 2);
                });
            }
            test::write_text_file(fixture.config_dir / "fol-control", "capture-fail\n");
            test::require(owner.wait() == 3, "recorder failure writer must exit 3: " + owner.errors());
            const auto journal = contents(log);
            const auto native = contents(fixture.config_dir / "fol-native-posts.log");
            test::require(
                occurrences(native, "AddOrder wire_valid=1") == 2 &&
                    occurrences(native, "DelOrder wire_valid=1") == 2 && occurrences(native, "wire_valid=0") == 0 &&
                    occurrences(journal, "\"state\":\"Cancelled\"") >= 2 &&
                    journal.find("\"enabled\":true") != std::string::npos &&
                    journal.find("\"outcome\":\"completed\"") != std::string::npos &&
                    journal.find("\"working_orders\":false") != std::string::npos &&
                    owner.errors().find("capture failed, entering normal SIGTERM cancel drain") != std::string::npos,
                "recorder failure must kill and confirm two cancels with completed drain: " + journal);
            test::require(!fs::exists(log.string() + ".sock"), "failure writer socket removed");
            test::require(fs::file_size(capture / "stop-status.folend") == 40,
                          "failed capture has reserved stop record");
            std::ifstream stop(capture / "stop-status.folend", std::ios::binary);
            std::array<std::uint64_t, 5> record{};
            stop.read(reinterpret_cast<char*>(record.data()), sizeof record);
            test::require(record[2] == 2 && record[3] == 3, "capture-failed stop status and exit recorded");
        }
        {
            const fs::path log{"crash-startup.ndjson"};
            const auto request = arguments(executable, fixture, log, "fol_crash_startup");
            const fs::path socket{log.string() + ".sock"};
            Process first(request, "crash-first");
            wait_status(first, log, [](const auto& status) {
                return status.find("\"reconstructing\":false") != std::string::npos;
            });
            struct stat before {
            }, after{};
            test::require(::lstat(socket.c_str(), &before) == 0, "owned socket missing before offline crash");
            {
                Process duplicate(request, "crash-duplicate");
                test::require(duplicate.wait() != 0 && ::lstat(socket.c_str(), &after) == 0 &&
                                  before.st_ino == after.st_ino && before.st_dev == after.st_dev,
                              "live identity owner did not protect its socket");
            }
            first.crash();
            test::require(fs::exists(socket), "SIGKILL fixture did not leave a stale socket");
            Process recovered(request, "crash-recovered");
            wait_status(recovered, log, [](const auto& status) {
                return status.find("\"reconstructing\":false") != std::string::npos;
            });
            command(executable, log, "quit", "crash-recovered-quit");
            test::require(recovered.wait() == 0 && !fs::exists(socket),
                          "same-state FOL restart required manual stale socket removal");
            test::require(contents(log).find("\"event\":\"command\"") == std::string::npos,
                          "offline stale-socket test submitted an order");
        }
        {
            const auto capture = root / "qualify-term-capture";
            Process owner({executable,
                           "plaza2",
                           "qualify",
                           "--offline-fake",
                           "--runtime-root",
                           fixture.root.string(),
                           "--scheme-dir",
                           fixture.scheme_dir.string(),
                           "--config-dir",
                           fixture.config_dir.string(),
                           "--library-path",
                           fixture.library_path.string(),
                           "--expected-release",
                           "SPECTRA9.12.0",
                           "--env-settings-var",
                           "MOEX_FOL_RUNNER_SETTINGS",
                           "--software-key-env",
                           "MOEX_FOL_RUNNER_KEY",
                           "--isin-id",
                           "1001",
                           "--instance-id",
                           "qualify_term",
                           "--dtc-port",
                           std::to_string(20000 + ::getpid() % 30000),
                           "--capture-dir",
                           capture.string(),
                           "--capture-min-free-bytes",
                           "0"},
                          "qualify-term");
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (owner.alive() && owner.output().find("first_valid_commit=1") == std::string::npos &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(10ms);
            test::require(owner.output().find("first_valid_commit=1") != std::string::npos, "qualify reached ONLINE");
            owner.terminate();
            test::require(owner.wait() == 0, "TERM qualify returns zero: " + owner.errors());
            const auto replay = moex::plaza2::cgate::replay_full_order_log_capture(capture);
            test::require(!replay.truncated && replay.host_status == 0 && replay.valid_commits > 0,
                          "TERM qualify writes clean end");
        }
        {
            const auto original_profile = contents(fixture.config_dir / "t1.ini");
            test::write_text_file(fixture.config_dir / "t1.ini",
                                  original_profile.substr(0, original_profile.find("[moex-fake-fol-runner]")) +
                                      "[moex-fake-fol-runner]\nprofile=short-snapshot\n");
            for (const auto mode : {"qualify", "run"}) {
                fs::remove(fixture.config_dir / "fol-native-posts.log");
                const auto log = fs::path(std::string("unframed-") + mode + ".ndjson");
                auto request = arguments(executable, fixture, log, std::string("unframed_") + mode);
                if (std::string_view(mode) == "qualify")
                    request = {executable,
                               "plaza2",
                               "qualify",
                               "--offline-fake",
                               "--runtime-root",
                               fixture.root.string(),
                               "--scheme-dir",
                               fixture.scheme_dir.string(),
                               "--config-dir",
                               fixture.config_dir.string(),
                               "--library-path",
                               fixture.library_path.string(),
                               "--expected-release",
                               "SPECTRA9.12.0",
                               "--env-settings-var",
                               "MOEX_FOL_RUNNER_SETTINGS",
                               "--software-key-env",
                               "MOEX_FOL_RUNNER_KEY",
                               "--isin-id",
                               "1001",
                               "--instance-id",
                               "unframed_qualify",
                               "--dtc-port",
                               std::to_string(20000 + ::getpid() % 30000)};
                const auto capture = root / (std::string("nonzero-") + mode);
                request.insert(request.end(), {"--capture-dir", capture.string(), "--capture-min-free-bytes", "0"});
                Process child(request, std::string("unframed-") + mode);
                test::require(child.wait() != 0, "three-strike FOL process must exit non-zero in both modes");
                const auto output = child.errors() + child.output() + contents(log);
                test::require(output.find("3 identical row-validation failures") != std::string::npos &&
                                  output.find("row size table=orders") != std::string::npos,
                              "FOL executable did not retain the exact fatal reason: " + output);
                const auto end = moex::plaza2::cgate::read_full_order_log_capture(capture, [](const auto&) {}, true);
                test::require(end.truncated && end.host_status != 0 &&
                                  end.end_status == moex::plaza2::cgate::FullOrderLogCaptureEndStatus::HostNonzero,
                              "nonzero executable stop carries host status");
                test::require(!fs::exists(fixture.config_dir / "fol-native-posts.log") &&
                                  !fs::exists(log.string() + ".sock"),
                              "unframed diagnostic posted or failed to clean its command socket");
            }
            test::write_text_file(fixture.config_dir / "t1.ini", original_profile);
        }
        for (const auto mode : {"fresh", "quiet", "invalid", "dead"}) {
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
            if (std::string_view(mode) == "fresh" || std::string_view(mode) == "quiet") {
                if (std::string_view(mode) == "quiet") {
                    test::write_text_file(fixture.config_dir / "fol-control", "quiet\n");
                    status = wait_status(owner, log, [](const auto& value) {
                        return value.find("\"source\":\"full_order_log\",\"valid\":true") != std::string::npos &&
                               stale_exchange_moment(value);
                    });
                    private_online(status);
                }
                test::require(command(executable, log, "move fol_order 1 102000", std::string(mode) + "-move")
                                      .find("\"ok\":true") != std::string::npos,
                              "fresh FOL Move was refused");
                wait_status(owner, log, [](const auto& value) {
                    return value.find("\"order_id\":20004") != std::string::npos &&
                           value.find("\"state\":\"Working\"") != std::string::npos;
                });
            } else {
                test::write_text_file(fixture.config_dir / "fol-control", std::string(mode) + '\n');
                status = wait_status(
                    owner, log,
                    [&](const auto& value) {
                        return value.find("\"source\":\"full_order_log\",\"valid\":false") != std::string::npos &&
                               (std::string_view(mode) == "invalid"
                                    ? value.find("\"crossed\":true") != std::string::npos
                                    : stale_exchange_moment(value));
                    },
                    std::string_view(mode) == "dead" ? 35s : 10s);
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
            const bool moved = std::string_view(mode) == "fresh" || std::string_view(mode) == "quiet";
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
        {
            fs::remove(fixture.config_dir / "fol-control");
            fs::remove(fixture.config_dir / "fol-native-posts.log");
            auto profile = contents(fixture.config_dir / "t1.ini");
            const auto at = profile.find("profile=fresh");
            test::require(at != std::string::npos, "recovery fixture profile missing");
            profile.replace(at, std::string_view("profile=fresh").size(), "profile=recovery");
            test::write_text_file(fixture.config_dir / "t1.ini", profile);
            // Recovery must not touch even an unusable public FOL client schema.
            test::write_text_file(fixture.scheme_dir / "full_order_log/snapshot.ini", "invalid FOL scheme\n");
            const fs::path log{"cancel-only.ndjson"};
            auto request = arguments(executable, fixture, log, "fol_cancel_only");
            request.push_back("--cancel-only");
            Process owner(std::move(request), "cancel-only-owner");
            auto status = wait_status(owner, log, [](const auto& value) {
                return value.find("\"reconstructing\":false") != std::string::npos &&
                       value.find("recovered:321:20003") != std::string::npos;
            });
            private_online(status);
            test::require(status.find("\"execution_mode\":\"full_order_log_cancel_only\"") != std::string::npos &&
                              status.find("FORTS_ORDLOG") == std::string::npos &&
                              status.find("FORTS_AGGR") == std::string::npos,
                          "cancel-only recovery must have private authority without market-data listeners");
            command(executable, log, "kill off", "cancel-only-kill-off");
            for (const auto& line : {"place forbidden 1001 buy 1 102500 day", "move recovered:321:20003 1 102000"})
                test::require(command(executable, log, line, "cancel-only-refused-" + std::to_string(index++))
                                      .find("\"ok\":false") != std::string::npos,
                              "kill off must not grant Add or Move authority in cancel-only recovery");
            test::require(
                command(executable, log, "cancel recovered:321:20003", "cancel-only-cancel").find("\"ok\":true") !=
                    std::string::npos,
                "reconstructed owned order cancel must work with unavailable FOL scheme");
            wait_status(owner, log,
                        [](const auto& value) { return value.find("\"state\":\"Cancelled\"") != std::string::npos; });
            command(executable, log, "quit", "cancel-only-quit");
            test::require(owner.wait() == 0, "cancel-only executable did not stop flat: " + owner.errors());
            const auto native = contents(fixture.config_dir / "fol-native-posts.log");
            test::require(occurrences(native, "DelOrder wire_valid=1") == 1 && occurrences(native, "bytes=") == 1 &&
                              native.find("AddOrder") == std::string::npos &&
                              native.find("MoveOrder") == std::string::npos,
                          "cancel-only fixture must publish exactly its recovered-order cancellation");
        }
        std::cout << "PASS actual FOL executable scoped fake Add/Move/Cancel, private7/current-FOL gates, "
                     "quiet-source admission/dead-stream refusals and cancel-only recovery\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
