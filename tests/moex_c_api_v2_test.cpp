#include "adapters/alorengine_capi/moex_c_api_v2.h"

#include "moex/connector_host/operator_config.hpp"
#include "moex/plaza2_trade/plaza2_order_lifecycle.hpp"
#include "plaza2_runtime_test_support.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace cg = moex::plaza2::cgate;
namespace test = moex::plaza2::test;
using moex::connector_host::ConnectorHost;
using moex::connector_host::HostPurpose;
using moex::connector_host::Plaza2HostConfigInputs;
using moex::plaza2_trade::Plaza2TradeSide;

struct FakeApi {
    void (*reset)();
    std::uint64_t (*count)(std::uint32_t);
};

FakeApi fake_api(const std::filesystem::path& library_path, void*& library) {
    library = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    test::require(library != nullptr, "load fake runtime");
    auto reset = reinterpret_cast<void (*)()>(dlsym(library, "moex_fake_reset_publisher_counts"));
    auto count = reinterpret_cast<std::uint64_t (*)(std::uint32_t)>(dlsym(library, "moex_fake_publisher_count"));
    test::require(reset != nullptr && count != nullptr, "load fake publisher counters");
    return {reset, count};
}

void set_common_environment() {
    ::setenv("V2_ENV", "ini=config/t1.ini;key=00000000", 1);
    ::setenv("V2_BROKER", "BRK1", 1);
    ::setenv("V2_CLIENT", "C01", 1);
    ::setenv("V2_CREDENTIALS", "test-only-secret", 1);
    ::setenv("V2_SOFTWARE_KEY", "00000000", 1);
    ::setenv("MOEX_FAKE_ZERO_POSITION", "1", 1);
    ::setenv("MOEX_FAKE_MISSING_ORDER", "1", 1);
    ::setenv("MOEX_FAKE_CLIENT_CODE", "BRK1C01", 1);
    ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
    ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
    ::unsetenv("MOEX_FAKE_NONTRADABLE_SESSION");
    ::unsetenv("MOEX_FAKE_NONTRADABLE_INSTRUMENT");
    ::unsetenv("MOEX_FAKE_AGGR_CROSSED");
    ::unsetenv("MOEX_FAKE_STALE_AGGR");
    ::unsetenv("MOEX_FAKE_MISSING_LIMITS");
    ::unsetenv("MOEX_FAKE_MISSING_POSITION");
}

struct ParamsStrings {
    std::string runtime_root;
    std::string library_path;
    std::string scheme_dir;
    std::string config_dir;
    std::string expected_release{"SPECTRA93"};
    std::string price{"103000"};
    std::string base_contract{"RTS"};
    std::string comment{"cabi-v2"};
    std::string run_id{"cabi-v2-test"};
    std::string journal_root;
    std::string receipt_path;
    std::string profile_id{"cabi-v2-profile"};
    std::string profile_fingerprint = std::string(64, 'e');
    std::string policy_version{"cabi-v2-policy"};
    std::string policy_sha256;

    MoexConnectorHostCreateParamsV2 make(bool order_test) const {
        MoexConnectorHostCreateParamsV2 params{};
        params.struct_size = sizeof(params);
        params.abi_version = MOEX_C_ABI_V2_VERSION;
        params.purpose = order_test ? MOEX_V2_PURPOSE_ORDER_TEST : MOEX_V2_PURPOSE_QUALIFY;
        params.runtime_root = runtime_root.c_str();
        params.library_path = library_path.c_str();
        params.scheme_dir = scheme_dir.c_str();
        params.config_dir = config_dir.c_str();
        params.env_settings_env_var = "V2_ENV";
        params.credentials_env_var = "V2_CREDENTIALS";
        params.software_key_env_var = "V2_SOFTWARE_KEY";
        params.broker_code_env_var = "V2_BROKER";
        params.client_code_env_var = "V2_CLIENT";
        params.expected_release = expected_release.c_str();
        params.isin_id = 1001;
        params.session_id = 321;
        params.armed_test_network = 1;
        params.armed_test_session = 1;
        params.armed_test_plaza2 = 1;
        params.armed_test_order_send = order_test ? 1 : 0;
        params.side = 2;
        params.price = price.c_str();
        params.base_contract_code = base_contract.c_str();
        params.comment = comment.c_str();
        params.ext_id = 79;
        params.add_user_id = 701;
        params.cancel_user_id = 702;
        params.recovery_user_id = 703;
        params.run_id = run_id.c_str();
        params.journal_root = journal_root.c_str();
        params.receipt_path = receipt_path.c_str();
        params.profile_id = profile_id.c_str();
        params.profile_fingerprint = profile_fingerprint.c_str();
        params.policy_version = policy_version.c_str();
        params.policy_sha256 = policy_sha256.c_str();
        return params;
    }
};

