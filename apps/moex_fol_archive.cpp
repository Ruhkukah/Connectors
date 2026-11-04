#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"
#include <iostream>
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "moex_fol_archive replay CAPTURE_DIR\nmoex_fol_archive export-dbn CAPTURE_DIR "
                         "NEW_OUTPUT.dbn\nOffline only; no runtime, credentials or connections.\n";
            return 0;
        }
        if (argc < 3)
            throw std::invalid_argument("expected replay DIR or export-dbn DIR NEW_OUTPUT.dbn");
        namespace cg = moex::plaza2::cgate;
        cg::FullOrderLogArchiveSummary s;
        if (argc == 3 && std::string_view(argv[1]) == "replay")
            s = cg::replay_full_order_log_capture(argv[2]);
        else if (argc == 4 && std::string_view(argv[1]) == "export-dbn")
            s = cg::export_full_order_log_dbn(argv[2], argv[3]);
        else
            throw std::invalid_argument("invalid archive command");
        std::cout << "{\"records\":" << s.records << ",\"rows\":" << s.rows << ",\"valid_commits\":" << s.valid_commits
                  << ",\"last_valid_order_count\":" << s.last_valid_order_count
                  << ",\"open_interest_rows\":" << s.open_interest_rows
                  << ",\"open_interest_updates\":" << s.open_interest_updates << ",\"stored_bytes\":" << s.stored_bytes
                  << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
