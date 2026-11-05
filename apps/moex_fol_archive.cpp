#include "moex/plaza2/cgate/plaza2_full_order_log_backfill.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"
#include <charconv>
#include <iostream>

namespace {
namespace cg = moex::plaza2::cgate;

template <class T> T positive(std::string_view text) {
    T value{};
    const auto end = text.data() + text.size();
    const auto [next, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || next != end || value <= 0)
        throw std::invalid_argument("backfill parameters must be positive integers");
    return value;
}

void print_boundary(const cg::FullOrderLogBackfillBoundary& boundary) {
    std::cout << "{\"revision\":" << boundary.revision << ",\"session\":" << boundary.session
              << ",\"commit_sequence\":" << boundary.commit_sequence << ",\"sha256\":\"" << boundary.sha256 << "\"}";
}

void print_plan(const cg::FullOrderLogBackfillPlan& plan) {
    std::cout << "\"capture_identity\":\"" << plan.capture_identity << "\",\"matching_id\":" << plan.matching_id
              << ",\"ordlog_lifenum\":" << plan.ordlog_lifenum << ",\"reopen_sequence\":" << plan.reopen_sequence
              << ",\"listener_settings\":\"" << plan.listener_settings << "\",\"open_settings\":\""
              << plan.open_settings << "\",\"start\":";
    print_boundary(plan.start);
    std::cout << ",\"left\":";
    print_boundary(plan.left);
    std::cout << ",\"right\":";
    print_boundary(plan.right);
}

void print_coverage(const cg::FullOrderLogArchiveStreamCoverage& coverage) {
    std::cout << "{\"opens\":" << coverage.opens << ",\"onlines\":" << coverage.onlines
              << ",\"closes\":" << coverage.closes << ",\"listener_errors\":" << coverage.listener_errors
              << ",\"life_notifications\":" << coverage.life_notifications
              << ",\"clear_deleted\":" << coverage.clear_deleted << '}';
}

void backfill(int argc, char** argv, bool checking) {
    const int base = checking ? 6 : 5;
    if (argc != base && argc != base + 1)
        throw std::invalid_argument("expected capture path(s), verified MATCH_ID ORDLOG_LIFENUM [REOPEN_SEQUENCE]");
    const auto matching = positive<std::int32_t>(argv[base - 2]);
    const auto life = positive<std::uint64_t>(argv[base - 1]);
    const auto reopen = argc > base ? positive<std::uint64_t>(argv[base]) : 0;
    if (checking) {
        const auto check = cg::check_full_order_log_backfill(argv[2], argv[3], matching, life, reopen);
        std::cout << "{\"result\":\"committed_overlap_verified\",\"recovery_capture_identity\":\""
                  << check.recovery_capture_identity << "\",\"committed_rows_between\":" << check.committed_rows_between
                  << ',';
        print_plan(check.plan);
    } else {
        const auto plan = cg::plan_full_order_log_backfill(argv[2], matching, life, reopen);
        std::cout << "{\"result\":\"planned\",";
        print_plan(plan);
    }
    std::cout << "}\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "moex_fol_archive replay CAPTURE_DIR\nmoex_fol_archive export-dbn CAPTURE_DIR "
                         "NEW_OUTPUT.dbn\n"
                         "moex_fol_archive plan-backfill CAPTURE_DIR MATCH_ID ORDLOG_LIFENUM [REOPEN_SEQUENCE]\n"
                         "moex_fol_archive check-backfill CAPTURE_DIR RECOVERY_DIR MATCH_ID ORDLOG_LIFENUM "
                         "[REOPEN_SEQUENCE]\n"
                         "Optional --allow-incomplete for replay/export-dbn: verified prefix only; JSON reports "
                         "truncated/last_good_sequence.\ntrade_prints counts DBN export records and is reported "
                         "only for export-dbn.\nOffline only; no runtime, credentials or connections.\n";
            return 0;
        }
        if (argc > 1 &&
            (std::string_view(argv[1]) == "plan-backfill" || std::string_view(argv[1]) == "check-backfill")) {
            backfill(argc, argv, std::string_view(argv[1]) == "check-backfill");
            return 0;
        }
        bool allow_incomplete = false;
        std::vector<char*> args{argv[0]};
        for (int i = 1; i < argc; ++i)
            if (std::string_view(argv[i]) == "--allow-incomplete")
                allow_incomplete = true;
            else
                args.push_back(argv[i]);
        argc = static_cast<int>(args.size());
        argv = args.data();
        if (argc < 3)
            throw std::invalid_argument("expected replay DIR or export-dbn DIR NEW_OUTPUT.dbn");
        cg::FullOrderLogArchiveSummary s;
        const bool exporting = argc == 4 && std::string_view(argv[1]) == "export-dbn";
        if (argc == 3 && std::string_view(argv[1]) == "replay")
            s = cg::replay_full_order_log_capture(argv[2], allow_incomplete);
        else if (exporting)
            s = cg::export_full_order_log_dbn(argv[2], argv[3], allow_incomplete);
        else
            throw std::invalid_argument("invalid archive command");
        std::cout << "{\"records\":" << s.records << ",\"rows\":" << s.rows << ",\"valid_commits\":" << s.valid_commits
                  << ",\"last_valid_order_count\":" << s.last_valid_order_count
                  << ",\"open_interest_rows\":" << s.open_interest_rows;
        if (exporting)
            std::cout << ",\"trade_prints\":" << s.trade_prints;
        if (!s.refdata_sha256.empty())
            std::cout << ",\"refdata_sha256\":\"" << s.refdata_sha256 << '"';
        if (!s.dbn_sha256.empty())
            std::cout << ",\"dbn_sha256\":\"" << s.dbn_sha256
                      << "\",\"refdata_unjoined_records\":" << s.refdata_unjoined_records;
        std::cout << ",\"open_interest_updates\":" << s.open_interest_updates << ",\"stored_bytes\":" << s.stored_bytes
                  << ",\"truncated\":" << (s.truncated ? "true" : "false")
                  << ",\"last_good_sequence\":" << s.last_good_sequence
                  << ",\"end_record_present\":" << (s.end_record_present ? "true" : "false")
                  << ",\"end_status\":" << static_cast<unsigned>(s.end_status) << ",\"host_status\":" << s.host_status
                  << ",\"fol_coverage\":";
        print_coverage(s.fol_coverage);
        std::cout << ",\"deals_coverage\":";
        print_coverage(s.deals_coverage);
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
