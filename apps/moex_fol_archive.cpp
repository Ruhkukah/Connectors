#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"
#include <iostream>
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "moex_fol_archive replay CAPTURE_DIR\nmoex_fol_archive export-dbn CAPTURE_DIR "
                         "NEW_OUTPUT.dbn\nOptional --allow-incomplete: verified prefix only; JSON reports "
                         "truncated/last_good_sequence.\ntrade_prints counts DBN export records and is reported "
                         "only for export-dbn.\nOffline only; no runtime, credentials or connections.\n";
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
        namespace cg = moex::plaza2::cgate;
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
                  << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
