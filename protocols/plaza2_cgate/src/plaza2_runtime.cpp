#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include "plaza2_generated_metadata.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dlfcn.h>

namespace moex::plaza2::cgate {

namespace {

using CgResult = std::uint32_t;
using CgListenerCallback = CgResult (*)(void* conn, void* listener, void* msg, void* data);

constexpr std::uint32_t kCgErrOk = 0;
constexpr std::uint32_t kCgRangeBegin = 131072;
constexpr std::uint32_t kCgErrInternal = kCgRangeBegin;
constexpr std::uint32_t kCgErrInvalidArgument = kCgRangeBegin + 1;
constexpr std::uint32_t kCgErrUnsupported = kCgRangeBegin + 2;
constexpr std::uint32_t kCgErrTimeout = kCgRangeBegin + 3;
constexpr std::uint32_t kCgErrMore = kCgRangeBegin + 4;
constexpr std::uint32_t kCgErrIncorrectState = kCgRangeBegin + 5;
constexpr std::uint32_t kCgErrBufferTooSmall = kCgRangeBegin + 7;

constexpr std::uint32_t kCgMsgOpen = 0x100;
constexpr std::uint32_t kCgMsgClose = 0x101;
constexpr std::uint32_t kCgMsgStreamData = 0x120;
constexpr std::uint32_t kCgMsgTnBegin = 0x200;
constexpr std::uint32_t kCgMsgTnCommit = 0x210;
constexpr std::uint32_t kCgMsgP2replLifenum = 0x1110;
constexpr std::uint32_t kCgMsgP2replClearDeleted = 0x1111;
constexpr std::uint32_t kCgMsgP2replOnline = 0x1112;
constexpr std::uint32_t kCgMsgP2replReplState = 0x1115;

constexpr std::string_view kSchemeFilename = "forts_scheme.ini";
constexpr std::array<std::string_view, 6> kProdConfigFiles = {
    "links_public.prod.ini", "links_public.rezerv.ini", "prod.ini",
    "auth_client.ini",       "client_router.ini",       "router.ini",
};
constexpr std::array<std::string_view, 10> kTestConfigFiles = {
    "links_public.t0.ini",
    "links_public.t1.ini",
    "links_public.game.ini",
    "links_public.custom.ini",
    "t0.ini",
    "t1.ini",
    "game.ini",
    "auth_client.ini",
    "client_router.ini",
    "router.ini",
};
constexpr std::array<std::string_view, 15> kRequiredSymbols = {
    "cg_env_open",   "cg_env_close",    "cg_conn_new",      "cg_conn_destroy",  "cg_conn_open",
    "cg_conn_close", "cg_conn_process", "cg_conn_getstate", "cg_lsn_new",       "cg_lsn_destroy",
    "cg_lsn_open",   "cg_lsn_close",    "cg_lsn_getstate",  "cg_lsn_getscheme", "cg_getstr",
};

constexpr std::array<std::string_view, 4> kLibraryFilenameCandidates = {
#if defined(__APPLE__)
    "libcgate.dylib",
    "libcgate.so",
#else
    "libcgate.so",
    "libcgate64.so",
#endif
    "cgate.dll",
    "cgate64.dll",
};

struct RuntimeApi {
    void* library_handle{nullptr};

    CgResult (*env_open)(const char* settings){nullptr};
    CgResult (*env_close)(){nullptr};
    CgResult (*conn_new)(const char* settings, void** connptr){nullptr};
    CgResult (*conn_destroy)(void* conn){nullptr};
    CgResult (*conn_open)(void* conn, const char* settings){nullptr};
    CgResult (*conn_close)(void* conn){nullptr};
    CgResult (*conn_process)(void* conn, std::uint32_t timeout, void* reserved){nullptr};
    CgResult (*conn_getstate)(void* conn, std::uint32_t* state){nullptr};
    CgResult (*lsn_new)(void* conn, const char* settings, CgListenerCallback callback, void* data,
                        void** lsnptr){nullptr};
    CgResult (*lsn_destroy)(void* listener){nullptr};
    CgResult (*lsn_open)(void* listener, const char* settings){nullptr};
    CgResult (*lsn_close)(void* listener){nullptr};
    CgResult (*lsn_getstate)(void* listener, std::uint32_t* state){nullptr};
    CgResult (*lsn_getscheme)(void* listener, void** schemeptr){nullptr};
    CgResult (*getstr)(const char* type, const void* data, char* buffer, std::size_t* buffer_size){nullptr};
};

struct RuntimeLibraryCloser {
    void operator()(RuntimeApi* api) const noexcept {
        if (api == nullptr) {
            return;
        }
        if (api->library_handle != nullptr) {
            ::dlclose(api->library_handle);
        }
        delete api;
    }
};

struct RuntimeSchemeField {
    std::string field_name;
    std::string type_token;
};

struct CgValuePair {
    CgValuePair* next;
    char* key;
    char* value;
};

struct CgFieldValueDesc {
    CgFieldValueDesc* next;
    char* name;
    char* desc;
    void* value;
    void* mask;
};

struct CgMessageDesc;

struct CgFieldDesc {
    CgFieldDesc* next;
    std::uint32_t id;
    char* name;
    char* desc;
    char* type;
    std::size_t size;
    std::size_t offset;
    void* def_value;
    std::size_t num_values;
    CgFieldValueDesc* values;
    CgValuePair* hints;
    std::size_t max_count;
    CgFieldDesc* count_field;
    CgMessageDesc* type_msg;
};

struct CgIndexFieldDesc {
    CgIndexFieldDesc* next;
    CgFieldDesc* field;
    std::uint32_t sort_order;
};

struct CgIndexDesc {
    CgIndexDesc* next;
    std::size_t num_fields;
    CgIndexFieldDesc* fields;
    char* name;
    char* desc;
    CgValuePair* hints;
};

struct CgMessageDesc {
    CgMessageDesc* next;
    std::size_t size;
    std::size_t num_fields;
    CgFieldDesc* fields;
    std::uint32_t id;
    char* name;
    char* desc;
    CgValuePair* hints;
    std::size_t num_indices;
    CgIndexDesc* indices;
    std::size_t align;
};

struct CgSchemeDesc {
    std::uint32_t scheme_type;
    std::uint32_t features;
    std::size_t num_messages;
    CgMessageDesc* messages;
    CgValuePair* hints;
};

struct CgMsg {
    std::uint32_t type;
    std::size_t data_size;
    void* data;
    std::int64_t owner_id;
};

struct CgMsgStreamData {
    std::uint32_t type;
    std::size_t data_size;
    void* data;
    std::int64_t owner_id;
    std::size_t msg_index;
    std::uint32_t msg_id;
    const char* msg_name;
    std::int64_t rev;
    std::size_t num_nulls;
    std::uint8_t* nulls;
    std::uint64_t user_id;
};

struct CgTime {
    std::uint16_t year;
    std::uint8_t month;
    std::uint8_t day;
    std::uint8_t hour;
    std::uint8_t minute;
    std::uint8_t second;
    std::uint16_t msec;
};

struct CgDataClearDeleted {
    std::uint32_t table_idx;
    std::int64_t table_rev;
    std::uint32_t flags;
};

struct CgDataLifeNum {
    std::uint32_t life_number;
    std::uint32_t flags;
};

struct RuntimeSchemeTable {
    std::string scheme_name;
    std::string table_name;
    std::vector<RuntimeSchemeField> fields;
};

struct ParsedRuntimeScheme {
    Plaza2VersionMarkers markers;
    std::vector<RuntimeSchemeTable> tables;
};

struct ReviewedSignatureEntry {
    std::string example_name;
    std::size_t count{0};
};

struct RuntimeSignatureEntry {
    std::string example_name;
    std::size_t count{0};
};

[[nodiscard]] std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

[[nodiscard]] std::string sanitize_token(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            continue;
        }
        result.push_back('_');
    }
    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }
    return result;
}

