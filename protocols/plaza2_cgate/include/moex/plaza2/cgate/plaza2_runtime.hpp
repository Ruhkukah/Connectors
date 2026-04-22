#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2::cgate {

enum class Plaza2Environment : std::uint8_t {
    Test = 0,
    Prod = 1,
};

enum class Plaza2Compatibility : std::uint8_t {
    Unknown = 0,
    Compatible = 1,
    Incompatible = 2,
};

enum class Plaza2ErrorCode : std::uint16_t {
    None = 0,
    InvalidConfiguration,
    MissingRuntime,
    SymbolLoadFailed,
    AdapterState,
    RuntimeCallFailed,
    ProbeIncompatible,
    UnknownRuntimeResult,
};

enum class Plaza2ProbeIssueCode : std::uint16_t {
    MissingRuntimeRoot = 0,
    MissingLibrary,
    MissingSchemeDirectory,
    MissingSchemeFile,
    MissingConfigDirectory,
    MissingConfigFile,
    UnexpectedConfigFile,
    UnsupportedLayout,
    UnsupportedVersion,
    MissingSymbol,
    FileHashMismatch,
    RuntimeSchemeParseFailed,
    ReviewedTableMissing,
    ReviewedTableSignatureMismatch,
    RuntimeTableUnexpected,
};

struct Plaza2Error {
    Plaza2ErrorCode code{Plaza2ErrorCode::None};
    std::uint32_t runtime_code{0};
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != Plaza2ErrorCode::None;
    }
};

struct Plaza2ProbeIssue {
    Plaza2ProbeIssueCode code{Plaza2ProbeIssueCode::MissingRuntimeRoot};
    bool fatal{false};
    std::string subject;
    std::string message;
};

struct Plaza2VersionMarkers {
    std::string spectra_release;
    std::string dds_version;
    std::string target_polygon;
};

struct Plaza2Settings {
    Plaza2Environment environment{Plaza2Environment::Test};
    std::filesystem::path runtime_root;
    std::filesystem::path library_path;
    std::filesystem::path scheme_dir;
    std::filesystem::path config_dir;
    std::string env_open_settings;
    std::string application_name_prefix{"connectors"};
    std::string application_name_scope{"plaza2"};
    std::uint32_t application_instance{0};
    std::string expected_spectra_release;
    std::string expected_scheme_sha256;
};

struct Plaza2RuntimeLayout {
    std::filesystem::path runtime_root;
    std::filesystem::path library_path;
    std::filesystem::path scheme_dir;
    std::filesystem::path scheme_path;
    std::filesystem::path config_dir;
    Plaza2VersionMarkers version_markers;
    std::vector<std::string> expected_config_files;
    std::vector<std::string> present_config_files;
};

struct Plaza2SchemeDriftReport {
    Plaza2Compatibility compatibility{Plaza2Compatibility::Unknown};
    std::string runtime_scheme_sha256;
    std::size_t reviewed_table_count{0};
    std::size_t runtime_table_count{0};
    std::size_t reviewed_field_count{0};
    std::size_t runtime_field_count{0};
    std::vector<Plaza2ProbeIssue> issues;
};

struct Plaza2RuntimeProbeReport {
    Plaza2Compatibility compatibility{Plaza2Compatibility::Unknown};
    Plaza2RuntimeLayout layout;
    Plaza2SchemeDriftReport scheme_drift;
    std::vector<std::string> resolved_symbols;
    std::vector<Plaza2ProbeIssue> issues;
    bool runtime_root_present{false};
    bool runtime_library_present{false};
    bool runtime_library_loadable{false};
    bool scheme_file_present{false};
    bool config_dir_present{false};
};

[[nodiscard]] Plaza2Error validate_plaza2_settings(const Plaza2Settings& settings);
[[nodiscard]] std::string make_plaza2_application_name(std::string_view prefix,
                                                       std::string_view scope,
                                                       std::uint32_t instance);
[[nodiscard]] Plaza2Error translate_plaza2_result(std::string_view operation, std::uint32_t runtime_code);

class Plaza2RuntimeProbe {
  public:
    [[nodiscard]] static Plaza2RuntimeProbeReport probe(const Plaza2Settings& settings);
    [[nodiscard]] static std::span<const std::string_view> expected_config_filenames(Plaza2Environment environment);
    [[nodiscard]] static std::span<const std::string_view> required_runtime_symbols();
};

struct Plaza2RuntimeSharedState;

class Plaza2Env {
  public:
    Plaza2Env() = default;
    ~Plaza2Env();

    Plaza2Env(Plaza2Env&&) noexcept = default;
    Plaza2Env& operator=(Plaza2Env&&) noexcept = default;

    Plaza2Env(const Plaza2Env&) = delete;
    Plaza2Env& operator=(const Plaza2Env&) = delete;

    [[nodiscard]] Plaza2Error open(const Plaza2Settings& settings);
    [[nodiscard]] Plaza2Error close();
    [[nodiscard]] bool is_open() const noexcept;

  private:
    friend class Plaza2Connection;

    std::shared_ptr<Plaza2RuntimeSharedState> shared_;
    bool open_{false};
};

class Plaza2Connection {
  public:
    Plaza2Connection() = default;
    ~Plaza2Connection();

    Plaza2Connection(Plaza2Connection&&) noexcept = default;
    Plaza2Connection& operator=(Plaza2Connection&&) noexcept = default;

    Plaza2Connection(const Plaza2Connection&) = delete;
    Plaza2Connection& operator=(const Plaza2Connection&) = delete;

    [[nodiscard]] Plaza2Error create(Plaza2Env& env, std::string_view settings);
    [[nodiscard]] Plaza2Error open(std::string_view settings);
    [[nodiscard]] Plaza2Error process(std::uint32_t timeout_ms, std::uint32_t* runtime_code = nullptr);
    [[nodiscard]] Plaza2Error close();
    [[nodiscard]] Plaza2Error destroy();
    [[nodiscard]] Plaza2Error state(std::uint32_t& out_state) const;
    [[nodiscard]] bool is_created() const noexcept;

  private:
    friend class Plaza2Listener;

    std::shared_ptr<Plaza2RuntimeSharedState> shared_;
    void* handle_{nullptr};
};

class Plaza2Listener {
  public:
    Plaza2Listener() = default;
    ~Plaza2Listener();

    Plaza2Listener(Plaza2Listener&&) noexcept = default;
    Plaza2Listener& operator=(Plaza2Listener&&) noexcept = default;

    Plaza2Listener(const Plaza2Listener&) = delete;
    Plaza2Listener& operator=(const Plaza2Listener&) = delete;

    [[nodiscard]] Plaza2Error create(Plaza2Connection& connection, std::string_view settings);
    [[nodiscard]] Plaza2Error open(std::string_view settings);
    [[nodiscard]] Plaza2Error close();
    [[nodiscard]] Plaza2Error destroy();
    [[nodiscard]] Plaza2Error state(std::uint32_t& out_state) const;
    [[nodiscard]] bool is_created() const noexcept;

  private:
    std::shared_ptr<Plaza2RuntimeSharedState> shared_;
    void* handle_{nullptr};
};

} // namespace moex::plaza2::cgate
