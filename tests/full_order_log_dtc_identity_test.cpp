#include "full_order_log_dtc_identity.hpp"

#include <chrono>
#include <fstream>
#include <future>
#include <iostream>

using namespace moex::connector_host;
namespace fs = std::filesystem;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
FullOrderLogDtcIdentity::Result await_result(FullOrderLogDtcIdentity& ledger) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        if (auto result = ledger.take())
            return *result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("identity acknowledgement timed out");
}
void write_partial(const fs::path& file) {
    std::ofstream out(file);
    out << "100\npart";
    out.close();
    require(::chmod(file.c_str(), 0600) == 0, "partial file permissions");
}
} // namespace

int main() {
    fs::path root;
    try {
        auto pattern = (fs::temp_directory_path() / "fol-identity-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        require(directory != nullptr, "temporary root");
        root = directory;
        TradingHostConfig config;
        config.isin_ids = {1001};
        config.orders.ext_id_begin = 100;
        config.orders.ext_id_end = 200;
        config.orders.broker_code = "BRK1";
        config.orders.client_code = "C01";
        config.orders.login_from = "test-login";
        config.identity_state_path = root / "state";
        const auto ledger_path = root / "state.dtc";
        {
            FullOrderLogDtcIdentity ledger(config);
            ledger.start();
            require(!ledger.failed(), "new ledger failed");
            require(ledger.submit("dtc:first", 100, false).empty(), "first identity refused");
            require(!ledger.submit("dtc:first", 100, false).empty(), "pending identity reused");
            const auto result = await_result(ledger);
            require(result.ext_id == 100 && result.persisted, "identity was not durably acknowledged");
            require(fs::exists(ledger_path / "first") && !fs::exists(ledger_path / ".pending-100-first"),
                    "acknowledgement preceded atomic rename");
            std::ifstream file(ledger_path / "first");
            const std::string bytes((std::istreambuf_iterator<char>(file)), {});
            require(bytes == "100\n1001\nBRK1C01\ntest-login\nM\n", "final record is incomplete");
            require(ledger.by_ext(100) && ledger.by_ext(100)->client_id == "first", "owner identity lost");
            ledger.latch_failure();
            require(ledger.failed() && !ledger.submit("dtc:blocked", 101, false).empty(),
                    "failure latch did not fence subsequent submissions");
        }
        // Do not parse or adopt a partial file. Its name preserves both
        // reservations; the host's durable ceiling also consumes the ext_id.
        write_partial(ledger_path / ".pending-101-crashed");
        {
            FullOrderLogDtcIdentity ledger(config);
            ledger.start();
            require(ledger.quarantined_records() == 1 && !ledger.failed(), "partial file not quarantined");
            require(fs::exists(ledger_path / ".quarantine-101-crashed") &&
                        !fs::exists(ledger_path / ".pending-101-crashed"),
                    "quarantine rename missing");
            require(ledger.contains("crashed") && !ledger.by_ext(101), "partial identity adopted or released");
            require(!ledger.submit("dtc:crashed", 102, false).empty() &&
                        !ledger.submit("dtc:new-key", 101, false).empty(),
                    "quarantined key or ext_id reused");
            require(ledger.submit("dtc:second", 102, true).empty() && await_result(ledger).persisted,
                    "quarantine prevented valid new identity");
        }
        {
            auto foreign = config;
            foreign.isin_ids = {2001};
            FullOrderLogDtcIdentity ledger(foreign);
            ledger.start();
            require(ledger.quarantined_records() == 1 && ledger.contains("first") && !ledger.by_ext(100),
                    "restart lost quarantine or adopted another instrument");
            require(!ledger.submit("dtc:foreign", 100, false).empty(), "foreign ext_id was reused");
        }
        {
            auto fake = config;
            fake.session.mode = moex::plaza2_trade::CgateSessionMode::OfflineFake;
            std::promise<void> entered, resume;
            auto ready = entered.get_future();
            auto unblock = resume.get_future();
            FullOrderLogDtcIdentity ledger(fake, [&] {
                entered.set_value();
                unblock.wait();
            });
            ledger.start();
            require(ledger.submit("dtc:late-ack", 105, false).empty(), "stalled fixture submit");
            const bool waiting = ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            // These owner-side calls must remain responsive while the disk
            // worker is blocked. Unblock before asserting to retain clean teardown.
            const auto started = std::chrono::steady_clock::now();
            ledger.latch_failure();
            const bool no_ack = !ledger.take();
            const bool refused = !ledger.submit("dtc:after-timeout", 106, false).empty();
            const auto elapsed = std::chrono::steady_clock::now() - started;
            resume.set_value();
            require(waiting && no_ack && refused && elapsed < std::chrono::milliseconds(100),
                    "stalled persistence blocked owner or admitted a new identity");
            require(await_result(ledger).persisted && ledger.failed() && ledger.by_ext(105),
                    "late acknowledgement lost reservation or cleared failure latch");
        }
        {
            FullOrderLogDtcIdentity ledger(config);
            ledger.start();
            // Insert after startup to exercise a filesystem write failure.
            fs::create_directory(ledger_path / ".pending-103-disk-error");
            require(ledger.submit("dtc:disk-error", 103, false).empty(), "failure fixture submit");
            require(!await_result(ledger).persisted && ledger.failed(), "disk failure was not latched");
            require(!fs::exists(ledger_path / "disk-error"), "failed write published a final record");
            require(!ledger.submit("dtc:after-failure", 104, false).empty(), "disk failure did not fence entry");
        }
        fs::remove(ledger_path / ".pending-103-disk-error");
        write_partial(ledger_path / ".pending-106-first");
        bool conflict = false;
        try {
            FullOrderLogDtcIdentity ledger(config);
            ledger.start();
        } catch (const std::runtime_error&) {
            conflict = true;
        }
        require(conflict, "final and quarantined records reused one client key");
        fs::remove_all(root);
        std::cout << "atomic identity persistence and crash quarantine passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (!root.empty())
            fs::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
