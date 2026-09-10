#include "../../apps/plaza2_private_identity.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    char dir[] = "/tmp/plaza2-identity-XXXXXX";
    if (!mkdtemp(dir))
        return 1;
    const auto path = std::filesystem::path(dir) / "identity.json";
    try {
        using Snapshot = moex::connector_host::ConnectorHostQualificationSnapshot;
        Snapshot snapshot;
        Snapshot::LimitDiagnostic row{};
        row.repl_id = 7;
        row.kind = moex::plaza2::private_state::LimitParticipantKind::Client;
        row.private_account_code = "brk1 C1";
        row.code_length = 7;
        snapshot.limit_diagnostics.push_back(row);
        Plaza2PrivateIdentityEvidence disabled(nullptr);
        if (disabled.pending())
            return 2;
        {
            Plaza2PrivateIdentityEvidence evidence(path.c_str());
            evidence.capture(snapshot, "BRK1", "BRK1C01");
            if (evidence.pending())
                return 3;
            evidence.capture(snapshot, "OTHER", "OTHER"); // One-shot, no overwrite.
        }
        struct stat info{};
        if (stat(path.c_str(), &info) != 0 || (info.st_mode & 0777) != 0600)
            return 4;
        std::ifstream input(path);
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        if (contents.find("62726b31204331") == std::string::npos ||
            contents.find("42524b31433031") == std::string::npos ||
            contents.find("\"equals_client\":0") == std::string::npos)
            return 5;
        bool rejected{};
        try {
            Plaza2PrivateIdentityEvidence duplicate(path.c_str());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        if (!rejected)
            return 6;
        const auto link = std::filesystem::path(dir) / "link";
        std::filesystem::create_symlink(path, link);
        rejected = false;
        try {
            Plaza2PrivateIdentityEvidence symlink(link.c_str());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        if (!rejected)
            return 7;
        std::filesystem::remove_all(dir);
        std::cout << "private identity exact bytes, mode 0600, no overwrite/symlink PASS\n";
        return 0;
    } catch (...) {
        std::filesystem::remove_all(dir);
        return 8;
    }
}
