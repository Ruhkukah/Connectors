#include "adapters/alorengine_capi/moex_c_api_v3.h"

#include "plaza2_runtime_test_support.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
namespace cg = moex::plaza2::cgate;
namespace test = moex::plaza2::test;

struct FakeApi {
    void (*reset)();
    std::uint64_t (*count)(std::uint32_t);
};

FakeApi load_fake(const std::filesystem::path& path, void*& library) {
    library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    test::require(library != nullptr, "load fake runtime");
    auto reset = reinterpret_cast<void (*)()>(dlsym(library, "moex_fake_reset_publisher_counts"));
    auto count = reinterpret_cast<std::uint64_t (*)(std::uint32_t)>(dlsym(library, "moex_fake_publisher_count"));
    test::require(reset != nullptr && count != nullptr, "load fake publisher counters");
    return {reset, count};
}

void set_environment() {
    ::setenv("V3_ENV", "ini=config/t1.ini;key=00000000", 1);
    ::setenv("V3_BROKER", "BRK1", 1);
    ::setenv("V3_CLIENT", "C01", 1);
    ::setenv("V3_CREDENTIALS", "test-only-secret", 1);
    ::setenv("V3_SOFTWARE_KEY", "00000000", 1);
    ::setenv("MOEX_FAKE_ZERO_POSITION", "1", 1);
    ::setenv("MOEX_FAKE_MISSING_ORDER", "1", 1);
    ::setenv("MOEX_FAKE_CLIENT_CODE", "BRK1C01", 1);
    ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
    ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
    ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
    ::setenv("MOEX_FAKE_FLAT_TRADE_REPLAY", "1", 1);
    ::unsetenv("MOEX_FAKE_AGGR_CROSSED");
    ::unsetenv("MOEX_FAKE_STALE_AGGR");
    ::unsetenv("MOEX_FAKE_EXT_ID");
}

struct Strings {
    std::string root;
    std::string library;
    std::string scheme;
    std::string config;
    std::string journal;
    std::string receipt;

    MoexConnectorHostCreateParamsV3 params() const {
        MoexConnectorHostCreateParamsV3 out{};
        out.struct_size = sizeof(out);
        out.abi_version = MOEX_C_ABI_V3_VERSION;
        out.runtime_root = root.c_str();
        out.library_path = library.c_str();
        out.scheme_dir = scheme.c_str();
        out.config_dir = config.c_str();
        out.env_settings_env_var = "V3_ENV";
        out.credentials_env_var = "V3_CREDENTIALS";
        out.software_key_env_var = "V3_SOFTWARE_KEY";
        out.broker_code_env_var = "V3_BROKER";
        out.client_code_env_var = "V3_CLIENT";
        out.expected_release = "SPECTRA93";
        out.isin_id = 1001;
        out.session_id = 321;
        out.armed_test_network = 1;
        out.armed_test_session = 1;
        out.armed_test_plaza2 = 1;
        out.armed_test_order_send = 1;
        out.base_ext_id = 79;
        out.base_add_user_id = 701;
        out.base_cancel_user_id = 702;
        out.base_recovery_user_id = 703;
        out.base_run_id = "cabi-v3-test";
        out.journal_root = journal.c_str();
        out.receipt_path = receipt.c_str();
        out.profile_id = "cabi-v3-profile";
        static const std::string fingerprint(64, 'e');
        out.profile_fingerprint = fingerprint.c_str();
        out.policy_version = "cabi-v3-policy";
        return out;
    }
};

MoexPersistentOrderRequestV3 request(std::uint32_t side, const char* price, const char* comment,
                                     std::int32_t quantity = 1) {
    MoexPersistentOrderRequestV3 out{};
    out.struct_size = sizeof(out);
    out.abi_version = MOEX_C_ABI_V3_VERSION;
    out.side = side;
    out.price = price;
    out.base_contract_code = "RTS";
    out.comment = comment;
    out.quantity = quantity;
    return out;
}

void warm(MoexConnectorHostHandleV3 handle) {
    test::require(moex_v3_start(handle) == MOEX_RESULT_OK, "v3 start");
    MoexPersistentSnapshotV3 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.abi_version = MOEX_C_ABI_V3_VERSION;
    for (int i = 0; i < 20; ++i) {
        test::require(moex_v3_poll(handle) == MOEX_RESULT_OK, "v3 poll");
        snapshot.struct_size = sizeof(snapshot);
        snapshot.abi_version = MOEX_C_ABI_V3_VERSION;
        test::require(moex_v3_get_snapshot(handle, &snapshot) == MOEX_RESULT_OK, "v3 snapshot");
        if (snapshot.observation_ready != 0)
            break;
    }
    test::require(snapshot.observation_ready != 0 && snapshot.new_order_allowed != 0, "v3 host ready");
}