[[nodiscard]] std::string read_file_to_string(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::vector<std::string> read_file_lines(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

[[nodiscard]] std::filesystem::path resolve_optional_path(const std::filesystem::path& root,
                                                          const std::filesystem::path& value) {
    if (value.empty()) {
        return {};
    }
    if (value.is_absolute()) {
        return value;
    }
    return root / value;
}

[[nodiscard]] std::optional<std::filesystem::path>
first_existing_directory(const std::vector<std::filesystem::path>& candidates) {
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> resolve_library_path(const Plaza2Settings& settings) {
    const auto explicit_path = resolve_optional_path(settings.runtime_root, settings.library_path);
    if (!explicit_path.empty()) {
        return explicit_path;
    }

    const std::array<std::filesystem::path, 3> candidate_dirs = {
        settings.runtime_root / "bin",
        settings.runtime_root / "lib",
        settings.runtime_root,
    };
    for (const auto& dir : candidate_dirs) {
        for (const auto file_name : kLibraryFilenameCandidates) {
            const auto candidate = dir / file_name;
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> resolve_scheme_dir(const Plaza2Settings& settings) {
    const auto explicit_dir = resolve_optional_path(settings.runtime_root, settings.scheme_dir);
    if (!explicit_dir.empty()) {
        return explicit_dir;
    }
    return first_existing_directory({
        settings.runtime_root / "scheme",
        settings.runtime_root / "Scheme",
        settings.runtime_root / "ini",
        settings.runtime_root,
    });
}

[[nodiscard]] std::optional<std::filesystem::path> resolve_config_dir(const Plaza2Settings& settings) {
    const auto explicit_dir = resolve_optional_path(settings.runtime_root, settings.config_dir);
    if (!explicit_dir.empty()) {
        return explicit_dir;
    }
    return first_existing_directory({
        settings.runtime_root / "config",
        settings.runtime_root / "cfg",
        settings.runtime_root / "ini",
        settings.runtime_root,
    });
}

[[nodiscard]] std::vector<std::string> collect_present_config_files(const std::filesystem::path& config_dir) {
    std::vector<std::string> present;
    if (!std::filesystem::is_directory(config_dir)) {
        return present;
    }
    for (const auto& entry : std::filesystem::directory_iterator(config_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".ini") {
            continue;
        }
        present.push_back(entry.path().filename().string());
    }
    std::sort(present.begin(), present.end());
    return present;
}

[[nodiscard]] std::string canonical_table_signature(std::span<const RuntimeSchemeField> fields) {
    std::ostringstream out;
    for (const auto& field : fields) {
        out << field.field_name << ':' << field.type_token << ';';
    }
    return out.str();
}

[[nodiscard]] std::string canonical_table_signature(std::span<const generated::FieldDescriptor> fields) {
    std::ostringstream out;
    for (const auto& field : fields) {
        out << field.field_name << ':' << field.type_token << ';';
    }
    return out.str();
}

[[nodiscard]] bool parse_runtime_scheme(const std::filesystem::path& scheme_path, ParsedRuntimeScheme& out_scheme,
                                        std::string& error_message) {
    out_scheme = {};
    RuntimeSchemeTable* current_table = nullptr;
    std::string current_section;

    for (const auto& raw_line : read_file_lines(scheme_path)) {
        const auto line = trim_copy(raw_line);
        if (line.empty()) {
            continue;
        }
        if (line.rfind(';', 0) == 0) {
            const auto comment = trim_copy(std::string_view(line).substr(1));
            constexpr std::string_view kSpectraPrefix = "Spectra release:";
            constexpr std::string_view kDdsPrefix = "DDS version:";
            constexpr std::string_view kTargetPrefixA = "Target poligon:";
            constexpr std::string_view kTargetPrefixB = "Target polygon:";
            if (comment.rfind(kSpectraPrefix, 0) == 0) {
                out_scheme.markers.spectra_release = trim_copy(std::string_view(comment).substr(kSpectraPrefix.size()));
            } else if (comment.rfind(kDdsPrefix, 0) == 0) {
                out_scheme.markers.dds_version = trim_copy(std::string_view(comment).substr(kDdsPrefix.size()));
            } else if (comment.rfind(kTargetPrefixA, 0) == 0) {
                out_scheme.markers.target_polygon = trim_copy(std::string_view(comment).substr(kTargetPrefixA.size()));
            } else if (comment.rfind(kTargetPrefixB, 0) == 0) {
                out_scheme.markers.target_polygon = trim_copy(std::string_view(comment).substr(kTargetPrefixB.size()));
            }
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            current_table = nullptr;
            if (current_section.rfind("table:", 0) == 0) {
                const auto rest = current_section.substr(6);
                const auto separator = rest.find(':');
                if (separator == std::string::npos) {
                    error_message = "invalid table section: " + current_section;
                    return false;
                }
                out_scheme.tables.push_back({
                    .scheme_name = rest.substr(0, separator),
                    .table_name = rest.substr(separator + 1),
                    .fields = {},
                });
                current_table = &out_scheme.tables.back();
            }
            continue;
        }

        if (current_table == nullptr) {
            continue;
        }

        if (line.rfind("field=", 0) != 0) {
            continue;
        }
        const auto remainder = line.substr(6);
        const auto comma = remainder.find(',');
        if (comma == std::string::npos) {
            error_message = "invalid field line: " + line;
            return false;
        }
        const auto field_name = trim_copy(std::string_view(remainder).substr(0, comma));
        const auto type_token = trim_copy(std::string_view(remainder).substr(comma + 1));
        if (field_name.empty() || type_token.empty()) {
            error_message = "invalid empty field token in line: " + line;
            return false;
        }
        current_table->fields.push_back({field_name, type_token});
    }

    for (const auto& table : out_scheme.tables) {
        if (table.table_name.empty() || table.fields.empty()) {
            error_message = "runtime scheme contains empty table definition";
            return false;
        }
    }
    if (out_scheme.tables.empty()) {
        error_message = "runtime scheme contains no table sections";
        return false;
    }
    return true;
}

struct Sha256State {
    std::array<std::uint32_t, 8> hash{
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
    };
    std::uint64_t total_bits{0};
    std::array<std::byte, 64> buffer{};
    std::size_t buffer_size{0};
};

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFau, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t shift) noexcept {
    return (value >> shift) | (value << (32u - shift));
}

void sha256_compress(Sha256State& state, const std::byte* block) {
    std::uint32_t schedule[64]{};
    for (std::size_t idx = 0; idx < 16; ++idx) {
        const auto base = idx * 4;
        schedule[idx] = (static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[base])) << 24u) |
                        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[base + 1])) << 16u) |
                        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[base + 2])) << 8u) |
                        static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[base + 3]));
    }
    for (std::size_t idx = 16; idx < 64; ++idx) {
        const auto s0 = rotr(schedule[idx - 15], 7u) ^ rotr(schedule[idx - 15], 18u) ^ (schedule[idx - 15] >> 3u);
        const auto s1 = rotr(schedule[idx - 2], 17u) ^ rotr(schedule[idx - 2], 19u) ^ (schedule[idx - 2] >> 10u);
        schedule[idx] = schedule[idx - 16] + s0 + schedule[idx - 7] + s1;
    }

    auto a = state.hash[0];
    auto b = state.hash[1];
    auto c = state.hash[2];
    auto d = state.hash[3];
    auto e = state.hash[4];
    auto f = state.hash[5];
    auto g = state.hash[6];
    auto h = state.hash[7];

    for (std::size_t idx = 0; idx < 64; ++idx) {
        const auto s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
        const auto choice = (e & f) ^ (~e & g);
        const auto temp1 = h + s1 + choice + kSha256K[idx] + schedule[idx];
        const auto s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state.hash[0] += a;
    state.hash[1] += b;
    state.hash[2] += c;
    state.hash[3] += d;
    state.hash[4] += e;
    state.hash[5] += f;
    state.hash[6] += g;
    state.hash[7] += h;
}

