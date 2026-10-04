#include "plaza2_cgate/plaza2_runtime_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"
#include "scope_exit.hpp"
#include "command_socket.hpp"
#include "command_input.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <poll.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef MOEX_SOURCE_GIT_SHA
#define MOEX_SOURCE_GIT_SHA "unknown"
#endif

namespace {
using namespace std::chrono_literals;
namespace test = moex::plaza2::test;

class Child {
  public:
    Child(const std::vector<std::string>& arguments, const std::filesystem::path& diagnostics, int input_fd = -1) {
        int input[2]{-1, -1}, output[2];
        test::require(::pipe(output) == 0, "stdout pipe failed");
        if (input_fd < 0)
            test::require(::pipe(input) == 0, "stdin pipe failed");
        const auto error = ::open(diagnostics.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        test::require(error >= 0, "diagnostics file failed");
        pid_ = ::fork();
        test::require(pid_ >= 0, "fork failed");
        if (pid_ == 0) {
            std::signal(SIGPIPE, SIG_DFL);
            std::signal(SIGHUP, SIG_DFL);
            ::dup2(input_fd < 0 ? input[0] : input_fd, STDIN_FILENO);
            ::dup2(output[1], STDOUT_FILENO);
            ::dup2(error, STDERR_FILENO);
            for (const auto fd : {input[0], input[1], output[0], output[1], error, input_fd})
                if (fd > STDERR_FILENO)
                    ::close(fd);
            std::vector<char*> argv;
            for (const auto& argument : arguments)
                argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        if (input[0] >= 0)
            ::close(input[0]);
        ::close(output[1]);
        ::close(error);
        input_ = input[1];
        output_ = output[0];
        ::fcntl(output_, F_SETFL, O_NONBLOCK);
        // A subsequent command-client exec must not keep the owner's input/output alive.
        for (const auto fd : {input_, output_})
            if (fd >= 0)
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    ~Child() {
        close_input();
        close_output();
        if (alive()) {
            ::kill(pid_, SIGTERM);
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (alive() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(10ms);
            if (alive()) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status_, 0);
                reaped_ = true;
            }
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    bool alive() {
        if (!reaped_ && ::waitpid(pid_, &status_, WNOHANG) == pid_)
            reaped_ = true;
        return !reaped_;
    }
    void signal(int value) {
        test::require(::kill(pid_, value) == 0, "signal failed");
    }
    void send(std::string_view line) {
        test::require(input_ >= 0 && ::write(input_, line.data(), line.size()) == static_cast<ssize_t>(line.size()),
                      "stdin command failed");
    }
    void close_input() {
        if (input_ >= 0)
            ::close(input_);
        input_ = -1;
    }
    void close_output() {
        if (output_ >= 0)
            ::close(output_);
        output_ = -1;
    }
    std::string line(std::chrono::milliseconds timeout = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto end = pending_.find('\n'); end != std::string::npos) {
                auto result = pending_.substr(0, end);
                pending_.erase(0, end + 1);
                return result;
            }
            pollfd fd{.fd = output_, .events = POLLIN};
            if (::poll(&fd, 1, 20) > 0) {
                char data[4096];
                const auto size = ::read(output_, data, sizeof(data));
                if (size > 0)
                    pending_.append(data, static_cast<std::size_t>(size));
            }
            if (!alive() && pending_.empty())
                break;
        }
        throw std::runtime_error("CLI response missing (wait status " + std::to_string(status_) + ", reaped " +
                                 std::to_string(reaped_) + ")");
    }
    int wait() {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (alive() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        test::require(!alive(), "CLI did not stop");
        return WIFEXITED(status_) ? WEXITSTATUS(status_) : 128 + WTERMSIG(status_);
    }

  private:
    pid_t pid_{};
    int input_{-1}, output_{-1}, status_{};
    bool reaped_{};
    std::string pending_;
};

std::vector<std::string> run_arguments(const std::string& executable, const test::RuntimeFixturePaths& fixture,
                                       const std::filesystem::path& log, std::string_view instance) {
    return {executable,
            "plaza2",
            "run",
            "--runtime-root",
            fixture.root.string(),
            "--scheme-dir",
            fixture.scheme_dir.string(),
            "--config-dir",
            fixture.config_dir.string(),
            "--library-path",
            fixture.library_path.string(),
            "--env-settings-var",
            "MOEX_CLI_TEST_SETTINGS",
            "--credentials-env",
            "MOEX_CLI_TEST_CREDENTIALS",
            "--software-key-env",
            "MOEX_CLI_TEST_KEY",
            "--broker-code-env",
            "MOEX_CLI_TEST_BROKER",
            "--client-code-env",
            "MOEX_CLI_TEST_CLIENT",
            "--login-env",
            "MOEX_CLI_TEST_LOGIN",
            "--ext-id-range",
            "100:199",
            "--max-commands-per-second",
            "30",
            "--isin-id",
            "1001",
            "--instance-id",
            std::string(instance),
            "--max-quantity",
            "100",
            "--max-open-orders",
            "100",
            "--max-notional",
            "1001=100000000",
            "--max-position",
            "1001=100",
            "--log",
            log.string()};
}
void ready(Child& owner, const std::function<void()>& send) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    do {
        send();
        if (owner.line(10s).find("\"reconstructing\":false") != std::string::npos)
            return;
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("CLI did not reconstruct its Live/fake snapshots");
}
std::string remote_command(const std::string& executable, const std::filesystem::path& log, std::string command) {
    Child client({executable, "plaza2", "cmd", "--log", log.string(), std::move(command)},
                 log.string() + ".client.err");
    client.close_input();
    const auto response = client.line();
    test::require(client.wait() == (response.find("\"ok\":false") == std::string::npos ? 0 : 2),
                  "command client exit status did not reflect its refusal");
    return response;
}

} // namespace
int main(int argc, char** argv) {
    try {
        test::require(argc == 4, "trading runner, MD test runner and fake runtime required");
        std::signal(SIGPIPE, SIG_IGN);
        const auto root = test::make_temp_directory("full-order-log-isolation");
        moex::connector_host::ScopeExit cleanup([&] { test::remove_tree(root); });
        const auto fixture = test::materialize_runtime_fixture(
            root / "runtime", std::filesystem::absolute(argv[3]), moex::plaza2::cgate::Plaza2Environment::Test,
            test::build_vendor_like_runtime_scheme("9.9", "9.9", "TEST"));
        const auto settings = "ini=" + (fixture.config_dir / "t1.ini").string() + ";key=00000000";
        ::setenv("MOEX_CLI_TEST_SETTINGS", settings.c_str(), 1);
        ::setenv("MOEX_CLI_TEST_CREDENTIALS", "fake", 1);
        ::setenv("MOEX_CLI_TEST_KEY", "00000000", 1);
        ::setenv("MOEX_CLI_TEST_BROKER", "BRK1", 1);
        ::setenv("MOEX_CLI_TEST_CLIENT", "C01", 1);
        ::setenv("MOEX_CLI_TEST_LOGIN", "offline-isolation", 1);
        const auto log = root / "trading.ndjson";
        auto trading_args =
            run_arguments(std::filesystem::absolute(argv[1]).string(), fixture, log, "offline_isolation_trade");
        Child trader(trading_args, root / "trading.err");
        ready(trader, [&] { trader.send("status\n"); });
        std::vector<std::string> md_args{std::filesystem::absolute(argv[2]).string(),
                                         "plaza2",
                                         "qualify",
                                         "--runtime-root",
                                         fixture.root.string(),
                                         "--library-path",
                                         fixture.library_path.string(),
                                         "--scheme-dir",
                                         fixture.scheme_dir.string(),
                                         "--config-dir",
                                         fixture.config_dir.string(),
                                         "--env-settings-var",
                                         "MOEX_CLI_TEST_SETTINGS",
                                         "--software-key-env",
                                         "MOEX_CLI_TEST_KEY",
                                         "--isin-id",
                                         "1001",
                                         "--instance-id",
                                         "offline_isolation_md",
                                         "--offline-fake",
                                         "--dtc-port",
                                         "11398"};
        Child md(md_args, root / "md.err");
        test::require(md.line().find("dtc_port=") != std::string::npos, "actual MD process started");
        const auto responsive = [&] {
            const auto start = std::chrono::steady_clock::now();
            trader.send("status\n");
            const auto status = trader.line(std::chrono::seconds(2));
            test::require(status.find("\"reconstructing\":false") != std::string::npos,
                          "trader snapshot remains current");
            test::require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
                          "trader stays responsive");
        };
        md.signal(SIGSTOP);
        responsive();
        md.signal(SIGKILL);
        md.wait();
        responsive();
        trader.send("quit\n");
        trader.close_input();
        test::require(trader.wait() == 0, "trader stops normally");
        // The same production main refuses a native live start before missing entitlement/owner approval.
        md_args.erase(std::find(md_args.begin(), md_args.end(), "--offline-fake"));
        Child gated(md_args, root / "gate.err");
        gated.close_input();
        test::require(gated.wait() == 2, "live start is gated before any native CGate connection");
        md_args.push_back("--offline-fake");
        // A DTC bind failure after host startup must unwind cleanly.
        const auto occupied_port = ::socket(AF_INET, SOCK_STREAM, 0);
        test::require(occupied_port >= 0, "occupied port socket");
        moex::connector_host::ScopeExit close_occupied([&] { ::close(occupied_port); });
        test::require(::fcntl(occupied_port, F_SETFD, FD_CLOEXEC) == 0, "occupied port close-on-exec");
        sockaddr_in occupied_address{};
        occupied_address.sin_family = AF_INET;
        occupied_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        test::require(::bind(occupied_port, reinterpret_cast<sockaddr*>(&occupied_address), sizeof(occupied_address)) ==
                              0 &&
                          ::listen(occupied_port, 1) == 0,
                      "hold loopback listener");
        socklen_t occupied_size = sizeof(occupied_address);
        test::require(::getsockname(occupied_port, reinterpret_cast<sockaddr*>(&occupied_address), &occupied_size) == 0,
                      "occupied port number");
        auto bind_failure_args = md_args;
        const auto port = std::find(bind_failure_args.begin(), bind_failure_args.end(), "--dtc-port");
        *(port + 1) = std::to_string(ntohs(occupied_address.sin_port));
        Child bind_failure(bind_failure_args, root / "bind-failure.err");
        bind_failure.close_input();
        test::require(bind_failure.wait() == 2, "DTC bind failure exits cleanly after native host startup");
        const auto isin = std::find(md_args.begin(), md_args.end(), "--isin-id");
        *(isin + 1) = "2147483648";
        Child overflow(md_args, root / "overflow.err");
        overflow.close_input();
        test::require(overflow.wait() == 2, "native i4 ISIN overflow must be rejected");
        std::cout << "stalled and failed separate MD process leaves trading responsive: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