std::string copy_canonical(MoexConnectorHostHandleV3 handle, const MoexPersistentPlanInfoV3& plan) {
    std::uint32_t size = 0;
    test::require(moex_v3_copy_plan_canonical(handle, nullptr, 0, &size) == MOEX_RESULT_BUFFER_TOO_SMALL,
                  "v3 canonical size query");
    test::require(size == plan.canonical_size, "v3 canonical size matches plan");
    std::string result(size, '\0');
    if (size != 0)
        test::require(moex_v3_copy_plan_canonical(handle, result.data(), size, &size) == MOEX_RESULT_OK,
                      "v3 canonical copy");
    return result;
}

MoexPersistentPlanInfoV3 plan(MoexConnectorHostHandleV3 handle, MoexPersistentOrderRequestV3& request_value) {
    MoexPersistentPlanInfoV3 out{};
    out.struct_size = sizeof(out);
    out.abi_version = MOEX_C_ABI_V3_VERSION;
    test::require(moex_v3_plan_order(handle, &request_value, &out) == MOEX_RESULT_OK && out.ok != 0, "v3 plan order");
    return out;
}

void require_result_header(MoexPersistentOrderResultV3& value) {
    value.struct_size = sizeof(value);
    value.abi_version = MOEX_C_ABI_V3_VERSION;
}
} // namespace