void sha256_update(Sha256State& state, std::span<const std::byte> bytes) {
    for (const auto byte : bytes) {
        state.buffer[state.buffer_size++] = byte;
        if (state.buffer_size == state.buffer.size()) {
            sha256_compress(state, state.buffer.data());
            state.total_bits += static_cast<std::uint64_t>(state.buffer.size()) * 8u;
            state.buffer_size = 0;
        }
    }
}

[[nodiscard]] std::string sha256_finish(Sha256State& state) {
    state.total_bits += static_cast<std::uint64_t>(state.buffer_size) * 8u;
    state.buffer[state.buffer_size++] = std::byte{0x80u};
    if (state.buffer_size > 56) {
        while (state.buffer_size < 64) {
            state.buffer[state.buffer_size++] = std::byte{0};
        }
        sha256_compress(state, state.buffer.data());
        state.buffer_size = 0;
    }
    while (state.buffer_size < 56) {
        state.buffer[state.buffer_size++] = std::byte{0};
    }

    for (int shift = 56; shift >= 0; shift -= 8) {
        state.buffer[state.buffer_size++] = std::byte{static_cast<unsigned char>((state.total_bits >> shift) & 0xFFu)};
    }
    sha256_compress(state, state.buffer.data());

    std::ostringstream out;
    out << std::hex;
    for (const auto value : state.hash) {
        out.width(8);
        out.fill('0');
        out << std::nouppercase << value;
    }
    return out.str();
}

[[nodiscard]] std::string sha256_hex(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Sha256State state;
    std::array<std::byte, 4096> buffer{};
    while (input.good()) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0) {
            break;
        }
        sha256_update(state, std::span<const std::byte>(buffer.data(), count));
    }
    return sha256_finish(state);
}

[[nodiscard]] Plaza2Compatibility combine_compatibility(Plaza2Compatibility lhs, Plaza2Compatibility rhs) noexcept {
    if (lhs == Plaza2Compatibility::Incompatible || rhs == Plaza2Compatibility::Incompatible) {
        return Plaza2Compatibility::Incompatible;
    }
    if (lhs == Plaza2Compatibility::Compatible && rhs == Plaza2Compatibility::Compatible) {
        return Plaza2Compatibility::Compatible;
    }
    return Plaza2Compatibility::Unknown;
}

void push_issue(std::vector<Plaza2ProbeIssue>& issues, Plaza2ProbeIssueCode code, bool fatal, std::string subject,
                std::string message) {
    issues.push_back({
        .code = code,
        .fatal = fatal,
        .subject = std::move(subject),
        .message = std::move(message),
    });
}

[[nodiscard]] Plaza2Compatibility compatibility_from_issues(const std::vector<Plaza2ProbeIssue>& issues) {
    for (const auto& issue : issues) {
        if (issue.fatal) {
            return Plaza2Compatibility::Incompatible;
        }
    }
    return issues.empty() ? Plaza2Compatibility::Compatible : Plaza2Compatibility::Unknown;
}

[[nodiscard]] std::unique_ptr<RuntimeApi, RuntimeLibraryCloser>
load_runtime_api(const std::filesystem::path& library_path, std::vector<std::string>* resolved_symbols,
                 std::vector<Plaza2ProbeIssue>* issues) {
    const auto* handle = ::dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        if (issues != nullptr) {
            push_issue(*issues, Plaza2ProbeIssueCode::MissingLibrary, true, library_path.string(),
                       std::string("failed to load CGate runtime library: ") + ::dlerror());
        }
        return {};
    }

    auto api = std::unique_ptr<RuntimeApi, RuntimeLibraryCloser>(new RuntimeApi{});
    api->library_handle = const_cast<void*>(handle);
    auto load_symbol = [&](const char* name, auto& target) -> bool {
        void* symbol = ::dlsym(api->library_handle, name);
        if (symbol == nullptr) {
            if (issues != nullptr) {
                push_issue(*issues, Plaza2ProbeIssueCode::MissingSymbol, true, name,
                           std::string("required CGate symbol is missing from runtime library: ") + name);
            }
            return false;
        }
        if (resolved_symbols != nullptr) {
            resolved_symbols->push_back(name);
        }
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(symbol);
        return true;
    };

    bool ok = true;
    ok = load_symbol("cg_env_open", api->env_open) && ok;
    ok = load_symbol("cg_env_close", api->env_close) && ok;
    ok = load_symbol("cg_conn_new", api->conn_new) && ok;
    ok = load_symbol("cg_conn_destroy", api->conn_destroy) && ok;
    ok = load_symbol("cg_conn_open", api->conn_open) && ok;
    ok = load_symbol("cg_conn_close", api->conn_close) && ok;
    ok = load_symbol("cg_conn_process", api->conn_process) && ok;
    ok = load_symbol("cg_conn_getstate", api->conn_getstate) && ok;
    ok = load_symbol("cg_lsn_new", api->lsn_new) && ok;
    ok = load_symbol("cg_lsn_destroy", api->lsn_destroy) && ok;
    ok = load_symbol("cg_lsn_open", api->lsn_open) && ok;
    ok = load_symbol("cg_lsn_close", api->lsn_close) && ok;
    ok = load_symbol("cg_lsn_getstate", api->lsn_getstate) && ok;
    ok = load_symbol("cg_lsn_getscheme", api->lsn_getscheme) && ok;
    ok = load_symbol("cg_getstr", api->getstr) && ok;
    if (!ok) {
        return {};
    }
    return api;
}

[[nodiscard]] CgResult noop_listener_callback(void*, void*, void*, void*) {
    return kCgErrOk;
}