void pump(MoexConnectorHostHandleV2 handle, MoexConnectorHostSnapshotV2& snapshot) {
    for (unsigned i = 0; i < 30; ++i) {
        test::require(moex_v2_poll(handle) == MOEX_RESULT_OK, "v2 poll");
        snapshot.struct_size = sizeof(snapshot);
        snapshot.abi_version = MOEX_C_ABI_V2_VERSION;
        test::require(moex_v2_get_snapshot(handle, &snapshot) == MOEX_RESULT_OK, "v2 snapshot");
        if (snapshot.observation_ready != 0)
            return;
    }
}

std::string canonical_from_v2(MoexConnectorHostHandleV2 handle, MoexPreSendPlanInfoV2& info) {
    info.struct_size = sizeof(info);
    info.abi_version = MOEX_C_ABI_V2_VERSION;
    test::require(moex_v2_get_plan_info(handle, &info) == MOEX_RESULT_OK, "v2 plan info");
    std::uint32_t required = 0;
    test::require(moex_v2_copy_plan_canonical(handle, nullptr, 0, &required) == MOEX_RESULT_BUFFER_TOO_SMALL,
                  "v2 canonical size query");
    std::string canonical(required, '\0');
    test::require(moex_v2_copy_plan_canonical(handle, canonical.data(), required, &required) == MOEX_RESULT_OK,
                  "v2 canonical copy");
    return canonical;
}

void require_qualification_refusal(const ParamsStrings& strings, const char* flag) {
    ::setenv(flag, "1", 1);
    auto params = strings.make(false);
    MoexConnectorHostHandleV2 handle = nullptr;
    test::require(moex_v2_create_host(&params, &handle) == MOEX_RESULT_OK, "failure-case qualify create");
    test::require(moex_v2_start(handle) == MOEX_RESULT_OK, "failure-case qualify start");
    MoexConnectorHostSnapshotV2 snapshot{};
    pump(handle, snapshot);
    test::require(snapshot.observation_ready == 0 && snapshot.cg_pub_msgnew == 0 && snapshot.cg_pub_post == 0,
                  std::string("failure-case readiness must fail closed without sending: ") + flag +
                      " active=" + std::to_string(snapshot.active_own_order_count));
    test::require(moex_v2_stop(handle) == MOEX_RESULT_OK, "failure-case qualify stop");
    test::require(moex_v2_destroy_host(handle) == MOEX_RESULT_OK, "failure-case qualify destroy");
    ::unsetenv(flag);
}

void require_qualification_refusal_with_params(MoexConnectorHostCreateParamsV2 params, const char* label) {
    MoexConnectorHostHandleV2 handle = nullptr;
    test::require(moex_v2_create_host(&params, &handle) == MOEX_RESULT_OK, std::string(label) + " create");
    test::require(moex_v2_start(handle) == MOEX_RESULT_OK, std::string(label) + " start");
    MoexConnectorHostSnapshotV2 snapshot{};
    pump(handle, snapshot);
    test::require(snapshot.observation_ready == 0 && snapshot.cg_pub_msgnew == 0 && snapshot.cg_pub_post == 0,
                  std::string(label) + " must fail closed without sending");
    test::require(moex_v2_stop(handle) == MOEX_RESULT_OK, std::string(label) + " stop");
    test::require(moex_v2_destroy_host(handle) == MOEX_RESULT_OK, std::string(label) + " destroy");
}

