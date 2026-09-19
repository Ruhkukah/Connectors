# Pinned official DTC schema source

- URL: https://www.sierrachart.com/DTC_Files/DTCProtocol.proto
- Retrieved: 2026-09-19
- SHA-256: `cc8c6f531db390255c273bcbf673d176f58a7e9e509e43e13fbfecdca70c4f67`
- Protocol states `CURRENT_VERSION = 8`.
- Official `SecurityTypeEnum.SECURITY_TYPE_FUTURE = 1`.
- Official `SecurityDefinitionResponse.SecurityType = 4`.
- Official `SecurityDefinitionResponse.IsFinalMessage = 9`.

The adjacent `.proto` is a minimal generated-test fixture containing those exact official enum/message field declarations, rather than a fork of the full changing upstream file. Its generated C# protobuf parser decodes bytes produced by the C++ DTC server. Review/update this pinned schema hash deliberately when taking a later upstream protocol snapshot.
