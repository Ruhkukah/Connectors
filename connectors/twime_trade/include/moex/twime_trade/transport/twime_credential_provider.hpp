#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace moex::twime_trade::transport {

enum class TwimeCredentialSource {
    None,
    Env,
    File,
};

struct TwimeCredentialConfig {
    TwimeCredentialSource source{TwimeCredentialSource::None};
    std::string env_var;
    std::filesystem::path file_path;
};

struct TwimeCredentials {
    std::string credentials;
};

class ITwimeCredentialProvider {
  public:
    virtual ~ITwimeCredentialProvider() = default;
    virtual std::optional<TwimeCredentials> load() = 0;
};

class EnvTwimeCredentialProvider final : public ITwimeCredentialProvider {
  public:
    explicit EnvTwimeCredentialProvider(std::string env_var);

    std::optional<TwimeCredentials> load() override;
    [[nodiscard]] const std::string& env_var() const noexcept;

  private:
    std::string env_var_;
};

class FileTwimeCredentialProvider final : public ITwimeCredentialProvider {
  public:
    explicit FileTwimeCredentialProvider(std::filesystem::path file_path);

    std::optional<TwimeCredentials> load() override;
    [[nodiscard]] const std::filesystem::path& file_path() const noexcept;

  private:
    std::filesystem::path file_path_;
};

[[nodiscard]] std::optional<TwimeCredentials> load_twime_credentials(const TwimeCredentialConfig& config);

} // namespace moex::twime_trade::transport
