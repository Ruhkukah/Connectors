#include "moex/plaza2/cgate/plaza2_credential_provider.hpp"

#include "moex_core/logging/redaction.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace moex::plaza2::cgate {

namespace {

std::string trim_copy(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

} // namespace

EnvPlaza2CredentialProvider::EnvPlaza2CredentialProvider(std::string env_var) : env_var_(std::move(env_var)) {}

std::optional<Plaza2Credentials> EnvPlaza2CredentialProvider::load() {
    if (env_var_.empty()) {
        return std::nullopt;
    }
    const char* value = std::getenv(env_var_.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }

    const auto trimmed = trim_copy(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return Plaza2Credentials{.value = trimmed};
}

const std::string& EnvPlaza2CredentialProvider::env_var() const noexcept {
    return env_var_;
}

FilePlaza2CredentialProvider::FilePlaza2CredentialProvider(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

std::optional<Plaza2Credentials> FilePlaza2CredentialProvider::load() {
    if (file_path_.empty()) {
        return std::nullopt;
    }

    std::ifstream input(file_path_);
    if (!input.is_open()) {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto trimmed = trim_copy(buffer.str());
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return Plaza2Credentials{.value = trimmed};
}

const std::filesystem::path& FilePlaza2CredentialProvider::file_path() const noexcept {
    return file_path_;
}

std::optional<Plaza2Credentials> load_plaza2_credentials(const Plaza2CredentialConfig& config) {
    switch (config.source) {
    case Plaza2CredentialSource::None:
        return std::nullopt;
    case Plaza2CredentialSource::Env:
        return EnvPlaza2CredentialProvider(config.env_var).load();
    case Plaza2CredentialSource::File:
        return FilePlaza2CredentialProvider(config.file_path).load();
    }
    return std::nullopt;
}

std::string redact_plaza2_credentials(std::string_view credentials) {
    return moex::logging::redact_value(moex::logging::SecretKind::SessionSecret, std::string(credentials));
}

std::string redact_plaza2_setting_value(std::string_view settings) {
    if (settings.empty()) {
        return {};
    }
    return "[REDACTED_SETTINGS]";
}

} // namespace moex::plaza2::cgate
