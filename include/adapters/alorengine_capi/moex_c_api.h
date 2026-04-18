#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOEX_C_ABI_VERSION 1u

typedef struct MoexHandleTag* MoexConnectorHandle;

typedef enum MoexEventType {
    MOEX_EVENT_UNSPECIFIED = 0,
    MOEX_EVENT_CONNECTOR_STATUS = 1,
    MOEX_EVENT_ORDER_STATUS = 2,
    MOEX_EVENT_PRIVATE_TRADE = 3,
    MOEX_EVENT_POSITION = 4,
    MOEX_EVENT_PUBLIC_L1 = 5,
    MOEX_EVENT_PUBLIC_DIAGNOSTIC = 6,
    MOEX_EVENT_FULL_ORDER_LOG = 7,
    MOEX_EVENT_CERT_STEP = 8
} MoexEventType;

typedef enum MoexSourceConnector {
    MOEX_SOURCE_UNKNOWN = 0,
    MOEX_SOURCE_TWIME_TRADE = 1,
    MOEX_SOURCE_FIX_TRADE = 2,
    MOEX_SOURCE_FAST_MD = 3,
    MOEX_SOURCE_SIMBA_MD = 4,
    MOEX_SOURCE_PLAZA2_REPL = 5,
    MOEX_SOURCE_PLAZA2_TRADE = 6
} MoexSourceConnector;

typedef struct MoexEventHeader {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t event_type;
    uint64_t connector_seq;
    uint64_t source_seq;
    int64_t monotonic_time_ns;
    int64_t exchange_time_utc_ns;
    int64_t source_time_utc_ns;
    int64_t socket_receive_utc_ns;
    int64_t decode_utc_ns;
    int64_t publish_utc_ns;
    int64_t managed_poll_utc_ns;
    uint32_t source_connector;
    uint32_t flags;
} MoexEventHeader;

typedef struct MoexBackpressureCounters {
    uint64_t produced;
    uint64_t polled;
    uint64_t dropped;
    uint64_t high_watermark;
    bool overflowed;
    uint8_t reserved[7];
} MoexBackpressureCounters;

typedef void (*MoexLowRateCallback)(const MoexEventHeader* header, const void* payload, void* user_data);

const char* moex_phase0_abi_name(void);
uint32_t moex_phase0_abi_version(void);
bool moex_phase0_prod_requires_arm(const char* environment, bool armed);

#ifdef __cplusplus
}
#endif
