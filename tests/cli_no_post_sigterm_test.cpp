// Embed the actual CLI entry point; fork (without exec) preserves the configured
// fake DLL state. Signal delivery, socket framing and shutdown code are real.
#define main moexctl_embedded_main
#include "../apps/moexctl.cpp"
#undef main
#include "fake_cgate_control.hpp"
#include "plaza2_runtime_test_support.hpp"
#include <fstream>
#include <sys/wait.h>

namespace test = moex::plaza2::test;
namespace cg = moex::plaza2::cgate;
using moex::connector_host::ScopeExit;
using moex::connector_host::send_command;
using test::require;
int main(int argc, char** argv) {
    try {
        require(argc == 2, "fake library required");
        const auto root = test::make_temp_directory("cli-no-post-signal");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        test::fake::Control fake(fixture.library_path);
        ::setenv("CLI_NO_POST_ENV", "ini=config/t1.ini;key=00000000", 1);
        ::setenv("CLI_NO_POST_CREDS", "fake-only", 1);
        ::setenv("CLI_NO_POST_KEY", "00000000", 1);
        ::setenv("CLI_NO_POST_BROKER", "BRK1", 1);
        ::setenv("CLI_NO_POST_CLIENT", "C01", 1);
        ::setenv("CLI_NO_POST_LOGIN", "fake-owner", 1);
        for (const std::string kind : {"healthy", "recover", "hold", "second-signal"}) {
            fake.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
            const auto dir = root / kind;
            std::filesystem::create_directory(dir);
            const auto log = dir / "journal.ndjson", socket = dir / "cmd.sock";
            std::vector<std::string> args = {argv[0],
                                             "plaza2",
                                             "run",
                                             "--qualification-no-post-test",
                                             "--allow-orders",
                                             "--sole-instance",
                                             "--runtime-root",
                                             fixture.root.string(),
                                             "--library-path",
                                             fixture.library_path.string(),
                                             "--scheme-dir",
                                             fixture.scheme_dir.string(),
                                             "--config-dir",
                                             fixture.config_dir.string(),
                                             "--env-settings-var",
                                             "CLI_NO_POST_ENV",
                                             "--credentials-env",
                                             "CLI_NO_POST_CREDS",
                                             "--software-key-env",
                                             "CLI_NO_POST_KEY",
                                             "--broker-code-env",
                                             "CLI_NO_POST_BROKER",
                                             "--client-code-env",
                                             "CLI_NO_POST_CLIENT",
                                             "--login-env",
                                             "CLI_NO_POST_LOGIN",
                                             "--ext-id-range",
                                             "100:100000",
                                             "--isin-id",
                                             "1001",
                                             "--max-commands-per-second",
                                             "5",
                                             "--max-quantity",
                                             "2",
                                             "--max-open-orders",
                                             "5",
                                             "--max-position",
                                             "1001=2",
                                             "--max-notional",
                                             "1001=300000",
                                             "--state",
                                             (dir / "identity.state").string(),
                                             "--log",
                                             log.string(),
                                             "--command-socket",
                                             socket.string()};
            const auto pid = ::fork();
            require(pid >= 0, "fork failed");
            if (!pid) {
                const int output = ::open((dir / "private-output.log").c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
                ::dup2(output, STDOUT_FILENO);
                ::dup2(output, STDERR_FILENO);
                ::close(output);
                std::vector<char*> raw;
                for (auto& a : args)
                    raw.push_back(a.data());
                raw.push_back(nullptr);
                stopping = second_signal = 0;
                const int result = moexctl_embedded_main(static_cast<int>(args.size()), raw.data());
                ::_Exit(result);
            }
            bool reaped{};
            ScopeExit cleanup([&] {
                if (!reaped) {
                    ::kill(pid, SIGKILL);
                    ::waitpid(pid, nullptr, 0);
                }
            });
            bool ready{};
            const auto admission = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < admission) {
                try {
                    const auto status = send_command(socket, "status");
                    ready = status.find("\"reconstructing\":false") != std::string::npos &&
                            status.find("\"order_entry_ready\":true") != std::string::npos &&
                            status.find("\"queued\":0") != std::string::npos;
                    if (ready)
                        break;
                } catch (...) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(ready, "CLI did not reach complete admission");
            require(send_command(socket, "kill off").find("\"ok\":false") != std::string::npos,
                    "kill switch was released");
            if (kind != "healthy") {
                const auto loss =
                    send_command(socket, kind == "recover" ? "qualification-loss recover" : "qualification-loss hold");
                require(loss.find("\"ok\":true") != std::string::npos, "owned loss refused");
                const auto lost = send_command(socket, "status");
                require(lost.find("\"reconstructing\":true") != std::string::npos,
                        "signal not delivered during reconnect");
            }
            const auto start = std::chrono::steady_clock::now();
            require(::kill(pid, SIGTERM) == 0, "SIGTERM failed");
            if (kind == "second-signal") {
                bool recorded{};
                const auto end = start + std::chrono::seconds(2);
                while (std::chrono::steady_clock::now() < end) {
                    std::ifstream stream(log);
                    const std::string text((std::istreambuf_iterator<char>(stream)), {});
                    recorded = text.find("\"line\":\"SIGTERM\"") != std::string::npos;
                    if (recorded)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                require(recorded && ::kill(pid, SIGTERM) == 0, "second SIGTERM failed");
            }
            int status{};
            const auto end = start + std::chrono::seconds(13);
            while (std::chrono::steady_clock::now() < end) {
                if (::waitpid(pid, &status, WNOHANG) == pid) {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(reaped && WIFEXITED(status), "CLI did not exit normally within bound");
            const auto elapsed = std::chrono::steady_clock::now() - start;
            require(WEXITSTATUS(status) == ((kind == "healthy" || kind == "recover") ? 0 : 8), "wrong CLI signal exit");
            if (kind == "hold")
                require(elapsed >= std::chrono::milliseconds(9500), "held recovery exited prematurely");
            if (kind == "second-signal")
                require(elapsed < std::chrono::seconds(3), "second signal did not interrupt drain");
            std::ifstream stream(log);
            const std::string text((std::istreambuf_iterator<char>(stream)), {});
            require(text.find("\"line\":\"SIGTERM\"") != std::string::npos &&
                        text.find("\"event\":\"command\"") == std::string::npos,
                    "signal audit missing or a command was posted");
            require(text.find("\"event\":\"cancel_drain\"") != std::string::npos, "drain audit missing");
        }
        std::cout << "Actual CLI fake-runtime SIGTERM/reconnect cases PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