[[nodiscard]] Plaza2SchemeDriftReport compare_runtime_scheme(const std::filesystem::path& scheme_path,
                                                             const Plaza2Settings& settings) {
    Plaza2SchemeDriftReport report;
    report.reviewed_table_count = generated::TableDescriptors().size();
    report.reviewed_field_count = generated::FieldDescriptors().size();
    report.runtime_scheme_sha256 = sha256_hex(scheme_path);

    ParsedRuntimeScheme parsed;
    std::string parse_error;
    if (!parse_runtime_scheme(scheme_path, parsed, parse_error)) {
        push_issue(report.issues, Plaza2ProbeIssueCode::RuntimeSchemeParseFailed, true, scheme_path.filename().string(),
                   "failed to parse runtime forts_scheme.ini: " + parse_error);
        report.compatibility = Plaza2Compatibility::Incompatible;
        return report;
    }

    report.runtime_table_count = parsed.tables.size();
    std::size_t runtime_field_count = 0;
    std::map<std::string, std::map<std::string, RuntimeSignatureEntry>> runtime_by_name;
    for (const auto& table : parsed.tables) {
        runtime_field_count += table.fields.size();
        auto& entry = runtime_by_name[table.table_name][canonical_table_signature(table.fields)];
        if (entry.example_name.empty()) {
            entry.example_name = table.scheme_name + "." + table.table_name;
        }
        ++entry.count;
    }
    report.runtime_field_count = runtime_field_count;

    std::map<std::string, std::map<std::string, ReviewedSignatureEntry>> reviewed_by_name;
    for (const auto& table : generated::TableDescriptors()) {
        const auto fields = generated::FieldsForTable(table.table_code);
        auto& entry = reviewed_by_name[std::string(table.table_name)][canonical_table_signature(fields)];
        if (entry.example_name.empty()) {
            entry.example_name = std::string(table.stream_name) + "." + std::string(table.table_name);
        }
        ++entry.count;
    }

    if (!settings.expected_scheme_sha256.empty() && report.runtime_scheme_sha256 != settings.expected_scheme_sha256) {
        push_issue(report.issues, Plaza2ProbeIssueCode::FileHashMismatch, true, scheme_path.filename().string(),
                   "runtime scheme hash mismatch: expected " + settings.expected_scheme_sha256 + ", got " +
                       report.runtime_scheme_sha256);
    }
    if (!settings.expected_spectra_release.empty() &&
        parsed.markers.spectra_release != settings.expected_spectra_release) {
        push_issue(report.issues, Plaza2ProbeIssueCode::UnsupportedVersion, true, scheme_path.filename().string(),
                   "runtime spectra release mismatch: expected " + settings.expected_spectra_release + ", got " +
                       parsed.markers.spectra_release);
    }

    std::set<std::string> table_names;
    for (const auto& [table_name, _] : reviewed_by_name) {
        table_names.insert(table_name);
    }
    for (const auto& [table_name, _] : runtime_by_name) {
        table_names.insert(table_name);
    }

    for (const auto& table_name : table_names) {
        const auto reviewed_it = reviewed_by_name.find(table_name);
        const auto runtime_it = runtime_by_name.find(table_name);
        if (reviewed_it == reviewed_by_name.end()) {
            push_issue(report.issues, Plaza2ProbeIssueCode::RuntimeTableUnexpected, true, table_name,
                       "runtime scheme exposes unexpected table name not present in Phase 3B baseline");
            continue;
        }
        if (runtime_it == runtime_by_name.end()) {
            push_issue(report.issues, Plaza2ProbeIssueCode::ReviewedTableMissing, true,
                       reviewed_it->second.begin()->second.example_name,
                       "runtime scheme is missing reviewed table family '" + table_name + "'");
            continue;
        }

        std::set<std::string> signatures;
        for (const auto& [signature, _] : reviewed_it->second) {
            signatures.insert(signature);
        }
        for (const auto& [signature, _] : runtime_it->second) {
            signatures.insert(signature);
        }
        for (const auto& signature : signatures) {
            const auto reviewed_signature_it = reviewed_it->second.find(signature);
            const auto runtime_signature_it = runtime_it->second.find(signature);
            const std::size_t reviewed_count =
                reviewed_signature_it == reviewed_it->second.end() ? 0 : reviewed_signature_it->second.count;
            const std::size_t runtime_count =
                runtime_signature_it == runtime_it->second.end() ? 0 : runtime_signature_it->second.count;

            if (reviewed_count == runtime_count) {
                continue;
            }
            if (reviewed_count > runtime_count) {
                const auto subject = reviewed_signature_it == reviewed_it->second.end()
                                         ? table_name
                                         : reviewed_signature_it->second.example_name;
                push_issue(report.issues,
                           runtime_count == 0 ? Plaza2ProbeIssueCode::ReviewedTableMissing
                                              : Plaza2ProbeIssueCode::ReviewedTableSignatureMismatch,
                           true, subject,
                           "runtime scheme diverged from reviewed table signature for '" + table_name + "'");
            } else {
                const auto subject = runtime_signature_it == runtime_it->second.end()
                                         ? table_name
                                         : runtime_signature_it->second.example_name;
                push_issue(report.issues, Plaza2ProbeIssueCode::RuntimeTableUnexpected, true, subject,
                           "runtime scheme exposes an unexpected table signature for '" + table_name + "'");
            }
        }
    }

    report.compatibility = compatibility_from_issues(report.issues);
    return report;
}

} // namespace

struct Plaza2RuntimeSharedState {
    Plaza2Settings settings;
    std::unique_ptr<RuntimeApi, RuntimeLibraryCloser> api;
};

struct RuntimeFieldPlan {
    generated::FieldCode field_code{kNoFieldCode};
    generated::ValueClass value_class{generated::ValueClass::kSignedInteger};
    std::string type_token;
    std::size_t offset{0};
    std::size_t size{0};
    std::size_t ordinal{0};
};

struct RuntimeMessagePlan {
    generated::TableCode table_code{kNoTableCode};
    std::size_t msg_index{0};
    std::string msg_name;
    std::vector<RuntimeFieldPlan> fields;
};

template <typename T> [[nodiscard]] T read_unaligned(const void* data) {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

[[nodiscard]] std::int64_t read_signed_integer(const void* data, std::size_t size) {
    switch (size) {
    case 1:
        return read_unaligned<std::int8_t>(data);
    case 2:
        return read_unaligned<std::int16_t>(data);
    case 4:
        return read_unaligned<std::int32_t>(data);
    case 8:
        return read_unaligned<std::int64_t>(data);
    default:
        return 0;
    }
}

[[nodiscard]] std::uint64_t read_unsigned_integer(const void* data, std::size_t size) {
    switch (size) {
    case 1:
        return read_unaligned<std::uint8_t>(data);
    case 2:
        return read_unaligned<std::uint16_t>(data);
    case 4:
        return read_unaligned<std::uint32_t>(data);
    case 8:
        return read_unaligned<std::uint64_t>(data);
    default:
        return 0;
    }
}

[[nodiscard]] std::string copy_c_string(const char* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return {};
    }
    const auto* end = static_cast<const char*>(std::memchr(data, '\0', size));
    const auto count = end == nullptr ? size : static_cast<std::size_t>(end - data);
    return std::string(data, count);
}

[[nodiscard]] std::uint64_t to_unix_seconds(const CgTime& value) {
    std::tm timestamp{};
    timestamp.tm_year = static_cast<int>(value.year) - 1900;
    timestamp.tm_mon = static_cast<int>(value.month) - 1;
    timestamp.tm_mday = static_cast<int>(value.day);
    timestamp.tm_hour = static_cast<int>(value.hour);
    timestamp.tm_min = static_cast<int>(value.minute);
    timestamp.tm_sec = static_cast<int>(value.second);
    return static_cast<std::uint64_t>(::timegm(&timestamp));
}

[[nodiscard]] Plaza2Error convert_runtime_field_to_string(RuntimeApi& api, std::string_view type_token,
                                                          const void* data, std::string& out) {
    if (api.getstr == nullptr) {
        return {
            .code = Plaza2ErrorCode::DecodeFailed,
            .runtime_code = 0,
            .message = "cg_getstr is not available in the loaded CGate runtime",
        };
    }

    std::size_t buffer_size = 64;
    out.assign(buffer_size, '\0');
    auto result = api.getstr(std::string(type_token).c_str(), data, out.data(), &buffer_size);
    if (result == kCgErrBufferTooSmall) {
        out.assign(buffer_size, '\0');
        result = api.getstr(std::string(type_token).c_str(), data, out.data(), &buffer_size);
    }
    if (result != kCgErrOk) {
        auto error = translate_plaza2_result("cg_getstr", result);
        if (!error) {
            error.code = Plaza2ErrorCode::DecodeFailed;
            error.message = "cg_getstr failed while decoding runtime field";
            error.runtime_code = result;
        }
        return error;
    }

    if (buffer_size < out.size()) {
        out.resize(buffer_size);
    }
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return {};
}