MoexOrderTestResultV2 run_authorized_order_case(MoexConnectorHostCreateParamsV2 params) {
    MoexConnectorHostHandleV2 handle = nullptr;
    test::require(moex_v2_create_host(&params, &handle) == MOEX_RESULT_OK, "order-case create");
    test::require(moex_v2_start(handle) == MOEX_RESULT_OK, "order-case start");
    MoexConnectorHostSnapshotV2 snapshot{};
    pump(handle, snapshot);
    test::require(snapshot.observation_ready != 0, "order-case ready");
    MoexPreSendPlanInfoV2 info{};
    const auto canonical = canonical_from_v2(handle, info);
    test::require(moex_v2_authorize(handle, canonical.data(), static_cast<std::uint32_t>(canonical.size()),
                                    info.plan_sha256) == MOEX_RESULT_OK,
                  "order-case authorization");
    MoexOrderTestResultV2 result{};
    result.struct_size = sizeof(result);
    result.abi_version = MOEX_C_ABI_V2_VERSION;
    test::require(moex_v2_run_order_test(handle, &result) == MOEX_RESULT_OK, "order-case run");
    test::require(moex_v2_stop(handle) == MOEX_RESULT_OK, "order-case stop");
    test::require(moex_v2_destroy_host(handle) == MOEX_RESULT_OK, "order-case destroy");
    return result;
}

void require_active_order_refusal(const ParamsStrings& strings) {
    ::unsetenv("MOEX_FAKE_MISSING_ORDER");
    auto params = strings.make(false);
    MoexConnectorHostHandleV2 handle = nullptr;
    test::require(moex_v2_create_host(&params, &handle) == MOEX_RESULT_OK, "active-order create");
    test::require(moex_v2_start(handle) == MOEX_RESULT_OK, "active-order start");
    MoexConnectorHostSnapshotV2 snapshot{};
    pump(handle, snapshot);
    test::require(snapshot.observation_ready == 0 && snapshot.active_own_order_count != 0 &&
                      snapshot.cg_pub_msgnew == 0 && snapshot.cg_pub_post == 0,
                  "active own USERORDERBOOK order must block readiness");
    test::require(moex_v2_stop(handle) == MOEX_RESULT_OK, "active-order stop");
    test::require(moex_v2_destroy_host(handle) == MOEX_RESULT_OK, "active-order destroy");
    ::setenv("MOEX_FAKE_MISSING_ORDER", "1", 1);
}

void require_order_test_create_rejection(MoexConnectorHostCreateParamsV2 params, const char* label) {
    MoexConnectorHostHandleV2 handle = nullptr;
    test::require(moex_v2_create_host(&params, &handle) == MOEX_RESULT_INVALID_ARGUMENT && handle == nullptr,
                  std::string("OrderTest missing explicit field rejected: ") + label);
}

Plaza2HostConfigInputs direct_inputs(const ParamsStrings& strings, const std::filesystem::path& fixture_root,
                                     HostPurpose purpose) {
    Plaza2HostConfigInputs inputs;
    inputs.purpose = purpose;
    inputs.runtime_root = fixture_root;
    inputs.library_path = strings.library_path;
    inputs.scheme_dir = strings.scheme_dir;
    inputs.config_dir = strings.config_dir;
    inputs.env_open_settings = "ini=config/t1.ini;key=00000000";
    inputs.credentials_env_var = "V2_CREDENTIALS";
    inputs.software_key_env_var = "V2_SOFTWARE_KEY";
    inputs.expected_spectra_release = strings.expected_release;
    inputs.broker_code = "BRK1";
    inputs.client_code = "C01";
    inputs.isin_id = 1001;
    inputs.session_id = 321;
    inputs.arm_state = {.test_network_armed = true,
                        .test_session_armed = true,
                        .test_plaza2_armed = true,
                        .test_order_send_armed = purpose == HostPurpose::OrderTest};
    inputs.publisher_name = purpose == HostPurpose::OrderTest ? "direct-order" : "direct-qualify";
    inputs.profile_id = strings.profile_id;
    inputs.profile_fingerprint = strings.profile_fingerprint;
    inputs.policy_version = strings.policy_version;
    inputs.policy_sha256 = strings.policy_sha256;
    inputs.base_contract_code = strings.base_contract;
    inputs.price = strings.price;
    inputs.comment = strings.comment;
    inputs.side = Plaza2TradeSide::Sell;
    inputs.ext_id = 79;
    inputs.add_user_id = 701;
    inputs.cancel_user_id = 702;
    inputs.recovery_user_id = 703;
    inputs.run_id = strings.run_id;
    inputs.journal_root = fixture_root / "direct-journals";
    inputs.receipt_path = fixture_root / "direct-receipt.json";
    return inputs;
}
} // namespace

