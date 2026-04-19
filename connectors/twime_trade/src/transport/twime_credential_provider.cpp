#include "moex/twime_trade/transport/twime_credential_provider.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace moex::twime_trade::transport {

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

EnvTwimeCredentialProvider::EnvTwimeCredentialProvider(std::string env_var) : env_var_(std::move(env_var)) {}

std::optional<TwimeCredentials> EnvTwimeCredentialProvider::load() {
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
    return TwimeCredentials{.credentials = trimmed};
}

const std::string& EnvTwimeCredentialProvider::env_var() const noexcept {
    return env_var_;
}

FileTwimeCredentialProvider::FileTwimeCredentialProvider(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

std::optional<TwimeCredentials> FileTwimeCredentialProvider::load() {
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

    return TwimeCredentials{.credentials = trimmed};
}

const std::filesystem::path& FileTwimeCredentialProvider::file_path() const noexcept {
    return file_path_;
}

std::optional<TwimeCredentials> load_twime_credentials(const TwimeCredentialConfig& config) {
    switch (config.source) {
    case TwimeCredentialSource::None:
        return std::nullopt;
    case TwimeCredentialSource::Env:
        return EnvTwimeCredentialProvider(config.env_var).load();
    case TwimeCredentialSource::File:
        return FileTwimeCredentialProvider(config.file_path).load();
    }
    return std::nullopt;
}

} // namespace moex::twime_trade::transport