[[nodiscard]] const generated::TableDescriptor* find_table_descriptor_for_stream(generated::StreamCode stream_code,
                                                                                 std::string_view table_name) {
    for (const auto& table : generated::TablesForStream(stream_code)) {
        if (table.table_name == table_name) {
            return &table;
        }
    }
    return nullptr;
}

[[nodiscard]] const RuntimeMessagePlan* find_runtime_message_plan(std::span<const RuntimeMessagePlan> plans,
                                                                  std::size_t msg_index, std::string_view msg_name) {
    for (const auto& plan : plans) {
        if (plan.msg_index == msg_index) {
            return &plan;
        }
    }
    for (const auto& plan : plans) {
        if (plan.msg_name == msg_name) {
            return &plan;
        }
    }
    return nullptr;
}

struct Plaza2ListenerCallbackState {
    std::shared_ptr<Plaza2RuntimeSharedState> shared;
    generated::StreamCode stream_code{kNoStreamCode};
    Plaza2ListenerEventHandler* handler{nullptr};
    void* listener_handle{nullptr};
    bool scheme_loaded{false};
    std::vector<RuntimeMessagePlan> message_plans;
    std::vector<Plaza2DecodedFieldValue> decoded_fields;
    std::vector<std::string> text_storage;
    Plaza2Error last_error;
};

[[nodiscard]] Plaza2Error ensure_listener_scheme_loaded(Plaza2ListenerCallbackState& state) {
    if (state.scheme_loaded) {
        return {};
    }
    if (!state.shared || !state.shared->api || state.shared->api->lsn_getscheme == nullptr ||
        state.listener_handle == nullptr) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "listener scheme load requires an active CGate listener handle",
        };
    }

    void* raw_scheme = nullptr;
    const auto scheme_result = state.shared->api->lsn_getscheme(state.listener_handle, &raw_scheme);
    const auto scheme_error = translate_plaza2_result("cg_lsn_getscheme", scheme_result);
    if (scheme_error) {
        return scheme_error;
    }

    const auto* scheme = static_cast<CgSchemeDesc*>(raw_scheme);
    if (scheme == nullptr || scheme->messages == nullptr) {
        return {
            .code = Plaza2ErrorCode::DecodeFailed,
            .runtime_code = 0,
            .message = "CGate listener scheme is empty after CG_MSG_OPEN",
        };
    }

    state.message_plans.clear();
    std::size_t msg_index = 0;
    for (auto* message = scheme->messages; message != nullptr; message = message->next, ++msg_index) {
        const auto message_name = message->name == nullptr ? std::string{} : std::string(message->name);
        const auto* table = find_table_descriptor_for_stream(state.stream_code, message_name);
        if (table == nullptr) {
            continue;
        }

        RuntimeMessagePlan plan;
        plan.table_code = table->table_code;
        plan.msg_index = msg_index;
        plan.msg_name = message_name;

        auto* field = message->fields;
        std::size_t ordinal = 0;
        while (field != nullptr) {
            const auto field_name = field->name == nullptr ? std::string_view{} : std::string_view(field->name);
            const auto fields = generated::FieldsForTable(table->table_code);
            const generated::FieldDescriptor* descriptor = nullptr;
            for (const auto& candidate : fields) {
                if (candidate.field_name == field_name) {
                    descriptor = &candidate;
                    break;
                }
            }
            if (descriptor == nullptr) {
                return {
                    .code = Plaza2ErrorCode::DecodeFailed,
                    .runtime_code = 0,
                    .message = "runtime listener scheme field '" + std::string(field_name) +
                               "' is not covered by the reviewed metadata baseline",
                };
            }

            plan.fields.push_back({
                .field_code = descriptor->field_code,
                .value_class = descriptor->value_class,
                .type_token = field->type == nullptr ? std::string(descriptor->type_token) : std::string(field->type),
                .offset = field->offset,
                .size = field->size,
                .ordinal = ordinal,
            });
            field = field->next;
            ++ordinal;
        }

        state.message_plans.push_back(std::move(plan));
    }

    if (state.message_plans.empty()) {
        return {
            .code = Plaza2ErrorCode::DecodeFailed,
            .runtime_code = 0,
            .message = "listener scheme does not expose any reviewed tables for the configured stream",
        };
    }

    state.scheme_loaded = true;
    return {};
}

[[nodiscard]] Plaza2Error dispatch_listener_event(Plaza2ListenerCallbackState& state,
                                                  const Plaza2ListenerEvent& event) {
    if (state.handler == nullptr) {
        return {};
    }
    return state.handler->on_plaza2_listener_event(event);
}

