#include "moex/twime_trade/transport/twime_credential_provider.hpp"
#include "moex/twime_trade/transport/twime_credential_redaction.hpp"

#include "twime_test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    try {
        using namespace moex::twime_trade::transport;

        ::setenv("MOEX_TWIME_TEST_CREDENTIALS", "TEST-CREDENTIALS", 1);
        const auto env_credentials = EnvTwimeCredentialProvider("MOEX_TWIME_TEST_CREDENTIALS").load();
        moex::twime_sbe::test::require(env_credentials.has_value() &&
                                           env_credentials->credentials == "TEST-CREDENTIALS",
                                       "env credential provider failed to load credentials");
        ::unsetenv("MOEX_TWIME_TEST_CREDENTIALS");
        moex::twime_sbe::test::require(!EnvTwimeCredentialProvider("MOEX_TWIME_TEST_CREDENTIALS").load().has_value(),
                                       "missing env credential should fail cleanly");

        const auto temp_path =
            std::filesystem::temp_directory_path() / "moex_connector_twime_credential_provider_test.txt";
        {
            std::ofstream output(temp_path);
            output << "FILE-CREDENTIALS\n";
        }

        const auto file_credentials = FileTwimeCredentialProvider(temp_path).load();
        moex::twime_sbe::test::require(file_credentials.has_value() &&
                                           file_credentials->credentials == "FILE-CREDENTIALS",
                                       "file credential provider failed to load credentials");
        std::filesystem::remove(temp_path);

        moex::twime_sbe::test::require(redact_twime_credentials("TEST-CREDENTIALS") == "[REDACTED]",
                                       "TWIME credential redaction mismatch");
        moex::twime_sbe::test::require(redact_twime_account_like_value("ABCD1234") == "AB***34",
                                       "account-like redaction mismatch");
        moex::twime_sbe::test::require(format_twime_test_network_banner(true) == "[TEST-NETWORK-ARMED]",
                                       "armed test-network banner mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
