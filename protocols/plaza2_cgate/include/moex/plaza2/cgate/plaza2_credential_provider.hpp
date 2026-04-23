#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace moex::plaza2::cgate {

enum class Plaza2CredentialSource : std::uint8_t {
    None = 0,
    Env = 1,
    File = 2,
};

struct Plaza2CredentialConfig {
    Plaza2CredentialSource source{Plaza2CredentialSource::None};
    std::string env_var;
    std::filesystem::path file_path;
};

struct Plaza2Credentials {
    std::string value;
};

class IPlaza2CredentialProvider {
  public:
    virtual ~IPlaza2CredentialProvider() = default;
    virtual std::optional<Plaza2Credentials> load() = 0;
};

class EnvPlaza2CredentialProvider final : public IPlaza2CredentialProvider {
  public:
    explicit EnvPlaza2CredentialProvider(std::string env_var);

    std::optional<Plaza2Credentials> load() override;
    [[nodiscard]] const std::string& env_var() const noexcept;

  private:
    std::string env_var_;
};

class FilePlaza2CredentialProvider final : public IPlaza2CredentialProvider {
  public:
    explicit FilePlaza2CredentialProvider(std::filesystem::path file_path);

    std::optional<Plaza2Credentials> load() override;
    [[nodiscard]] const std::filesystem::path& file_path() const noexcept;

  private:
    std::filesystem::path file_path_;
};

[[nodiscard]] std::optional<Plaza2Credentials> load_plaza2_credentials(const Plaza2CredentialConfig& config);
[[nodiscard]] std::string redact_plaza2_credentials(std::string_view credentials);
[[nodiscard]] std::string redact_plaza2_setting_value(std::string_view settings);

} // namespace moex::plaza2::cgate