[[nodiscard]] CgResult listener_callback_bridge(void*, void*, void* raw_msg, void* data) {
    auto* state = static_cast<Plaza2ListenerCallbackState*>(data);
    if (state == nullptr || raw_msg == nullptr) {
        return kCgErrOk;
    }

    auto fail = [&](Plaza2Error error) -> CgResult {
        state->last_error = std::move(error);
        return state->last_error.runtime_code == 0 ? kCgErrInternal : state->last_error.runtime_code;
    };

    try {
        const auto* msg = static_cast<CgMsg*>(raw_msg);
        switch (msg->type) {
        case kCgMsgOpen: {
            if (const auto error = ensure_listener_scheme_loaded(*state); error) {
                return fail(error);
            }
            const auto event = Plaza2ListenerEvent{
                .kind = Plaza2ListenerEventKind::Open,
                .stream_code = state->stream_code,
            };
            if (const auto error = dispatch_listener_event(*state, event); error) {
                return fail(error);
            }
            return kCgErrOk;
        }
        case kCgMsgClose: {
            const auto close_reason = msg->data != nullptr && msg->data_size >= sizeof(std::uint32_t)
                                          ? read_unaligned<std::uint32_t>(msg->data)
                                          : 0U;
            const auto event = Plaza2ListenerEvent{
                .kind = Plaza2ListenerEventKind::Close,
                .stream_code = state->stream_code,
                .close_reason = close_reason,
            };
            if (const auto error = dispatch_listener_event(*state, event); error) {
                return fail(error);
            }
            return kCgErrOk;
        }
        case kCgMsgTnBegin: {
            const auto event = Plaza2ListenerEvent{
                .kind = Plaza2ListenerEventKind::TransactionBegin,
                .stream_code = state->stream_code,
            };
            if (const auto error = dispatch_listener_event(*state, event); error) {
                return fail(error);
            }
            return kCgErrOk;
        }
        case kCgMsgTnCommit: {
            const auto event = Plaza2ListenerEvent{
                .kind = Plaza2ListenerEventKind::TransactionCommit,
                .stream_code = state->stream_code,
            };
            if (const auto error = dispatch_listener_event(*state, event); error) {
                return fail(error);
            }
            return kCgErrOk;
        }
        case kCgMsgP2replOnline: {
            const auto event = Plaza2ListenerEvent{
                .kind = Plaza2ListenerEventKind::Online,
                .stream_code = state->stream_code,
            };
            if (const auto error = dispatch_listener_event(*state, event); error) {
                return fail(error);
            }
            return kCgErrOk;
        }
        case kCgMsgP2replLifenum: {
            std::uint64_t life_number = 0;
            if (msg->data != nullptr && msg->data_size >= sizeof(CgDataLifeNum)) {
                life_number = static_cast<std::uint64_t>(static_cast<const CgDataLifeNum*>(msg->data)->life_number);
            } else if (msg->data != nullptr && msg->data_size >= sizeof(std::uint32_t)) {
                life_number = read_unaligned<std::uint32_t>(msg->data);
            }
            const auto event = Plaza2ListenerEvent{
                .kind = Plaza2ListenerEventKind::LifeNum,
                .stream_code = state->stream_code,
                .unsigned_value = life_number,
            };
            if (const auto error = dispatch_listener_event(*state, event); error) {
                return fail(error);
            }
            return kCgErrOk;
        }
        case kCgMsgP2replClearDeleted: {
            if (msg->data == nullptr || msg->data_size < sizeof(CgDataClearDeleted)) {
                return fail({
                    .code = Plaza2ErrorCode::DecodeFailed,
                    .runtime_code = 0,
                    .message = "CG_MSG_P2REPL_CLEARDELETED did not provide cg_data_cleardeleted_t payload",
                });
            }
            const auto payload = read_unaligned<CgDataClearDeleted>(msg->data);
            const auto* plan = find_runtime_message_plan(state->message_plans, payload.table_idx, {});
            if (plan == nullptr) {
                return fail({
                    .code = Plaza2ErrorCode::DecodeFailed,
                    .runtime_code = 0,
                    .message = "CG_MSG_P2REPL_CLEARDELETED referenced an unknown runtime table index",
                });
            }
            const auto event = Plaza2ListenerEvent{
                .kind = Plaza2ListenerEventKind::ClearDeleted,
                .stream_code = state->stream_code,
                .table_code = plan->table_code,
                .signed_value = payload.table_rev,
                .clear_deleted_flags = payload.flags,
            };
            if (const auto error = dispatch_listener_event(*state, event); error) {
                return fail(error);
            }
            return kCgErrOk;
        }
        case kCgMsgP2replReplState: {
            const auto text =
                msg->data == nullptr ? std::string_view{} : std::string_view(static_cast<const char*>(msg->data));
            const auto event = Plaza2ListenerEvent{
                .kind = Plaza2ListenerEventKind::ReplState,
                .stream_code = state->stream_code,
                .text_value = text,
            };
            if (const auto error = dispatch_listener_event(*state, event); error) {
                return fail(error);
            }
            return kCgErrOk;
        }
        case kCgMsgStreamData: {
            const auto* payload = static_cast<const CgMsgStreamData*>(raw_msg);
            const auto* plan = find_runtime_message_plan(
                state->message_plans, payload->msg_index,
                payload->msg_name == nullptr ? std::string_view{} : std::string_view(payload->msg_name));
            if (plan == nullptr) {
                return fail({
                    .code = Plaza2ErrorCode::DecodeFailed,
                    .runtime_code = 0,
                    .message =
                        "CG_MSG_STREAM_DATA referenced a runtime message that is not covered by the reviewed baseline",
                });
            }

            state->decoded_fields.clear();
            state->text_storage.clear();
            state->decoded_fields.reserve(plan->fields.size());
            state->text_storage.reserve(plan->fields.size());

            for (const auto& field : plan->fields) {
                if (payload->data == nullptr || payload->data_size < field.offset + field.size) {
                    return fail({
                        .code = Plaza2ErrorCode::DecodeFailed,
                        .runtime_code = 0,
                        .message = "CG_MSG_STREAM_DATA payload size is smaller than the runtime field plan",
                    });
                }
                if (payload->nulls != nullptr && field.ordinal < payload->num_nulls &&
                    payload->nulls[field.ordinal] != 0U) {
                    continue;
                }

                const auto* field_ptr = static_cast<const std::byte*>(payload->data) + field.offset;
                Plaza2DecodedFieldValue decoded{
                    .field_code = field.field_code,
                };

                switch (field.value_class) {
                case generated::ValueClass::kSignedInteger:
                    decoded.kind = Plaza2DecodedValueKind::SignedInteger;
                    decoded.signed_value = read_signed_integer(field_ptr, field.size);
                    break;
                case generated::ValueClass::kUnsignedInteger:
                    decoded.kind = Plaza2DecodedValueKind::UnsignedInteger;
                    decoded.unsigned_value = read_unsigned_integer(field_ptr, field.size);
                    break;
                case generated::ValueClass::kFixedString:
                    decoded.kind = Plaza2DecodedValueKind::String;
                    state->text_storage.push_back(
                        field.type_token == "a" ? copy_c_string(reinterpret_cast<const char*>(field_ptr), 1)
                                                : copy_c_string(reinterpret_cast<const char*>(field_ptr), field.size));
                    decoded.text_value = state->text_storage.back();
                    break;
                case generated::ValueClass::kDecimal:
                    decoded.kind = Plaza2DecodedValueKind::Decimal;
                    state->text_storage.emplace_back();
                    if (const auto error = convert_runtime_field_to_string(*state->shared->api, field.type_token,
                                                                           field_ptr, state->text_storage.back());
                        error) {
                        return fail(error);
                    }
                    decoded.text_value = state->text_storage.back();
                    break;
                case generated::ValueClass::kFloatingPoint:
                    decoded.kind = Plaza2DecodedValueKind::FloatingPoint;
                    state->text_storage.emplace_back();
                    if (const auto error = convert_runtime_field_to_string(*state->shared->api, field.type_token,
                                                                           field_ptr, state->text_storage.back());
                        error) {
                        return fail(error);
                    }
                    decoded.text_value = state->text_storage.back();
                    break;
                case generated::ValueClass::kTimestamp:
                    decoded.kind = Plaza2DecodedValueKind::Timestamp;
                    decoded.unsigned_value = to_unix_seconds(read_unaligned<CgTime>(field_ptr));
                    break;
                case generated::ValueClass::kBinary:
                    continue;
                }

                state->decoded_fields.push_back(decoded);
            }

            const auto event = Plaza2ListenerEvent{
                .kind = Plaza2ListenerEventKind::StreamData,
                .stream_code = state->stream_code,
                .table_code = plan->table_code,
                .fields = state->decoded_fields,
                .signed_value = payload->rev,
            };
            if (const auto error = dispatch_listener_event(*state, event); error) {
                return fail(error);
            }
            return kCgErrOk;
        }
        default:
            return kCgErrOk;
        }
    } catch (const std::exception& error) {
        return fail({
            .code = Plaza2ErrorCode::CallbackFailed,
            .runtime_code = 0,
            .message = std::string("PLAZA II listener callback failed: ") + error.what(),
        });
    } catch (...) {
        return fail({
            .code = Plaza2ErrorCode::CallbackFailed,
            .runtime_code = 0,
            .message = "PLAZA II listener callback failed with an unknown exception",
        });
    }
}

Plaza2Error validate_plaza2_settings(const Plaza2Settings& settings) {
    if (settings.runtime_root.empty()) {
        return {
            .code = Plaza2ErrorCode::InvalidConfiguration,
            .runtime_code = 0,
            .message = "runtime_root must be set for PLAZA II runtime probing",
        };
    }
    return {};
}

std::string make_plaza2_application_name(std::string_view prefix, std::string_view scope, std::uint32_t instance) {
    std::string sanitized_prefix = sanitize_token(prefix);
    std::string sanitized_scope = sanitize_token(scope);
    if (sanitized_prefix.empty()) {
        sanitized_prefix = "connectors";
    }
    if (sanitized_scope.empty()) {
        sanitized_scope = "plaza2";
    }
    std::ostringstream name;
    name << sanitized_prefix << '_' << sanitized_scope << '_' << instance;
    auto result = name.str();
    if (result.size() > 64) {
        result.resize(64);
    }
    return result;
}

