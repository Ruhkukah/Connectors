#pragma once

#include "moex/connector_host/connector_host.hpp"
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

// Opt-in, bounded, one-shot identity comparison. Hex preserves exact protocol
// bytes (including non-ASCII); it is not anonymization or encryption.
class Plaza2PrivateIdentityEvidence {
  public:
    explicit Plaza2PrivateIdentityEvidence(const char* path) {
        if (!path)
            return;
        fd_ = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd_ < 0)
            throw std::runtime_error("cannot create private identity artifact");
        if (fchmod(fd_, 0600) != 0) {
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("cannot protect private identity artifact");
        }
    }
    ~Plaza2PrivateIdentityEvidence() {
        if (fd_ >= 0)
            close(fd_);
    }
    Plaza2PrivateIdentityEvidence(const Plaza2PrivateIdentityEvidence&) = delete;
    Plaza2PrivateIdentityEvidence& operator=(const Plaza2PrivateIdentityEvidence&) = delete;
    bool pending() const {
        return fd_ >= 0 && !captured_;
    }
    void capture(const moex::connector_host::ConnectorHostQualificationSnapshot& snapshot, std::string_view broker,
                 std::string_view client) {
        if (!pending())
            return;
        if (snapshot.limit_diagnostics.size() > 64 || broker.size() > 32 || client.size() > 32)
            throw std::runtime_error("private identity evidence bound exceeded");
        std::ostringstream out;
        out << "{\"schema\":\"plaza2.private-identity.v1\",\"broker_hex\":\"" << hex(broker)
            << "\",\"full_client_hex\":\"" << hex(client) << "\",\"rows\":[";
        bool separator = false;
        for (const auto& row : snapshot.limit_diagnostics) {
            if (row.private_account_code.size() > 32)
                throw std::runtime_error("private identity code bound exceeded");
            if (separator)
                out << ',';
            separator = true;
            out << "{\"repl_id\":" << row.repl_id << ",\"kind\":" << static_cast<unsigned>(row.kind)
                << ",\"code_length\":" << row.code_length << ",\"account_code_hex\":\"" << hex(row.private_account_code)
                << "\",\"equals_broker\":" << row.equals_broker << ",\"equals_client\":" << row.equals_client << '}';
        }
        out << "]}\n";
        const auto bytes = out.str();
        std::size_t offset{};
        while (offset < bytes.size()) {
            const auto n = write(fd_, bytes.data() + offset, bytes.size() - offset);
            if (n <= 0)
                throw std::runtime_error("private identity evidence write failed");
            offset += static_cast<std::size_t>(n);
        }
        if (fsync(fd_) != 0)
            throw std::runtime_error("private identity evidence sync failed");
        captured_ = true;
    }

  private:
    static std::string hex(std::string_view value) {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        for (unsigned char c : value) {
            result += digits[c >> 4];
            result += digits[c & 15];
        }
        return result;
    }
    int fd_{-1};
    bool captured_{};
};
