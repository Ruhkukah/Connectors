#include "moex/plaza2/cgate/plaza2_aggr20_authority_probe.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace {

using namespace moex::plaza2::cgate;

Plaza2Aggr20AuthorityProbeConfig make_config(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    Plaza2Aggr20AuthorityProbeConfig config;
    config.profile_id = "read_only_aggr20_authority_probe_test";
    config.endpoint_host = "198.51.100.10";
    config.endpoint_port = 4001;
    config.runtime.environment = Plaza2Environment::Test;
    config.runtime.runtime_root = fixture.root;
    config.runtime.expected_spectra_release = "SPECTRA93";
    config.runtime.env_open_settings = "ini=config/t1.ini;key=${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}";
    config.connection_settings = "p2tcp://198.51.100.10:4001;app_name=read_only_aggr20_probe";
    config.refdata_stream.settings =
        "p2repl://FORTS_REFDATA_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_REFDATA_REPL";
    config.refdata_stream.open_settings = "mode=snapshot+online";
    config.aggr_stream.settings = "p2repl://FORTS_AGGR20_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_AGGR20_REPL";
    config.aggr_stream.open_settings = "mode=snapshot+online";
    config.software_key.source = Plaza2CredentialSource::Env;
    config.software_key.env_var = "MOEX_PLAZA2_CGATE_SOFTWARE_KEY";
    config.arm_state.test_network_armed = true;
    config.arm_state.test_session_armed = true;
    config.arm_state.test_plaza2_armed = true;
    config.process_timeout_ms = 0;
    config.observation_window = std::chrono::milliseconds(20);
    return config;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

Plaza2Aggr20AuthorityProbeAttempt make_complete_attempt(bool session_data_ready_after_online) {
    Plaza2Aggr20AuthorityProbeAttempt attempt;
    attempt.listener_created = true;
    attempt.listener_opened = true;
    attempt.online = true;
    attempt.snapshot_complete = true;
    attempt.session_data_ready_after_online = session_data_ready_after_online;
    return attempt;
}

bool contains_trace(const Plaza2Aggr20AuthorityProbeAttempt& attempt, std::string_view needle) {
    for (const auto& line : attempt.event_trace) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void test_attempt_classification() {
    const auto both_ready =
        std::vector<Plaza2Aggr20AuthorityProbeAttempt>{make_complete_attempt(true), make_complete_attempt(true)};
    moex::plaza2::test::require(plaza2_aggr20_authority_probe_classify_attempts(both_ready) ==
                                    Plaza2Aggr20AuthorityProbeResult::Yes,
                                "overall YES must require both complete attempts to observe readiness");

    const auto one_missing =
        std::vector<Plaza2Aggr20AuthorityProbeAttempt>{make_complete_attempt(true), make_complete_attempt(false)};
    moex::plaza2::test::require(plaza2_aggr20_authority_probe_classify_attempts(one_missing) ==
                                    Plaza2Aggr20AuthorityProbeResult::No,
                                "complete attempts without readiness must produce NO");

    auto failed = make_complete_attempt(true);
    failed.error = "listener open failed";
    const auto one_failed =
        std::vector<Plaza2Aggr20AuthorityProbeAttempt>{std::move(failed), make_complete_attempt(true)};
    moex::plaza2::test::require(plaza2_aggr20_authority_probe_classify_attempts(one_failed) ==
                                    Plaza2Aggr20AuthorityProbeResult::Inconclusive,
                                "an attempt error must produce INCONCLUSIVE");
    moex::plaza2::test::require(plaza2_aggr20_authority_probe_classify_attempts({make_complete_attempt(true)}) ==
                                    Plaza2Aggr20AuthorityProbeResult::Inconclusive,
                                "a missing repeated attempt must produce INCONCLUSIVE");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "expected fake runtime library path\n";
            return 1;
        }

        using namespace moex::plaza2::test;
        const auto fixture_root = make_temp_directory("plaza2_aggr20_authority_probe_test");
        const auto cleanup = [&]() { remove_tree(fixture_root); };
        const auto fake_library = std::filesystem::path(argv[1]);
        const auto scheme = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture = materialize_runtime_fixture(fixture_root, fake_library, Plaza2Environment::Test, scheme);
        const auto audit_path = fixture.root / "read_only_probe.audit";

        require(Plaza2Aggr20AuthorityProbeConfig{}.observation_window >= std::chrono::seconds(60),
                "diagnostic probe observation default must be at least 60 seconds");
        test_attempt_classification();

        ::setenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY", "READ-ONLY-PROBE-KEY", 1);
        ::setenv("MOEX_FAKE_CAPTURE_AUDIT", audit_path.c_str(), 1);

        Plaza2Aggr20AuthorityProbe probe(make_config(fixture));
        const auto report = probe.run();
        require(report.read_only_contract_ok, "successful probe must certify its read-only contract");
        require(report.result == Plaza2Aggr20AuthorityProbeResult::Yes,
                "fake probe must observe current-session readiness after ONLINE");
        require(report.refdata_listener_created && report.refdata_listener_opened && report.refdata_online &&
                    report.refdata_snapshot_complete,
                "fresh REFDATA must be complete before AGGR selection");
        require(report.selection.has_value(), "probe must select a current futures instrument");
        require(report.selection->sess_id == 321 && report.selection->isin_id == 1001 &&
                    report.selection->symbol == "RTS-6.26" && report.selection->refdata_lifenum == 7,
                "probe selection identity mismatch");
        require(report.selection->fut_instruments_source.has_value() &&
                    report.selection->fut_sess_contents_source.has_value() &&
                    report.selection->session_source.has_value(),
                "probe selection must retain all REFDATA provenance");
        require(report.attempts.size() == 2, "probe must repeat the listener-only AGGR attempt");
        require(report.initial_open_session_data_ready_after_online == std::optional<bool>{true},
                "initial listener result must be reported independently");
        require(report.listener_reopen_session_data_ready_after_online == std::optional<bool>{true},
                "reopened listener result must be reported independently");
        require(report.attempts[0].name == "INITIAL_OPEN" && report.attempts[1].name == "LISTENER_REOPEN",
                "probe attempts must retain stable names");
        for (const auto& attempt : report.attempts) {
            require(attempt.listener_created && attempt.listener_opened && attempt.online && attempt.snapshot_complete,
                    "each AGGR attempt must reach snapshot+ONLINE");
            require(attempt.session_data_ready_after_online && attempt.target_authoritative,
                    "each AGGR attempt must establish target authority after ONLINE");
            require(attempt.experiment_complete, "each AGGR attempt must finish without error");
            require(attempt.sys_events.size() >= 2, "each AGGR attempt must record bootstrap and current sys_events");
            require(attempt.sys_events.front().observed_before_online,
                    "bootstrap sys_event must be recorded before ONLINE");
            require(!attempt.sys_events.back().observed_before_online &&
                        attempt.sys_events.back().transaction_committed,
                    "current sys_event must be recorded after ONLINE and after commit");
            require(attempt.sys_events.front().transaction_id != 0 &&
                        attempt.sys_events.front().transaction_id != attempt.sys_events.back().transaction_id,
                    "sys_events must retain enclosing transaction identities");
            require(contains_trace(attempt, "event=LIFENUM"), "each attempt must retain LifeNum evidence");
            require(contains_trace(attempt, "event=TN_BEGIN"), "each attempt must retain TN_BEGIN evidence");
            require(contains_trace(attempt, "event=TN_COMMIT"), "each attempt must retain TN_COMMIT evidence");
            require(contains_trace(attempt, "event=ONLINE"), "each attempt must retain ONLINE evidence");
            require(contains_trace(attempt, "probe=listener_close_complete"),
                    "each attempt must retain listener close evidence");
            require(contains_trace(attempt, "probe=listener_destroy_complete"),
                    "each attempt must retain listener destroy evidence");
        }

        const auto audit = read_text(audit_path);
        require(audit.find("cg_pub_new") == std::string::npos && audit.find("cg_pub_open") == std::string::npos &&
                    audit.find("cg_pub_msgnew") == std::string::npos && audit.find("cg_pub_post") == std::string::npos,
                "read-only probe must not create or use a publisher");
        require(audit.find("cg_lsn_new") != std::string::npos && audit.find("cg_lsn_open") != std::string::npos,
                "read-only probe must use listener APIs");

        cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