Plaza2Error translate_plaza2_result(std::string_view operation, std::uint32_t runtime_code) {
    if (runtime_code == kCgErrOk || runtime_code == kCgErrTimeout) {
        return {};
    }

    Plaza2Error error;
    error.runtime_code = runtime_code;
    error.code = Plaza2ErrorCode::UnknownRuntimeResult;

    switch (runtime_code) {
    case kCgErrInvalidArgument:
        error.code = Plaza2ErrorCode::InvalidConfiguration;
        error.message = std::string(operation) + ": CG_ERR_INVALIDARGUMENT";
        break;
    case kCgErrIncorrectState:
        error.code = Plaza2ErrorCode::AdapterState;
        error.message = std::string(operation) + ": CG_ERR_INCORRECTSTATE";
        break;
    case kCgErrInternal:
        error.code = Plaza2ErrorCode::RuntimeCallFailed;
        error.message = std::string(operation) + ": CG_ERR_INTERNAL";
        break;
    case kCgErrUnsupported:
        error.code = Plaza2ErrorCode::RuntimeCallFailed;
        error.message = std::string(operation) + ": CG_ERR_UNSUPPORTED";
        break;
    case kCgErrMore:
        error.code = Plaza2ErrorCode::RuntimeCallFailed;
        error.message = std::string(operation) + ": CG_ERR_MORE";
        break;
    default:
        error.message = std::string(operation) + ": CGate returned runtime code " + std::to_string(runtime_code);
        break;
    }
    return error;
}

Plaza2RuntimeProbeReport Plaza2RuntimeProbe::probe(const Plaza2Settings& settings) {
    Plaza2RuntimeProbeReport report;
    report.layout.runtime_root = settings.runtime_root;
    report.layout.expected_config_files.assign(expected_config_filenames(settings.environment).begin(),
                                               expected_config_filenames(settings.environment).end());

    const auto validation_error = validate_plaza2_settings(settings);
    if (validation_error) {
        push_issue(report.issues, Plaza2ProbeIssueCode::UnsupportedLayout, true, "settings", validation_error.message);
        report.compatibility = Plaza2Compatibility::Incompatible;
        return report;
    }

    report.runtime_root_present = std::filesystem::exists(settings.runtime_root);
    if (!report.runtime_root_present) {
        push_issue(report.issues, Plaza2ProbeIssueCode::MissingRuntimeRoot, true, settings.runtime_root.string(),
                   "runtime root does not exist");
        report.compatibility = Plaza2Compatibility::Incompatible;
        return report;
    }

    const auto library_path = resolve_library_path(settings);
    if (!library_path.has_value()) {
        push_issue(report.issues, Plaza2ProbeIssueCode::MissingLibrary, true, settings.runtime_root.string(),
                   "CGate runtime library was not found under runtime_root/bin, runtime_root/lib, or runtime_root");
    } else {
        report.layout.library_path = *library_path;
        report.runtime_library_present = std::filesystem::exists(*library_path);
        auto library_issues = std::vector<Plaza2ProbeIssue>{};
        auto resolved_symbols = std::vector<std::string>{};
        auto api = load_runtime_api(*library_path, &resolved_symbols, &library_issues);
        report.runtime_library_loadable = api != nullptr;
        report.resolved_symbols = std::move(resolved_symbols);
        report.issues.insert(report.issues.end(), library_issues.begin(), library_issues.end());
    }

    const auto scheme_dir = resolve_scheme_dir(settings);
    if (!scheme_dir.has_value()) {
        push_issue(report.issues, Plaza2ProbeIssueCode::MissingSchemeDirectory, true, settings.runtime_root.string(),
                   "scheme directory was not found under runtime_root/scheme, runtime_root/Scheme, runtime_root/ini, "
                   "or runtime_root");
    } else {
        report.layout.scheme_dir = *scheme_dir;
        report.layout.scheme_path = *scheme_dir / kSchemeFilename;
        report.scheme_file_present = std::filesystem::exists(report.layout.scheme_path);
        if (!report.scheme_file_present) {
            push_issue(report.issues, Plaza2ProbeIssueCode::MissingSchemeFile, true, report.layout.scheme_path.string(),
                       "expected runtime scheme file is missing");
        } else {
            auto parsed_scheme = ParsedRuntimeScheme{};
            std::string parse_error;
            if (parse_runtime_scheme(report.layout.scheme_path, parsed_scheme, parse_error)) {
                report.layout.version_markers = parsed_scheme.markers;
            }
            report.scheme_drift = compare_runtime_scheme(report.layout.scheme_path, settings);
            report.issues.insert(report.issues.end(), report.scheme_drift.issues.begin(),
                                 report.scheme_drift.issues.end());
        }
    }

    const auto config_dir = resolve_config_dir(settings);
    if (!config_dir.has_value()) {
        push_issue(report.issues, Plaza2ProbeIssueCode::MissingConfigDirectory, true, settings.runtime_root.string(),
                   "config directory was not found under runtime_root/config, runtime_root/cfg, runtime_root/ini, or "
                   "runtime_root");
    } else {
        report.layout.config_dir = *config_dir;
        report.config_dir_present = true;
        report.layout.present_config_files = collect_present_config_files(*config_dir);
        const std::set<std::string> expected(report.layout.expected_config_files.begin(),
                                             report.layout.expected_config_files.end());
        const std::set<std::string> present(report.layout.present_config_files.begin(),
                                            report.layout.present_config_files.end());
        for (const auto& expected_name : expected) {
            if (!present.contains(expected_name)) {
                push_issue(report.issues, Plaza2ProbeIssueCode::MissingConfigFile, true, expected_name,
                           "expected config file is missing from resolved config directory");
            }
        }
        for (const auto& present_name : present) {
            if (!expected.contains(present_name)) {
                push_issue(report.issues, Plaza2ProbeIssueCode::UnexpectedConfigFile, false, present_name,
                           "unexpected config file is present in resolved config directory");
            }
        }
    }

    report.compatibility = compatibility_from_issues(report.issues);
    report.compatibility = combine_compatibility(report.compatibility, report.scheme_drift.compatibility);
    return report;
}

std::span<const std::string_view> Plaza2RuntimeProbe::expected_config_filenames(Plaza2Environment environment) {
    if (environment == Plaza2Environment::Prod) {
        return kProdConfigFiles;
    }
    return kTestConfigFiles;
}

std::span<const std::string_view> Plaza2RuntimeProbe::required_runtime_symbols() {
    return kRequiredSymbols;
}

Plaza2Env::~Plaza2Env() {
    if (open_) {
        static_cast<void>(close());
    }
}

Plaza2Error Plaza2Env::open(const Plaza2Settings& settings) {
    if (open_) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "cg_env_open called while PLAZA II environment is already open",
        };
    }
    if (settings.env_open_settings.empty()) {
        return {
            .code = Plaza2ErrorCode::InvalidConfiguration,
            .runtime_code = 0,
            .message = "env_open_settings must be set explicitly for Phase 3C adapter open",
        };
    }

    const auto library_path = resolve_library_path(settings);
    if (!library_path.has_value()) {
        return {
            .code = Plaza2ErrorCode::MissingRuntime,
            .runtime_code = 0,
            .message = "CGate runtime library could not be resolved from Plaza2Settings",
        };
    }

    auto state = std::make_shared<Plaza2RuntimeSharedState>();
    state->settings = settings;
    std::vector<Plaza2ProbeIssue> ignored_issues;
    state->api = load_runtime_api(*library_path, nullptr, &ignored_issues);
    if (!state->api) {
        return {
            .code = Plaza2ErrorCode::SymbolLoadFailed,
            .runtime_code = 0,
            .message = ignored_issues.empty() ? "failed to load CGate runtime library" : ignored_issues.front().message,
        };
    }

    const auto result = state->api->env_open(settings.env_open_settings.c_str());
    const auto translated = translate_plaza2_result("cg_env_open", result);
    if (translated) {
        return translated;
    }

    shared_ = std::move(state);
    open_ = true;
    return {};
}

