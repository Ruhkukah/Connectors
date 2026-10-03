#include "plaza2_runtime_test_support.hpp"

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

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
        const auto deadline = std::chrono::steady_clock::now() + 5s;
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
            "--isin-id",
            "1001",
            "--instance-id",
            std::string(instance),
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
    test::require(client.wait() == 0, "command client failed");
    return response;
}
void no_posts(const std::filesystem::path& log) {
    std::ifstream input(log);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    test::require(contents.find("\"event\":\"command\"") == std::string::npos,
                  "read-only CLI test posted a trading command");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    std::signal(SIGPIPE, SIG_IGN);
    const auto root = std::filesystem::path("/tmp") / ("moexctl_channel_" + std::to_string(::getpid()));
    test::require(std::filesystem::create_directory(root), "test directory already exists");
    const auto fixture =
        test::materialize_runtime_fixture(root / "runtime", argv[2], moex::plaza2::cgate::Plaza2Environment::Test,
                                          test::build_vendor_like_runtime_scheme("SPECTRA 9.9.0", "9.9", "T1"));
    const auto settings = "ini=" + (fixture.config_dir / "t1.ini").string() + ";key=00000000";
    ::setenv("MOEX_CLI_TEST_SETTINGS", settings.c_str(), 1);
    ::setenv("MOEX_CLI_TEST_CREDENTIALS", "fake-test-only", 1);
    ::setenv("MOEX_CLI_TEST_KEY", "00000000", 1);
    ::setenv("MOEX_CLI_TEST_BROKER", "BRK1", 1);
    ::setenv("MOEX_CLI_TEST_CLIENT", "C01", 1);
    unsigned failures{};
    auto scenario = [&](std::string_view name, const auto& body) {
        try {
            body();
            std::cout << name << ": PASS\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    };
    scenario("SIGHUP", [&] {
        const auto log = root / "hup.ndjson";
        Child owner(run_arguments(argv[1], fixture, log, "cli_hup"), root / "hup.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.signal(SIGHUP);
        std::this_thread::sleep_for(200ms);
        test::require(owner.alive(), "SIGHUP terminated the running host");
        test::require(remote_command(argv[1], log, "status").find("\"reconstructing\":false") != std::string::npos,
                      "SIGHUP lost command access");
        remote_command(argv[1], log, "quit");
        test::require(owner.wait() == 0, "SIGHUP host shutdown failed");
        no_posts(log);
    });
    scenario("SIGPIPE", [&] {
        const auto log = root / "pipe.ndjson";
        Child owner(run_arguments(argv[1], fixture, log, "cli_pipe"), root / "pipe.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.close_output();
        owner.send("status\n");
        std::this_thread::sleep_for(200ms);
        test::require(owner.alive(), "broken stdout terminated the running host");
        test::require(remote_command(argv[1], log, "status").find("\"reconstructing\":false") != std::string::npos,
                      "broken stdout lost socket responses");
        remote_command(argv[1], log, "quit");
        test::require(owner.wait() == 0, "SIGPIPE host shutdown failed");
        no_posts(log);
    });
    scenario("stdin EOF/reopen", [&] {
        const auto log = root / "eof.ndjson", fifo = root / "input.fifo";
        test::require(::mkfifo(fifo.c_str(), 0600) == 0, "FIFO creation failed");
        const auto reader = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
        test::require(reader >= 0, "FIFO reader failed");
        Child owner(run_arguments(argv[1], fixture, log, "cli_eof"), root / "eof.err", reader);
        ::close(reader);
        auto writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        test::require(writer >= 0, "FIFO writer failed");
        auto send = [&] { test::require(::write(writer, "status\n", 7) == 7, "FIFO status failed"); };
        ready(owner, send);
        ::close(writer);
        std::this_thread::sleep_for(1s);
        test::require(owner.alive(), "stdin EOF terminated the host");
        writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        test::require(writer >= 0, "FIFO writer reopen failed");
        test::require(::write(writer, "kill on\n", 8) == 8, "FIFO resumed command failed");
        ::close(writer);
        // Earlier status replies cannot satisfy this new command after the writer reopened.
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        bool acknowledged{};
        while (std::chrono::steady_clock::now() < deadline && !acknowledged)
            acknowledged = owner.line().find("\"ok\":true") != std::string::npos;
        test::require(acknowledged, "stdin input did not recover after EOF");
        owner.signal(SIGTERM);
        test::require(owner.wait() == 0, "EOF host shutdown failed");
        no_posts(log);
    });
    scenario("UNIX command channel", [&] {
        const auto log = root / "socket.ndjson";
        Child owner(run_arguments(argv[1], fixture, log, "cli_socket"), root / "socket.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.close_input();
        struct stat endpoint {};
        test::require(::stat((log.string() + ".sock").c_str(), &endpoint) == 0 && (endpoint.st_mode & 0777) == 0600 &&
                          S_ISSOCK(endpoint.st_mode),
                      "owner-only command socket missing");
        test::require(remote_command(argv[1], log, "kill on").find("\"ok\":true") != std::string::npos,
                      "kill switch could not be enabled through socket");
        test::require(remote_command(argv[1], log, "kill off").find("\"ok\":true") != std::string::npos,
                      "kill switch could not be disabled through socket");
        remote_command(argv[1], log, "quit");
        test::require(owner.wait() == 0, "socket quit did not stop host");
        test::require(!std::filesystem::exists(log.string() + ".sock"), "socket endpoint survived host stop");
        std::ifstream input(log);
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        test::require(contents.find("\"event\":\"kill_switch\"") != std::string::npos,
                      "socket command did not reach host");
        no_posts(log);
    });
    if (failures == 0)
        test::remove_tree(root);
    return failures == 0 ? 0 : 1;
}