int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake runtime path");
        set_common_environment();
        const auto root = test::make_temp_directory("moex_cabi_v2");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test"));
        ParamsStrings strings{.runtime_root = fixture.root.string(),
                              .library_path = fixture.library_path.string(),
                              .scheme_dir = fixture.scheme_dir.string(),
                              .config_dir = fixture.config_dir.string(),
                              .journal_root = (root / "journals").string(),
                              .receipt_path = (root / "receipt.json").string()};
        strings.policy_sha256 = cg::plaza2_sha256_hex(strings.policy_version + ":qty1:distance4:age5000:zero");

        test::require(moex_phase0_abi_version() == 1U, "v1 ABI version preserved");
        test::require(moex_v2_abi_version() == 2U, "v2 ABI version");
        test::require(moex_v2_sizeof_MoexConnectorHostSnapshotV2() == sizeof(MoexConnectorHostSnapshotV2) &&
                          moex_v2_alignof_MoexConnectorHostSnapshotV2() == alignof(MoexConnectorHostSnapshotV2),
                      "snapshot layout export");
        test::require(moex_v2_sizeof_MoexConnectorHostCreateParamsV2() == sizeof(MoexConnectorHostCreateParamsV2) &&
                          moex_v2_alignof_MoexConnectorHostCreateParamsV2() == alignof(MoexConnectorHostCreateParamsV2),
                      "create layout export");
        test::require(
            moex_v2_sizeof_MoexV2StreamHealth() == sizeof(MoexV2StreamHealth) &&
                moex_v2_alignof_MoexV2StreamHealth() == alignof(MoexV2StreamHealth) &&
                moex_v2_sizeof_MoexV2TargetProvenance() == sizeof(MoexV2TargetProvenance) &&
                moex_v2_alignof_MoexV2TargetProvenance() == alignof(MoexV2TargetProvenance) &&
                moex_v2_sizeof_MoexPreSendPlanInfoV2() == sizeof(MoexPreSendPlanInfoV2) &&
                moex_v2_alignof_MoexPreSendPlanInfoV2() == alignof(MoexPreSendPlanInfoV2) &&
                moex_v2_sizeof_MoexV2ReplyInfo() == sizeof(MoexV2ReplyInfo) &&
                moex_v2_alignof_MoexV2ReplyInfo() == alignof(MoexV2ReplyInfo) &&
                moex_v2_sizeof_MoexV2SubmissionInfo() == sizeof(MoexV2SubmissionInfo) &&
                moex_v2_alignof_MoexV2SubmissionInfo() == alignof(MoexV2SubmissionInfo) &&
                moex_v2_sizeof_MoexOrderTestResultV2() == sizeof(MoexOrderTestResultV2) &&
                moex_v2_alignof_MoexOrderTestResultV2() == alignof(MoexOrderTestResultV2) &&
                moex_v2_sizeof_MoexRestartReconciliationResultV2() == sizeof(MoexRestartReconciliationResultV2) &&
                moex_v2_alignof_MoexRestartReconciliationResultV2() == alignof(MoexRestartReconciliationResultV2),
            "all V2 layout exports");
        test::require(moex_v2_create_host(nullptr, nullptr) == MOEX_RESULT_INVALID_ARGUMENT, "null create rejected");

        auto qualify_params = strings.make(false);
        MoexConnectorHostCreateParamsV2 bad = qualify_params;
        bad.struct_size = sizeof(bad) - 1;
        MoexConnectorHostHandleV2 handle = nullptr;
        test::require(moex_v2_create_host(&bad, &handle) == MOEX_RESULT_INVALID_ARGUMENT, "bad size rejected");
        bad = qualify_params;
        bad.abi_version = 1;
        test::require(moex_v2_create_host(&bad, &handle) == MOEX_RESULT_INVALID_ARGUMENT, "bad version rejected");
        test::require(moex_v2_create_host(&qualify_params, &handle) == MOEX_RESULT_OK, "qualify create");
        // The create call copied all strings; changing caller-owned storage must not
        // alter the host configuration retained by the opaque handle.
        strings.runtime_root.assign("caller-storage-mutated");
        test::require(moex_v2_start(handle) == MOEX_RESULT_OK, "qualify start");
        MoexConnectorHostSnapshotV2 snapshot{};
        pump(handle, snapshot);
        test::require(snapshot.observation_ready != 0 && snapshot.publisher_ready != 0 && snapshot.reply_ready != 0 &&
                          snapshot.target_refdata_provenance_ready != 0 && snapshot.cg_pub_msgnew == 0 &&
                          snapshot.cg_pub_post == 0,
                      "qualify snapshot through V2");
        MoexPreSendPlanInfoV2 qualify_plan{};
        const auto qualify_canonical = canonical_from_v2(handle, qualify_plan);
        test::require(qualify_canonical.size() == qualify_plan.canonical_size, "qualify canonical plan");
        test::require(moex_v2_authorize(handle, nullptr, 0, strings.policy_sha256.c_str()) != MOEX_RESULT_OK,
                      "qualify authorization unavailable");
        MoexOrderTestResultV2 qualify_result{};
        qualify_result.struct_size = sizeof(qualify_result);
        qualify_result.abi_version = MOEX_C_ABI_V2_VERSION;
        test::require(moex_v2_run_order_test(handle, &qualify_result) == MOEX_RESULT_OK && !qualify_result.ok &&
                          snapshot.cg_pub_msgnew == 0 && snapshot.cg_pub_post == 0,
                      "qualify order-test unavailable");
        test::require(moex_v2_stop(handle) == MOEX_RESULT_OK, "qualify stop");
        test::require(moex_v2_destroy_host(handle) == MOEX_RESULT_OK, "qualify destroy");
        strings.runtime_root = fixture.root.string();

        // Compare the byte-exact plan returned by the ABI with the direct
        // ConnectorHost plan built from the same typed configuration.
        {
            auto direct_config = moex::connector_host::build_plaza2_host_config(
                direct_inputs(strings, fixture.root, HostPurpose::Qualify));
            ConnectorHost direct(std::move(direct_config));
            test::require(!direct.start(), "direct qualify start");
            for (unsigned i = 0; i < 30 && !direct.snapshot().observation_ready; ++i)
                test::require(!direct.poll(), "direct qualify poll");
            test::require(direct.snapshot().observation_ready, "direct qualify ready");
            const auto direct_plan = direct.plan();
            test::require(direct_plan.ok && direct_plan.canonical_json == qualify_canonical &&
                              direct_plan.sha256 == std::string(qualify_plan.plan_sha256),
                          "V2 canonical plan equals direct ConnectorHost");
            test::require(!direct.stop(), "direct qualify stop");
        }

        {
            auto send_armed = strings.make(false);
            send_armed.armed_test_order_send = 1;
            test::require(moex_v2_create_host(&send_armed, &handle) == MOEX_RESULT_OK, "send-arm create");
            test::require(moex_v2_start(handle) != MOEX_RESULT_OK, "qualify send arm refused");
            test::require(moex_v2_destroy_host(handle) == MOEX_RESULT_OK, "send-arm destroy");
        }
        {
            auto missing_arm = strings.make(true);
            missing_arm.armed_test_plaza2 = 0;
            test::require(moex_v2_create_host(&missing_arm, &handle) == MOEX_RESULT_OK, "missing-arm create");
            test::require(moex_v2_start(handle) != MOEX_RESULT_OK, "missing TEST arm refused");
            test::require(moex_v2_destroy_host(handle) == MOEX_RESULT_OK, "missing-arm destroy");
        }
        require_qualification_refusal(strings, "MOEX_FAKE_AGGR_CROSSED");
        require_qualification_refusal(strings, "MOEX_FAKE_MISSING_POSITION");
        require_active_order_refusal(strings);
        ::unsetenv("MOEX_FAKE_ZERO_POSITION");
        require_qualification_refusal_with_params(strings.make(false), "non-zero position");
        ::setenv("MOEX_FAKE_ZERO_POSITION", "1", 1);
        auto wrong_target = strings.make(false);
        wrong_target.isin_id = 999999;
        require_qualification_refusal_with_params(wrong_target, "wrong target");
        auto wrong_session = strings.make(false);
        wrong_session.session_id = 999999;
        require_qualification_refusal_with_params(wrong_session, "wrong session");

        // QUALIFY may omit executable order identity/receipt fields; the host
        // supplies inert values because it cannot authorize or submit.
        auto qualify_without_order_fields = strings.make(false);
        qualify_without_order_fields.side = 0;
        qualify_without_order_fields.price = nullptr;
        qualify_without_order_fields.base_contract_code = nullptr;
        qualify_without_order_fields.ext_id = 0;
        qualify_without_order_fields.add_user_id = 0;
        qualify_without_order_fields.cancel_user_id = 0;
        qualify_without_order_fields.recovery_user_id = 0;
        qualify_without_order_fields.run_id = nullptr;
        qualify_without_order_fields.journal_root = nullptr;
        qualify_without_order_fields.receipt_path = nullptr;
        qualify_without_order_fields.profile_id = nullptr;
        qualify_without_order_fields.profile_fingerprint = nullptr;
        MoexConnectorHostHandleV2 qualify_without_handle = nullptr;
        test::require(moex_v2_create_host(&qualify_without_order_fields, &qualify_without_handle) == MOEX_RESULT_OK,
                      "qualify omits order fields");
        test::require(moex_v2_destroy_host(qualify_without_handle) == MOEX_RESULT_OK,
                      "qualify without order fields destroy");

        auto missing_price = strings.make(true);
        missing_price.price = nullptr;
        require_order_test_create_rejection(missing_price, "price");
        auto missing_base_contract = strings.make(true);
        missing_base_contract.base_contract_code = nullptr;
        require_order_test_create_rejection(missing_base_contract, "base contract");
        auto missing_ext_id = strings.make(true);
        missing_ext_id.ext_id = 0;
        require_order_test_create_rejection(missing_ext_id, "ext id");
        auto missing_add_user = strings.make(true);
        missing_add_user.add_user_id = 0;
        require_order_test_create_rejection(missing_add_user, "add user id");
        auto missing_cancel_user = strings.make(true);
        missing_cancel_user.cancel_user_id = 0;
        require_order_test_create_rejection(missing_cancel_user, "cancel user id");
        auto missing_recovery_user = strings.make(true);
        missing_recovery_user.recovery_user_id = 0;
        require_order_test_create_rejection(missing_recovery_user, "recovery user id");
        auto missing_run_id = strings.make(true);
        missing_run_id.run_id = nullptr;
        require_order_test_create_rejection(missing_run_id, "run id");
        auto missing_journal_root = strings.make(true);
        missing_journal_root.journal_root = nullptr;
        require_order_test_create_rejection(missing_journal_root, "journal root");
        auto missing_receipt = strings.make(true);
        missing_receipt.receipt_path = nullptr;
        require_order_test_create_rejection(missing_receipt, "receipt path");
        auto missing_profile_id = strings.make(true);
        missing_profile_id.profile_id = nullptr;
        require_order_test_create_rejection(missing_profile_id, "profile id");
        auto missing_profile_fingerprint = strings.make(true);
        missing_profile_fingerprint.profile_fingerprint = nullptr;
        require_order_test_create_rejection(missing_profile_fingerprint, "profile fingerprint");

        auto order_params = strings.make(true);
        test::require(moex_v2_create_host(&order_params, &handle) == MOEX_RESULT_OK, "order-test create");
        test::require(moex_v2_start(handle) == MOEX_RESULT_OK, "order-test start");
        pump(handle, snapshot);
        test::require(snapshot.observation_ready != 0, "order-test ready");
        MoexPreSendPlanInfoV2 info{};
        const auto canonical = canonical_from_v2(handle, info);
        test::require(info.ok != 0 && info.canonical_size == canonical.size() && info.plan_sha256[0] != '\0',
                      "plan info fields");
        auto modified = canonical;
        modified[0] = modified[0] == '{' ? '[' : '{';
        test::require(moex_v2_authorize(handle, modified.data(), static_cast<std::uint32_t>(modified.size()),
                                        info.plan_sha256) != MOEX_RESULT_OK,
                      "modified canonical refused");
        auto pretty = std::string("\n") + canonical;
        test::require(moex_v2_authorize(handle, pretty.data(), static_cast<std::uint32_t>(pretty.size()),
                                        info.plan_sha256) != MOEX_RESULT_OK,
                      "non-exact canonical refused");
        test::require(moex_v2_authorize(handle, canonical.data(), static_cast<std::uint32_t>(canonical.size()),
                                        std::string(64, '0').c_str()) != MOEX_RESULT_OK,
                      "wrong SHA refused");
        test::require(moex_v2_authorize(handle, canonical.data(), static_cast<std::uint32_t>(canonical.size()),
                                        info.plan_sha256) == MOEX_RESULT_OK,
                      "exact plan authorization");
        FakeApi fake{};
        void* library = nullptr;
        fake = fake_api(fixture.library_path, library);
        fake.reset();
        MoexOrderTestResultV2 result{};
        result.struct_size = sizeof(result);
        result.abi_version = MOEX_C_ABI_V2_VERSION;
        test::require(moex_v2_run_order_test(handle, &result) == MOEX_RESULT_OK && result.ok != 0 &&
                          result.lifecycle_state == 8U && result.market_safe_terminal != 0 &&
                          result.evidence_consistent != 0 && result.add_submission.post_invoked != 0 &&
                          result.cancel_submission.post_invoked != 0 && result.recovery_submission.post_invoked == 0 &&
                          fake.count(0) == 2 && fake.count(1) == 2,
                      "V2 bounded Add Working Cancelled lifecycle");
        const auto posts = fake.count(1);
        MoexOrderTestResultV2 second{};
        second.struct_size = sizeof(second);
        second.abi_version = MOEX_C_ABI_V2_VERSION;
        test::require(moex_v2_run_order_test(handle, &second) == MOEX_RESULT_OK && second.ok == 0 &&
                          fake.count(1) == posts,
                      "second order-test refused");
        const auto journal_root = std::filesystem::path(strings.journal_root);
        std::error_code cleanup_error;
        std::filesystem::remove_all(journal_root / strings.run_id, cleanup_error);
        test::require(!cleanup_error, "remove completed one-shot journal before orphan-lock fixture");
        const auto active_root = journal_root / "active";
        std::filesystem::create_directories(active_root);
        for (const auto* name : {"ext_79", "user_701", "user_702", "user_703"})
            std::filesystem::create_directory(active_root / name);
        MoexRestartReconciliationResultV2 reconciliation{};
        reconciliation.struct_size = sizeof(reconciliation);
        reconciliation.abi_version = MOEX_C_ABI_V2_VERSION;
        test::require(moex_v2_reconcile(handle, &reconciliation) == MOEX_RESULT_OK && reconciliation.run_found != 0 &&
                          reconciliation.locks_retained != 0,
                      "V2 reconciliation retains unfinished fake locks");
        test::require(moex_v2_stop(handle) == MOEX_RESULT_OK, "order-test stop");
        test::require(moex_v2_destroy_host(handle) == MOEX_RESULT_OK, "order-test destroy");

        // Receipt persistence is a pre-send gate: a path that resolves to an
        // existing directory must fail before any publisher allocation/post.
        {
            auto receipt_failure = strings;
            receipt_failure.run_id = "cabi-v2-receipt-failure";
            receipt_failure.journal_root = (root / "receipt-failure-journals").string();
            receipt_failure.receipt_path = fixture.config_dir.string();
            auto receipt_params = receipt_failure.make(true);
            fake.reset();
            const auto result = run_authorized_order_case(receipt_params);
            test::require(result.add_submission.post_invoked == 0 && result.lifecycle_state == 0U &&
                              fake.count(0) == 0 && fake.count(1) == 0,
                          "receipt persistence failure must block publisher calls: ok=" + std::to_string(result.ok) +
                              " post_invoked=" + std::to_string(result.add_submission.post_invoked) +
                              " msgnew=" + std::to_string(fake.count(0)) + " post=" + std::to_string(fake.count(1)) +
                              " message=" + std::string(result.message));
        }

        // An uncertain Add remains a single operation with one bounded
        // recovery path; V2 must not turn it into an Add retry.
        {
            ::setenv("MOEX_FAKE_PUB_REPLY_MODE", "timeout", 1);
            auto uncertain = strings;
            uncertain.run_id = "cabi-v2-uncertain-add";
            uncertain.journal_root = (root / "uncertain-journals").string();
            uncertain.receipt_path = (root / "uncertain-receipt.json").string();
            auto uncertain_params = uncertain.make(true);
            fake.reset();
            const auto result = run_authorized_order_case(uncertain_params);
            test::require(!result.ok && result.add_submission.certainty == MOEX_V2_SUBMISSION_POSTED &&
                              result.add_submission.post_invoked != 0 && result.cancel_submission.post_invoked == 0 &&
                              result.recovery_submission.post_invoked != 0 && fake.count(0) == 2,
                          "uncertain Add must use one recovery and never retry Add");
            ::unsetenv("MOEX_FAKE_PUB_REPLY_MODE");
        }
        dlclose(library);
        test::remove_tree(root);
        std::cout << "moex C ABI v2 test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
