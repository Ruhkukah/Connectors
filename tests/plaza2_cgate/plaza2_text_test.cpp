// Exercise the actual evidence writer, including instrument and system text.
#define main plaza2_authority_probe_application_main
#include "../../apps/plaza2_aggr20_authority_probe.cpp"
#undef main

#include <cstdlib>
#include <fstream>

namespace {
void require_text(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv) {
    using namespace moex::plaza2::cgate;
    using namespace moex::plaza2::cgate::text;
    require_text(argc == 2, "expected output JSON path");
    // Exact exchange bytes, independent of compiler source encoding.
    const std::string raw = "\xd4\xfc\xfe\xf7\xe5\xf0\xf1\xed\xfb\xe9 "
                            "\xea\xee\xed\xf2\xf0\xe0\xea\xf2 ALRS-12.26";
    const auto original = raw;
    const auto expected_u8 = u8"\u0424\u044c\u044e\u0447\u0435\u0440\u0441\u043d\u044b\u0439 "
                             u8"\u043a\u043e\u043d\u0442\u0440\u0430\u043a\u0442 ALRS-12.26";
    const std::string expected(reinterpret_cast<const char*>(expected_u8));
    const auto decoded = spectra_fixed_string_to_utf8(raw);
    require_text(decoded == expected && valid_utf8(decoded), "exact CP1251 fixture mismatch");
    require_text(raw == original, "raw evidence mutated");
    require_text(spectra_fixed_string_to_utf8(raw + std::string("\0junk", 5)) == expected,
                 "fixed string read beyond terminator");
    require_text(spectra_fixed_string_to_utf8("").empty(), "empty fixed string failed");
    require_text(spectra_fixed_string_to_utf8("ALRS-12.26") == "ALRS-12.26", "ASCII identifier changed");
    require_text(spectra_fixed_string_to_utf8("session_data_ready") == "session_data_ready",
                 "ASCII system event changed");
    // CP1251 bytes may coincidentally form UTF-8: no autodetection is allowed.
    require_text(windows1251_to_utf8("\xd0\x90") == "\xd0\xa0\xd1\x92", "encoding was guessed");
    require_text(json_escape(decoded) == decoded, "UTF-8 was double converted by JSON writer");
    require_text(windows1251_to_utf8("\x98") == "\xef\xbf\xbd", "undefined CP1251 policy failed");
    for (const std::string bad : {"\x80", "\xc0\xaf", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80",
                                  "\xf5\x80\x80\x80", "\xf0\x9f", "\xff"}) {
        require_text(!valid_utf8(bad), "invalid UTF-8 accepted");
        require_text(valid_utf8(json_escape(bad)), "invalid bytes leaked into JSON");
        require_text(json_escape(bad).find("\\ufffd") != std::string::npos, "invalid bytes not escaped");
        require_text(json_quote_utf8(bad).starts_with('"') && json_quote_utf8(bad).ends_with('"'),
                     "shared JSON string serializer omitted quotes");
    }
    require_text(valid_utf8("\xf0\x9f\x98\x80") && valid_utf8("\xf4\x8f\xbf\xbf"),
                 "valid supplementary UTF-8 rejected");
    require_text(json_escape(std::string("\0\n\t\r\"\\", 6)) == "\\u0000\\u000a\\u0009\\u000d\\\"\\\\",
                 "JSON controls not escaped");
    Plaza2Aggr20AuthorityProbeReport report;
    report.selection = Plaza2Aggr20AuthorityProbeSelection{.symbol = "ALRS-12.26", .name = decoded};
    Plaza2Aggr20AuthorityProbeAttempt attempt;
    attempt.sys_events.push_back({.message = spectra_fixed_string_to_utf8("\xd1\xe5\xf1\xf1\xe8\xff")});
    attempt.sys_events.push_back({.message = "session_data_ready"});
    attempt.sys_events.push_back({.message = "broken\xff"});
    report.attempts.push_back(attempt);
    report.error = "bad\xed\xa0\x80";
    write_report_json(argv[1], report);
    const auto malformed_receipt_value = std::string("\xd0\x90") + R"( "quoted" \)" + std::string("\0\x01\x1f\xff", 4);
    std::ofstream runner_receipt(std::string(argv[1]) + ".runner.json", std::ios::binary);
    runner_receipt << "{\"metadata_507_ready\":false,\"symbol\":" << json_quote_utf8(malformed_receipt_value)
                   << ",\"invalid_utf8_raw_hex\":{\"symbol\":\"ff\"}}";
    require_text(static_cast<bool>(runner_receipt), "runner JSON fixture write failed");
    // Independent Python codec oracle checks all 256 byte mappings.
    for (unsigned int byte = 0; byte < 256; ++byte) {
        const char raw_byte = static_cast<char>(byte);
        std::cout << '"' << json_escape(windows1251_to_utf8({&raw_byte, 1})) << "\"\n";
    }
    return 0;
}