int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake runtime path");
        set_environment();
        const auto root = test::make_temp_directory("moex_cabi_v3");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test"));
        Strings strings{fixture.root.string(),       fixture.library_path.string(), fixture.scheme_dir.string(),
                        fixture.config_dir.string(), (root / "journals").string(),  (root / "receipt.json").string()};

        test::require(moex_v3_abi_version() == 3U, "v3 ABI version");
        test::require(moex_v3_sizeof_MoexConnectorHostCreateParamsV3() == sizeof(MoexConnectorHostCreateParamsV3) &&
                          moex_v3_alignof_MoexConnectorHostCreateParamsV3() ==
                              alignof(MoexConnectorHostCreateParamsV3) &&
                          moex_v3_sizeof_MoexPersistentOrderRequestV3() == sizeof(MoexPersistentOrderRequestV3) &&
                          moex_v3_alignof_MoexPersistentOrderRequestV3() == alignof(MoexPersistentOrderRequestV3),
                      "v3 request layout exports");
        test::require(
            moex_v3_sizeof_MoexPersistentSnapshotV3() == sizeof(MoexPersistentSnapshotV3) &&
                moex_v3_alignof_MoexPersistentSnapshotV3() == alignof(MoexPersistentSnapshotV3) &&
                moex_v3_sizeof_MoexPersistentPlanInfoV3() == sizeof(MoexPersistentPlanInfoV3) &&
                moex_v3_alignof_MoexPersistentPlanInfoV3() == alignof(MoexPersistentPlanInfoV3) &&
                moex_v3_sizeof_MoexPersistentOrderResultV3() == sizeof(MoexPersistentOrderResultV3) &&
                moex_v3_alignof_MoexPersistentOrderResultV3() == alignof(MoexPersistentOrderResultV3) &&
                moex_v3_sizeof_MoexPersistentReconciliationResultV3() == sizeof(MoexPersistentReconciliationResultV3) &&
                moex_v3_alignof_MoexPersistentReconciliationResultV3() == alignof(MoexPersistentReconciliationResultV3),
            "v3 all layout exports");

        void* fake_library = nullptr;
        const auto fake = load_fake(fixture.library_path, fake_library);
        fake.reset();
        auto params = strings.params();
        MoexConnectorHostHandleV3 handle = nullptr;
        test::require(moex_v3_create_host(&params, &handle) == MOEX_RESULT_OK, "v3 create");
        warm(handle);

        MoexPersistentSnapshotV3 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        snapshot.abi_version = MOEX_C_ABI_V3_VERSION;
        test::require(moex_v3_get_snapshot(handle, &snapshot) == MOEX_RESULT_OK && snapshot.order_epoch_active == 0 &&
                          snapshot.new_order_allowed != 0 && snapshot.cg_pub_msgnew == 0 && snapshot.cg_pub_post == 0,
                      "initial persistent snapshot");

        auto first_request = request(2, "103000", "v3-sell-a");
        auto first_plan = plan(handle, first_request);
        const auto first_canonical = copy_canonical(handle, first_plan);
        test::require(first_plan.side == 2 && first_plan.quantity == 1 &&
                          first_plan.canonical_size == first_canonical.size(),
                      "epoch one plan terms");

        auto second_request = request(1, "102250", "v3-buy-b");
        auto second_plan = plan(handle, second_request);
        const auto second_canonical = copy_canonical(handle, second_plan);
        test::require(first_plan.plan_sha256[0] != '\0' &&
                          std::string(first_plan.plan_sha256) != second_plan.plan_sha256,
                      "fresh epoch plan hash");
        test::require(moex_v3_begin_order(handle, &first_request, second_canonical.data(), second_canonical.size(),
                                          second_plan.plan_sha256) != MOEX_RESULT_OK,
                      "request A plus canonical B rejected");
        test::require(moex_v3_plan_order(handle, &first_request, &first_plan) == MOEX_RESULT_OK,
                      "restore epoch one cache");
        const auto restored_canonical = copy_canonical(handle, first_plan);
        auto modified = restored_canonical;
        modified[0] = modified[0] == '{' ? '[' : '{';
        test::require(moex_v3_begin_order(handle, &first_request, modified.data(), modified.size(),
                                          first_plan.plan_sha256) != MOEX_RESULT_OK,
                      "modified canonical rejected");
        test::require(moex_v3_begin_order(handle, &first_request, restored_canonical.data(), restored_canonical.size(),
                                          std::string(64, '0').c_str()) != MOEX_RESULT_OK,
                      "wrong SHA rejected");
        test::require(moex_v3_begin_order(handle, &first_request, restored_canonical.data(), restored_canonical.size(),
                                          first_plan.plan_sha256) == MOEX_RESULT_OK,
                      "epoch one begin");

        MoexPersistentOrderResultV3 result{};
        require_result_header(result);
        test::require(moex_v3_submit_order(handle, &result) == MOEX_RESULT_OK && result.ok == 0 &&
                          result.lifecycle_state == MOEX_V3_ORDER_POSTED && result.add_submission.post_invoked != 0,
                      "epoch one submit is domain result");
        const auto posts_after_add = fake.count(1);
        require_result_header(result);
        test::require(moex_v3_submit_order(handle, &result) == MOEX_RESULT_OK && result.ok == 0 &&
                          fake.count(1) == posts_after_add,
                      "second Add refused");
        require_result_header(result);
        test::require(moex_v3_poll_order(handle, &result) == MOEX_RESULT_OK && result.ok == 0 &&
                          result.lifecycle_state == MOEX_V3_ORDER_WORKING,
                      "epoch one Working");
        require_result_header(result);
        test::require(moex_v3_poll_order(handle, &result) == MOEX_RESULT_OK &&
                          result.lifecycle_state == MOEX_V3_ORDER_WORKING && fake.count(1) == posts_after_add,
                      "poll does not auto cancel");
        require_result_header(result);
        test::require(moex_v3_finish_order_epoch(handle) != MOEX_RESULT_OK, "finish Working refused");
        require_result_header(result);
        test::require(moex_v3_cancel_current_order(handle, &result) == MOEX_RESULT_OK && result.ok == 0 &&
                          result.lifecycle_state == MOEX_V3_ORDER_CANCEL_PENDING,
                      "epoch one cancel pending");
        const auto posts_after_cancel = fake.count(1);
        require_result_header(result);
        test::require(moex_v3_cancel_current_order(handle, &result) == MOEX_RESULT_OK && result.ok == 0 &&
                          fake.count(1) == posts_after_cancel,
                      "second cancel refused");
        require_result_header(result);
        test::require(moex_v3_finish_order_epoch(handle) != MOEX_RESULT_OK, "finish CancelPending refused");
        for (int i = 0; i < 4 && result.lifecycle_state != MOEX_V3_ORDER_CANCELLED; ++i) {
            require_result_header(result);
            test::require(moex_v3_poll_order(handle, &result) == MOEX_RESULT_OK, "epoch one terminal poll");
        }
        test::require(result.ok != 0 && result.lifecycle_state == MOEX_V3_ORDER_CANCELLED &&
                          result.market_safe_terminal != 0,
                      "epoch one Cancelled");
        test::require(moex_v3_finish_order_epoch(handle) == MOEX_RESULT_OK, "epoch one finish");

        ::setenv("MOEX_FAKE_EXT_ID", "80", 1);
        ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20103", 1);
        second_plan = plan(handle, second_request);
        const auto fresh_second_canonical = copy_canonical(handle, second_plan);
        test::require(second_plan.side == 1 &&
                          std::string(second_plan.plan_sha256) != std::string(first_plan.plan_sha256),
                      "epoch two BUY plan");
        test::require(moex_v3_begin_order(handle, &second_request, first_canonical.data(), first_canonical.size(),
                                          first_plan.plan_sha256) != MOEX_RESULT_OK,
                      "old epoch canonical refused");
        test::require(moex_v3_begin_order(handle, &second_request, fresh_second_canonical.data(),
                                          fresh_second_canonical.size(), second_plan.plan_sha256) == MOEX_RESULT_OK,
                      "epoch two begin");
        require_result_header(result);
        test::require(moex_v3_submit_order(handle, &result) == MOEX_RESULT_OK, "epoch two submit");
        require_result_header(result);
        test::require(moex_v3_poll_order(handle, &result) == MOEX_RESULT_OK &&
                          result.lifecycle_state == MOEX_V3_ORDER_WORKING,
                      "epoch two Working");
        require_result_header(result);
        test::require(moex_v3_cancel_current_order(handle, &result) == MOEX_RESULT_OK &&
                          result.lifecycle_state == MOEX_V3_ORDER_CANCEL_PENDING,
                      "epoch two cancel");
        for (int i = 0; i < 4 && result.lifecycle_state != MOEX_V3_ORDER_CANCELLED; ++i) {
            require_result_header(result);
            test::require(moex_v3_poll_order(handle, &result) == MOEX_RESULT_OK, "epoch two terminal poll");
        }
        test::require(result.ok != 0 && result.lifecycle_state == MOEX_V3_ORDER_CANCELLED, "epoch two Cancelled");
        test::require(moex_v3_finish_order_epoch(handle) == MOEX_RESULT_OK, "epoch two finish");
        test::require(fake.count(0) == 4 && fake.count(1) == 4,
                      "one Add and one cancel per epoch msgnew=" + std::to_string(fake.count(0)) +
                          " post=" + std::to_string(fake.count(1)));

        // A recovered active checkpoint remains blocked through the V3
        // surface, while reconciliation remains explicitly callable.
        ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
        ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
        auto restart_params = strings.params();
        restart_params.base_run_id = "cabi-v3-restart";
        const auto restart_journals = root / "restart-journals";
        restart_params.journal_root = restart_journals.c_str();
        MoexConnectorHostHandleV3 active = nullptr;
        test::require(moex_v3_create_host(&restart_params, &active) == MOEX_RESULT_OK, "restart active create");
        warm(active);
        auto restart_request = request(2, "103000", "restart");
        MoexPersistentPlanInfoV3 restart_plan{};
        restart_plan.struct_size = sizeof(restart_plan);
        restart_plan.abi_version = MOEX_C_ABI_V3_VERSION;
        test::require(moex_v3_plan_order(active, &restart_request, &restart_plan) == MOEX_RESULT_OK,
                      "restart active plan");
        const auto restart_canonical = copy_canonical(active, restart_plan);
        test::require(moex_v3_begin_order(active, &restart_request, restart_canonical.data(), restart_canonical.size(),
                                          restart_plan.plan_sha256) == MOEX_RESULT_OK,
                      "restart active begin");
        require_result_header(result);
        test::require(moex_v3_submit_order(active, &result) == MOEX_RESULT_OK, "restart active submit");
        test::require(moex_v3_destroy_host(active) == MOEX_RESULT_OK, "restart active destroy");
        MoexConnectorHostHandleV3 recovered = nullptr;
        test::require(moex_v3_create_host(&restart_params, &recovered) == MOEX_RESULT_OK, "recovered create");
        test::require(moex_v3_get_snapshot(recovered, &snapshot) == MOEX_RESULT_OK &&
                          snapshot.order_epoch_active != 0 && snapshot.new_order_allowed == 0,
                      "recovered checkpoint blocks new order");
        test::require(moex_v3_start(recovered) == MOEX_RESULT_OK, "recovered start");
        for (int i = 0; i < 8; ++i)
            (void)moex_v3_poll(recovered);
        MoexPersistentPlanInfoV3 blocked_plan{};
        blocked_plan.struct_size = sizeof(blocked_plan);
        blocked_plan.abi_version = MOEX_C_ABI_V3_VERSION;
        test::require(moex_v3_plan_order(recovered, &restart_request, &blocked_plan) != MOEX_RESULT_OK &&
                          blocked_plan.ok == 0,
                      "recovered checkpoint refuses planning");
        MoexPersistentReconciliationResultV3 reconciliation{};
        reconciliation.struct_size = sizeof(reconciliation);
        reconciliation.abi_version = MOEX_C_ABI_V3_VERSION;
        test::require(moex_v3_reconcile(recovered, &reconciliation) == MOEX_RESULT_OK && reconciliation.run_found != 0,
                      "reconciliation remains callable through V3");
        test::require(moex_v3_destroy_host(recovered) == MOEX_RESULT_OK, "recovered cleanup");
        std::error_code restart_cleanup;
        std::filesystem::remove_all(restart_journals, restart_cleanup);

        // A safely terminal journal may be observed after the process dies
        // before the persistent checkpoint advances.  V3 must expose the
        // same historical recovery path without coupling it to the current
        // market book or allocating another publisher message.
        ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
        ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
        ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
        fake.reset();
        auto terminal_params = strings.params();
        terminal_params.base_run_id = "cabi-v3-terminal";
        const auto terminal_journals = root / "terminal-journals";
        terminal_params.journal_root = terminal_journals.c_str();
        MoexConnectorHostHandleV3 terminal = nullptr;
        test::require(moex_v3_create_host(&terminal_params, &terminal) == MOEX_RESULT_OK, "terminal create");
        warm(terminal);
        auto terminal_request = request(2, "103000", "terminal-recovery");
        MoexPersistentPlanInfoV3 terminal_plan{};
        terminal_plan.struct_size = sizeof(terminal_plan);
        terminal_plan.abi_version = MOEX_C_ABI_V3_VERSION;
        test::require(moex_v3_plan_order(terminal, &terminal_request, &terminal_plan) == MOEX_RESULT_OK,
                      "terminal plan");
        const auto terminal_canonical = copy_canonical(terminal, terminal_plan);
        test::require(moex_v3_begin_order(terminal, &terminal_request, terminal_canonical.data(),
                                          terminal_canonical.size(), terminal_plan.plan_sha256) == MOEX_RESULT_OK,
                      "terminal begin");
        require_result_header(result);
        test::require(moex_v3_submit_order(terminal, &result) == MOEX_RESULT_OK, "terminal submit");
        require_result_header(result);
        test::require(moex_v3_poll_order(terminal, &result) == MOEX_RESULT_OK &&
                          (result.lifecycle_state == MOEX_V3_ORDER_WORKING ||
                           result.lifecycle_state == MOEX_V3_ORDER_PARTIALLY_FILLED),
                      "terminal reaches Working");
        require_result_header(result);
        test::require(moex_v3_cancel_current_order(terminal, &result) == MOEX_RESULT_OK &&
                          result.lifecycle_state == MOEX_V3_ORDER_CANCEL_PENDING,
                      "terminal cancel pending");
        for (int i = 0; i < 4 && result.lifecycle_state != MOEX_V3_ORDER_CANCELLED; ++i) {
            require_result_header(result);
            test::require(moex_v3_poll_order(terminal, &result) == MOEX_RESULT_OK, "terminal cancelled poll");
        }
        test::require(result.ok != 0 && result.lifecycle_state == MOEX_V3_ORDER_CANCELLED &&
                          result.market_safe_terminal != 0,
                      "terminal cancelled safely");
        const auto terminal_msgnew = fake.count(0);
        const auto terminal_post = fake.count(1);
        test::require(terminal_msgnew == 2 && terminal_post == 2, "terminal publisher counts");
        test::require(moex_v3_destroy_host(terminal) == MOEX_RESULT_OK, "terminal simulated crash");

        MoexConnectorHostHandleV3 terminal_recovered = nullptr;
        test::require(moex_v3_create_host(&terminal_params, &terminal_recovered) == MOEX_RESULT_OK,
                      "terminal recovered create");
        test::require(moex_v3_get_snapshot(terminal_recovered, &snapshot) == MOEX_RESULT_OK &&
                          snapshot.order_epoch_active != 0 && snapshot.new_order_allowed == 0,
                      "terminal recovery initially blocks new order");
        ::setenv("MOEX_FAKE_AGGR_CROSSED", "1", 1);
        test::require(moex_v3_start(terminal_recovered) == MOEX_RESULT_OK, "terminal recovered start");
        for (int i = 0; i < 10; ++i) {
            test::require(moex_v3_poll(terminal_recovered) == MOEX_RESULT_OK, "terminal recovered poll");
            test::require(moex_v3_get_snapshot(terminal_recovered, &snapshot) == MOEX_RESULT_OK,
                          "terminal recovered snapshot");
            if (snapshot.private_streams_ready != 0 && snapshot.trade_replay_complete != 0)
                break;
        }
        test::require(snapshot.private_streams_ready != 0 && snapshot.trade_replay_complete != 0,
                      "terminal recovery has private evidence");
        MoexPersistentReconciliationResultV3 terminal_reconciliation{};
        terminal_reconciliation.struct_size = sizeof(terminal_reconciliation);
        terminal_reconciliation.abi_version = MOEX_C_ABI_V3_VERSION;
        test::require(moex_v3_reconcile(terminal_recovered, &terminal_reconciliation) == MOEX_RESULT_OK &&
                          terminal_reconciliation.ok != 0 && terminal_reconciliation.run_found != 0 &&
                          terminal_reconciliation.resolved != 0 && terminal_reconciliation.locks_retained == 0 &&
                          terminal_reconciliation.lifecycle_state == MOEX_V3_ORDER_CANCELLED,
                      "terminal recovery resolves through V3");
        test::require(moex_v3_get_snapshot(terminal_recovered, &snapshot) == MOEX_RESULT_OK &&
                          snapshot.order_epoch_active == 0 && snapshot.new_order_allowed == 0 &&
                          fake.count(0) == terminal_msgnew && fake.count(1) == terminal_post,
                      "terminal recovery has no publisher calls");
        test::require(moex_v3_stop(terminal_recovered) == MOEX_RESULT_OK &&
                          moex_v3_destroy_host(terminal_recovered) == MOEX_RESULT_OK,
                      "terminal recovery cleanup");
        ::unsetenv("MOEX_FAKE_AGGR_CROSSED");
        ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
        std::error_code terminal_cleanup;
        std::filesystem::remove_all(terminal_journals, terminal_cleanup);

        // Fixed policy rejects quantity other than one before any publisher call.
        auto bad_quantity = request(2, "103000", "bad-quantity", 2);
        MoexPersistentPlanInfoV3 bad_plan{};
        bad_plan.struct_size = sizeof(bad_plan);
        bad_plan.abi_version = MOEX_C_ABI_V3_VERSION;
        test::require(moex_v3_plan_order(handle, &bad_quantity, &bad_plan) == MOEX_RESULT_INVALID_ARGUMENT,
                      "quantity != 1 rejected");

        test::require(moex_v3_stop(handle) == MOEX_RESULT_OK, "v3 stop");
        test::require(moex_v3_destroy_host(handle) == MOEX_RESULT_OK, "v3 destroy");

        // A crossed market blocks planning without a publisher call.
        ::setenv("MOEX_FAKE_AGGR_CROSSED", "1", 1);
        auto crossed_params = strings.params();
        crossed_params.base_run_id = "cabi-v3-crossed";
        const auto crossed_journals = root / "crossed-journals";
        crossed_params.journal_root = crossed_journals.c_str();
        MoexConnectorHostHandleV3 crossed = nullptr;
        test::require(moex_v3_create_host(&crossed_params, &crossed) == MOEX_RESULT_OK, "crossed create");
        test::require(moex_v3_start(crossed) == MOEX_RESULT_OK, "crossed start");
        for (int i = 0; i < 8; ++i)
            (void)moex_v3_poll(crossed);
        auto crossed_request = request(2, "103000", "crossed");
        MoexPersistentPlanInfoV3 crossed_plan{};
        crossed_plan.struct_size = sizeof(crossed_plan);
        crossed_plan.abi_version = MOEX_C_ABI_V3_VERSION;
        (void)moex_v3_plan_order(crossed, &crossed_request, &crossed_plan);
        test::require(crossed_plan.ok == 0, "crossed plan fails closed");
        test::require(moex_v3_get_snapshot(crossed, &snapshot) == MOEX_RESULT_OK && snapshot.cg_pub_msgnew == 0 &&
                          snapshot.cg_pub_post == 0,
                      "crossed plan has no publisher calls");
        test::require(moex_v3_stop(crossed) == MOEX_RESULT_OK && moex_v3_destroy_host(crossed) == MOEX_RESULT_OK,
                      "crossed cleanup");
        ::unsetenv("MOEX_FAKE_AGGR_CROSSED");

        dlclose(fake_library);
        test::remove_tree(root);
        std::cout << "moex C ABI v3 test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