Plaza2Error Plaza2Env::close() {
    if (!open_ || !shared_ || !shared_->api) {
        return {};
    }
    const auto result = shared_->api->env_close();
    const auto translated = translate_plaza2_result("cg_env_close", result);
    if (!translated) {
        open_ = false;
        shared_.reset();
    }
    return translated;
}

bool Plaza2Env::is_open() const noexcept {
    return open_;
}

Plaza2Connection::~Plaza2Connection() {
    if (handle_ != nullptr) {
        static_cast<void>(destroy());
    }
}

Plaza2Error Plaza2Connection::create(Plaza2Env& env, std::string_view settings) {
    if (!env.open_ || !env.shared_ || !env.shared_->api) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "connection create requires an open PLAZA II environment",
        };
    }
    if (handle_ != nullptr) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "connection is already created",
        };
    }
    if (settings.empty()) {
        return {
            .code = Plaza2ErrorCode::InvalidConfiguration,
            .runtime_code = 0,
            .message = "connection settings must be provided explicitly in Phase 3C",
        };
    }

    shared_ = env.shared_;
    void* raw_handle = nullptr;
    const auto result = shared_->api->conn_new(std::string(settings).c_str(), &raw_handle);
    const auto translated = translate_plaza2_result("cg_conn_new", result);
    if (translated) {
        shared_.reset();
        return translated;
    }
    handle_ = raw_handle;
    return {};
}

Plaza2Error Plaza2Connection::open(std::string_view settings) {
    if (!shared_ || !shared_->api || handle_ == nullptr) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "connection open requires a created connection",
        };
    }
    const std::string copied_settings(settings);
    const auto result = shared_->api->conn_open(handle_, copied_settings.empty() ? nullptr : copied_settings.c_str());
    return translate_plaza2_result("cg_conn_open", result);
}

Plaza2Error Plaza2Connection::process(std::uint32_t timeout_ms, std::uint32_t* runtime_code) {
    if (!shared_ || !shared_->api || handle_ == nullptr) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "connection process requires a created connection",
        };
    }
    const auto result = shared_->api->conn_process(handle_, timeout_ms, nullptr);
    if (runtime_code != nullptr) {
        *runtime_code = result;
    }
    return translate_plaza2_result("cg_conn_process", result);
}

Plaza2Error Plaza2Connection::close() {
    if (!shared_ || !shared_->api || handle_ == nullptr) {
        return {};
    }
    return translate_plaza2_result("cg_conn_close", shared_->api->conn_close(handle_));
}

Plaza2Error Plaza2Connection::destroy() {
    if (!shared_ || !shared_->api || handle_ == nullptr) {
        return {};
    }
    const auto result = shared_->api->conn_destroy(handle_);
    const auto translated = translate_plaza2_result("cg_conn_destroy", result);
    if (!translated) {
        handle_ = nullptr;
        shared_.reset();
    }
    return translated;
}

Plaza2Error Plaza2Connection::state(std::uint32_t& out_state) const {
    if (!shared_ || !shared_->api || handle_ == nullptr) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "connection state requires a created connection",
        };
    }
    return translate_plaza2_result("cg_conn_getstate", shared_->api->conn_getstate(handle_, &out_state));
}

bool Plaza2Connection::is_created() const noexcept {
    return handle_ != nullptr;
}

Plaza2Listener::Plaza2Listener() = default;

Plaza2Listener::~Plaza2Listener() {
    if (handle_ != nullptr) {
        static_cast<void>(destroy());
    }
}

Plaza2Listener::Plaza2Listener(Plaza2Listener&&) noexcept = default;

Plaza2Listener& Plaza2Listener::operator=(Plaza2Listener&&) noexcept = default;

Plaza2Error Plaza2Listener::create(Plaza2Connection& connection, std::string_view settings) {
    return create(connection, kNoStreamCode, settings, nullptr);
}

Plaza2Error Plaza2Listener::create(Plaza2Connection& connection, generated::StreamCode stream_code,
                                   std::string_view settings, Plaza2ListenerEventHandler* handler) {
    if (!connection.shared_ || !connection.shared_->api || connection.handle_ == nullptr) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "listener create requires a created connection",
        };
    }
    if (handle_ != nullptr) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "listener is already created",
        };
    }
    if (settings.empty()) {
        return {
            .code = Plaza2ErrorCode::InvalidConfiguration,
            .runtime_code = 0,
            .message = "listener settings must be provided explicitly in Phase 3C",
        };
    }

    shared_ = connection.shared_;
    callback_state_.reset();
    if (handler != nullptr) {
        callback_state_ = std::make_unique<Plaza2ListenerCallbackState>();
        callback_state_->shared = shared_;
        callback_state_->stream_code = stream_code;
        callback_state_->handler = handler;
    }

    void* raw_handle = nullptr;
    const std::string copied_settings(settings);
    const auto result =
        shared_->api->lsn_new(connection.handle_, copied_settings.c_str(),
                              handler == nullptr ? &noop_listener_callback : &listener_callback_bridge,
                              callback_state_ == nullptr ? nullptr : callback_state_.get(), &raw_handle);
    const auto translated = translate_plaza2_result("cg_lsn_new", result);
    if (translated) {
        callback_state_.reset();
        shared_.reset();
        return translated;
    }
    handle_ = raw_handle;
    if (callback_state_ != nullptr) {
        callback_state_->listener_handle = raw_handle;
    }
    return {};
}

Plaza2Error Plaza2Listener::open(std::string_view settings) {
    if (!shared_ || !shared_->api || handle_ == nullptr) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "listener open requires a created listener",
        };
    }
    const std::string copied_settings(settings);
    return translate_plaza2_result(
        "cg_lsn_open", shared_->api->lsn_open(handle_, copied_settings.empty() ? nullptr : copied_settings.c_str()));
}

Plaza2Error Plaza2Listener::close() {
    if (!shared_ || !shared_->api || handle_ == nullptr) {
        return {};
    }
    return translate_plaza2_result("cg_lsn_close", shared_->api->lsn_close(handle_));
}

Plaza2Error Plaza2Listener::destroy() {
    if (!shared_ || !shared_->api || handle_ == nullptr) {
        return {};
    }
    const auto result = shared_->api->lsn_destroy(handle_);
    const auto translated = translate_plaza2_result("cg_lsn_destroy", result);
    if (!translated) {
        handle_ = nullptr;
        callback_state_.reset();
        shared_.reset();
    }
    return translated;
}

Plaza2Error Plaza2Listener::state(std::uint32_t& out_state) const {
    if (!shared_ || !shared_->api || handle_ == nullptr) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = kCgErrIncorrectState,
            .message = "listener state requires a created listener",
        };
    }
    return translate_plaza2_result("cg_lsn_getstate", shared_->api->lsn_getstate(handle_, &out_state));
}

bool Plaza2Listener::is_created() const noexcept {
    return handle_ != nullptr;
}

} // namespace moex::plaza2::cgate
