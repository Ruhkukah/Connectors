#pragma once
#include "server_schema.hpp"

// MOEX CGate 9.12.0.2144 vendor scheme, not synthesized from required metadata.
// Official archive: https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/cgate_linux_amd64-9.12.0.2144.zip
// Archive SHA-256: 73fb651459fdb1420f82f684e7c2fc3dbaa20dac085f9ff83fdc38fc35e16d61
// SPECTRA9.12.0/refdata.ini SHA-256: c4b5cc21bfc4164b1e8c1802429389bc0e880f5afed7bfa2c5d12701b0ffe450
// fut_sess_contents preserves vendor field order and CGate pack(4) offsets.
// Other tables continue using the independently captured 9.9 fixture.
namespace moex::plaza2::test {
inline std::span<const ServerSchemaField> spectra912_future_session_schema() {
    using generated::FieldCode;
    using generated::ValueClass;
    static constexpr ServerSchemaField fields[] = {
        {static_cast<FieldCode>(0x9737CA14u), "replID", "i8", ValueClass::kSignedInteger, 8, 0},
        {static_cast<FieldCode>(0x1DA88716u), "replRev", "i8", ValueClass::kSignedInteger, 8, 8},
        {static_cast<FieldCode>(0x17EADA17u), "replAct", "i8", ValueClass::kSignedInteger, 8, 16},
        {static_cast<FieldCode>(0xD7C818C6u), "sess_id", "i4", ValueClass::kSignedInteger, 4, 24},
        {static_cast<FieldCode>(0x929CA2D1u), "isin_id", "i4", ValueClass::kSignedInteger, 4, 28},
        {static_cast<FieldCode>(0x2F56B664u), "short_isin", "c25", ValueClass::kFixedString, 26, 32},
        {static_cast<FieldCode>(0x3B3C916Bu), "isin", "c25", ValueClass::kFixedString, 26, 58},
        {static_cast<FieldCode>(0x6AD0D2B7u), "name", "c75", ValueClass::kFixedString, 76, 84},
        {static_cast<FieldCode>(0x13837AD3u), "inst_term", "i4", ValueClass::kSignedInteger, 4, 160},
        {static_cast<FieldCode>(0x20B0445Cu), "base_contract_code", "c25", ValueClass::kFixedString, 26, 164},
        {static_cast<FieldCode>(0xD1A9DE0Du), "limit_up", "d16.5", ValueClass::kDecimal, 11, 190},
        {static_cast<FieldCode>(0xA9FABC2Cu), "limit_down", "d16.5", ValueClass::kDecimal, 11, 201},
        {static_cast<FieldCode>(0xAF69B3E5u), "settlement_price", "d16.5", ValueClass::kDecimal, 11, 212},
        {static_cast<FieldCode>(0x505AA721u), "buy_deposit", "d16.2", ValueClass::kDecimal, 10, 223},
        {static_cast<FieldCode>(0xA976032Du), "sell_deposit", "d16.2", ValueClass::kDecimal, 10, 233},
        {static_cast<FieldCode>(0x8AE22F95u), "roundto", "i4", ValueClass::kSignedInteger, 4, 244},
        {static_cast<FieldCode>(0xB4D33B47u), "min_step", "d16.5", ValueClass::kDecimal, 11, 248},
        {static_cast<FieldCode>(0xEF807F2Cu), "step_price", "d16.5", ValueClass::kDecimal, 11, 259},
        {static_cast<FieldCode>(0xAF4230AEu), "lot_volume", "i4", ValueClass::kSignedInteger, 4, 272},
        {static_cast<FieldCode>(0x00000000u), "xlot_volume", "d16.8", ValueClass::kDecimal, 10, 276},
        {static_cast<FieldCode>(0x0956CD32u), "last_trade_date", "t", ValueClass::kTimestamp, 10, 286},
        {static_cast<FieldCode>(0x5BD0E1CAu), "is_spread", "i1", ValueClass::kSignedInteger, 1, 296},
        {static_cast<FieldCode>(0x7A26E4FFu), "d_exp_start", "t", ValueClass::kTimestamp, 10, 298},
        {static_cast<FieldCode>(0x34966F6Cu), "percent_rate", "d6.2", ValueClass::kDecimal, 5, 308},
        {static_cast<FieldCode>(0x7C2AAB2Au), "signs", "i4", ValueClass::kSignedInteger, 4, 316},
        {static_cast<FieldCode>(0xC26D9694u), "multileg_type", "i4", ValueClass::kSignedInteger, 4, 320},
        {static_cast<FieldCode>(0x9AE21242u), "legs_qty", "i4", ValueClass::kSignedInteger, 4, 324},
        {static_cast<FieldCode>(0xB9A6BF0Cu), "step_price_clr", "d16.5", ValueClass::kDecimal, 11, 328},
        {static_cast<FieldCode>(0xBF391551u), "step_price_curr", "d16.5", ValueClass::kDecimal, 11, 339},
        {static_cast<FieldCode>(0xE89B4F7Au), "pctyield_coeff", "d16.5", ValueClass::kDecimal, 11, 350},
        {static_cast<FieldCode>(0xDED8E435u), "pctyield_total", "d16.5", ValueClass::kDecimal, 11, 361},
        {static_cast<FieldCode>(0x800D05CAu), "d_exp_end", "t", ValueClass::kTimestamp, 10, 372},
        {static_cast<FieldCode>(0x8AD95886u), "enforce_ims_half_netting", "i1", ValueClass::kSignedInteger, 1, 382},
        {static_cast<FieldCode>(0x09C25C66u), "tas_base_fut_isin_id", "i4", ValueClass::kSignedInteger, 4, 384},
        {static_cast<FieldCode>(0xEAFC32D0u), "trade_mode_id", "i4", ValueClass::kSignedInteger, 4, 388},
        {static_cast<FieldCode>(0x72CB4528u), "group_mask", "i8", ValueClass::kSignedInteger, 8, 392},
        {static_cast<FieldCode>(0xDAAA7D8Bu), "d_sess_assign", "t", ValueClass::kTimestamp, 10, 400},
        {static_cast<FieldCode>(0xFFB9E6C9u), "trade_period_access", "i8", ValueClass::kSignedInteger, 8, 412},
        {static_cast<FieldCode>(0xFA0CB9AAu), "buy_deposit_erc", "d16.2", ValueClass::kDecimal, 10, 420},
        {static_cast<FieldCode>(0xFC4F3F46u), "sell_deposit_erc", "d16.2", ValueClass::kDecimal, 10, 430},
        {static_cast<FieldCode>(0x71FC6B3Du), "buy_deposit_hrc", "d16.2", ValueClass::kDecimal, 10, 440},
        {static_cast<FieldCode>(0xDB1C2AE1u), "sell_deposit_hrc", "d16.2", ValueClass::kDecimal, 10, 450},
        {static_cast<FieldCode>(0x2E845649u), "buy_deposit_lrc", "d16.2", ValueClass::kDecimal, 10, 460},
        {static_cast<FieldCode>(0x1E943FD5u), "sell_deposit_lrc", "d16.2", ValueClass::kDecimal, 10, 470},
        {static_cast<FieldCode>(0x832607B2u), "buy_deposit_mrc", "d16.2", ValueClass::kDecimal, 10, 480},
        {static_cast<FieldCode>(0x7560A85Eu), "sell_deposit_mrc", "d16.2", ValueClass::kDecimal, 10, 490},
        {static_cast<FieldCode>(0x1B34BA69u), "settlement_price_clr", "d16.5", ValueClass::kDecimal, 11, 500},
        {static_cast<FieldCode>(0xFDE80621u), "state", "i4", ValueClass::kSignedInteger, 4, 512},
    };
    return fields;
}
inline constexpr std::size_t kSpectra912FutureSessionRowSize = 516;
} // namespace moex::plaza2::test
