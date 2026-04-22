#!/usr/bin/env python3
from __future__ import annotations

import argparse
import tempfile
from pathlib import Path

from moex_phase0_common import dump_yaml, load_json_yaml, stable_id


DOC_ARTIFACT_ROLES = {
    "cgate_en.pdf": {
        "display_name": "CGate API reference PDF",
        "role": "runtime_api_reference",
        "version_marker": "manifest_locked",
    },
    "p2gate_en.html": {
        "display_name": "PLAZA II gateway HTML reference",
        "role": "primary_repl_spec",
        "version_marker": "PLAZA II gateway version 9.3",
    },
    "p2gate_en.pdf": {
        "display_name": "PLAZA II gateway PDF reference",
        "role": "portable_repl_spec",
        "version_marker": "PLAZA II gateway version 9.3",
    },
    "p2micexgate_en.pdf": {
        "display_name": "P2MICEX gateway PDF reference",
        "role": "gateway_topology_reference",
        "version_marker": "manifest_locked",
    },
    "style.css": {
        "display_name": "PLAZA II HTML stylesheet",
        "role": "support_asset",
        "version_marker": "not_applicable",
    },
}

STREAMS = [
    ("FORTS_TRADE_REPL", "R", "User's orders and trades"),
    ("FORTS_ORDLOG_REPL", "R", "anonymous orders"),
    ("FORTS_DEALS_REPL", "R", "anonymous trades"),
    ("FORTS_FEE_REPL", "AR", "exchange fees and penalties"),
    ("FORTS_FEERATE_REPL", "AR", "Precise Exchange fee rates"),
    ("FORTS_BROKER_FEE_REPL", "I", "Brokerage fees"),
    ("FORTS_BROKER_FEE_PARAMS_REPL", "I", "Parameters for calculating the brokerage fee"),
    ("FORTS_USERORDERBOOK_REPL", "R", "User orders: order-book snapshot"),
    ("FORTS_ORDBOOK_REPL", "R", "Depersonalized order-book snapshot"),
    ("FORTS_COMMON_REPL", "I", "Market fundamentals"),
    ("FORTS_AGGR_REPL", "I", "Aggregated order-book streams"),
    ("FORTS_POS_REPL", "I", "information on positions"),
    ("FORTS_PART_REPL", "I", "Information about limits"),
    ("FORTS_PROHIBITION_REPL", "R", "Prohibitions"),
    ("FORTS_REFDATA_REPL", "R", "Reference and session information"),
    ("FORTS_MM_REPL", "I", "information on MM's obligations"),
    ("FORTS_CLR_REPL", "AR", "clearing information"),
    ("RTS_INDEX_REPL", "R", "online indices"),
    ("FORTS_VM_REPL", "I", "Variation margin and premium"),
    ("FORTS_VOLAT_REPL", "I", "online volatility information"),
    ("FORTS_RISKINFOBLACK_REPL", "I", "Risk parameters for Black-Scholes model"),
    ("FORTS_RISKINFOBACH_REPL", "I", "Risk parameters for Bachelier model"),
    ("FORTS_INFO_REPL", "R", "additional reference information"),
    ("FORTS_TNPENALTY_REPL", "I", "information about Transaction fees"),
    ("MOEX_RATES_REPL", "I", "online currency rates"),
    ("FORTS_FORECASTIM_REPL", "I", "Risk forecast after limits extension"),
    ("FORTS_USER_REPL", "R", "Users"),
    ("FORTS_REJECTEDORDERS_REPL", "R", "Register of orders rejected during the clearing"),
    ("FORTS_RMT_REPL", "I", "Collateral without orders and current operational risk"),
    ("FORTS_SESSIONSTATE_REPL", "I", "Current trading day status"),
    ("FORTS_INSTRUMENTSTATE_REPL", "I", "Statuses of instruments for the current trading day"),
    ("FORTS_SECURITYGROUPSTATE_REPL", "I", "Group status of instruments for the current trading day"),
]

TABLES = [
    ("FORTS_TRADE_REPL", "orders_log", "Log of operations with orders"),
    ("FORTS_TRADE_REPL", "multileg_orders_log", "Log of operations with multileg orders"),
    ("FORTS_TRADE_REPL", "user_deal", "User trades"),
    ("FORTS_TRADE_REPL", "user_multileg_deal", "User's multileg orders trades"),
    ("FORTS_TRADE_REPL", "heartbeat", "Server times table"),
    ("FORTS_TRADE_REPL", "sys_events", "table of events"),
    ("FORTS_ORDLOG_REPL", "orders_log", "Log of operations with orders"),
    ("FORTS_ORDLOG_REPL", "multileg_orders_log", "Log of operations with multileg orders"),
    ("FORTS_ORDLOG_REPL", "heartbeat", "Server times table"),
    ("FORTS_ORDLOG_REPL", "sys_events", "table of events"),
    ("FORTS_DEALS_REPL", "deal", "Trades"),
    ("FORTS_DEALS_REPL", "multileg_deal", "Multileg trades"),
    ("FORTS_DEALS_REPL", "heartbeat", "Server times table"),
    ("FORTS_DEALS_REPL", "sys_events", "table of events"),
    ("FORTS_FEE_REPL", "adjusted_fee", "exchange fees"),
    ("FORTS_FEE_REPL", "penalty", "penalties"),
    ("FORTS_FEE_REPL", "sys_events", "table of events"),
    ("FORTS_FEERATE_REPL", "futures_rate", "fee rates on futures and multi-leg instruments"),
    ("FORTS_FEERATE_REPL", "option_rate", "fee rates on option contracts"),
    ("FORTS_FEERATE_REPL", "sys_events", "table of events"),
    ("FORTS_BROKER_FEE_REPL", "broker_fee", "brokerage fee"),
    ("FORTS_BROKER_FEE_REPL", "sys_events", "table of events"),
    ("FORTS_BROKER_FEE_PARAMS_REPL", "broker_fee_params", "Parameters for calculating the brokerage fee"),
    ("FORTS_BROKER_FEE_PARAMS_REPL", "sys_events", "table of events"),
    ("FORTS_USERORDERBOOK_REPL", "orders", "Current futures and options order-book"),
    ("FORTS_USERORDERBOOK_REPL", "multileg_orders", "Table of active multileg orders"),
    ("FORTS_USERORDERBOOK_REPL", "info", "Order-book snapshots information"),
    (
        "FORTS_USERORDERBOOK_REPL",
        "orders_currentday",
        "Snapshot of active user orders at the start of the current day",
    ),
    (
        "FORTS_USERORDERBOOK_REPL",
        "multileg_orders_currentday",
        "Snapshot of active user multileg orders at the start of the current day",
    ),
    ("FORTS_USERORDERBOOK_REPL", "info_currentday", "Snapshot information"),
    ("FORTS_ORDBOOK_REPL", "orders", "Current order-book"),
    ("FORTS_ORDBOOK_REPL", "multileg_orders", "Table of active multileg orders"),
    ("FORTS_ORDBOOK_REPL", "info", "Order-book snapshots information"),
    ("FORTS_ORDBOOK_REPL", "orders_currentday", "Snapshot of active orders at the start of the current day"),
    (
        "FORTS_ORDBOOK_REPL",
        "multileg_orders_currentday",
        "Snapshot of active multileg orders at the start of the current day",
    ),
    ("FORTS_ORDBOOK_REPL", "info_currentday", "Snapshot information"),
    ("FORTS_COMMON_REPL", "common", "Market fundamentals"),
    ("FORTS_COMMON_REPL", "sys_events", "table of events"),
    ("FORTS_AGGR_REPL", "orders_aggr", "Aggregated order-books"),
    ("FORTS_AGGR_REPL", "sys_events", "table of events"),
    ("FORTS_POS_REPL", "position", "Client positions"),
    ("FORTS_POS_REPL", "position_sa", "Settlement Account positions"),
    ("FORTS_POS_REPL", "info", "Snapshot information"),
    ("FORTS_POS_REPL", "sys_events", "table of events"),
    ("FORTS_PART_REPL", "part", "Limits on clients and brokerage firms"),
    ("FORTS_PART_REPL", "part_sa", "Limits by Settlement Account"),
    ("FORTS_PART_REPL", "sys_events", "table of events"),
    ("FORTS_PROHIBITION_REPL", "prohibition", "Prohibitions"),
    ("FORTS_PROHIBITION_REPL", "sys_events", "table of events"),
    ("FORTS_REFDATA_REPL", "rates", "Currency rates dictionary"),
    ("FORTS_REFDATA_REPL", "fut_sess_contents", "Traded instruments directory (futures)"),
    ("FORTS_REFDATA_REPL", "fut_vcb", "Traded assets directory (futures)"),
    ("FORTS_REFDATA_REPL", "fut_instruments", "Instruments dictionary"),
    ("FORTS_REFDATA_REPL", "fut_bond_registry", "Spot asset parameters directory"),
    ("FORTS_REFDATA_REPL", "dealer", "Companies directory"),
    ("FORTS_REFDATA_REPL", "sys_messages", "Trading system messages"),
    ("FORTS_REFDATA_REPL", "opt_sess_contents", "Traded instruments directory (options)"),
    ("FORTS_REFDATA_REPL", "opt_vcb", "Traded assets directory (options)"),
    ("FORTS_REFDATA_REPL", "multileg_dict", "Multileg instruments dictionary"),
    (
        "FORTS_REFDATA_REPL",
        "fut_intercl_info",
        "Information on the variation margin on futures, calculated based on the results of intraday clearing",
    ),
    (
        "FORTS_REFDATA_REPL",
        "opt_intercl_info",
        "Information on variation margin and premium on options calculated based on the results of intraday clearing",
    ),
    ("FORTS_REFDATA_REPL", "opt_exp_orders", "Register of requests for exercise of option"),
    ("FORTS_REFDATA_REPL", "fut_bond_nkd", "Accrued interest as of the bond futures contract expiration date"),
    ("FORTS_REFDATA_REPL", "fut_bond_nominal", "Payment of bonds' face value"),
    ("FORTS_REFDATA_REPL", "fut_bond_isin", "Directory of compliance of instruments with spot assets"),
    ("FORTS_REFDATA_REPL", "user", "System users"),
    ("FORTS_REFDATA_REPL", "sess_option_series", "Option series"),
    ("FORTS_REFDATA_REPL", "investor", "Clients directory"),
    ("FORTS_REFDATA_REPL", "fut_margin_type", "Type of margining"),
    ("FORTS_REFDATA_REPL", "fut_settlement_account", "Settlement Account"),
    ("FORTS_REFDATA_REPL", "sma_master", "SMA login binding to MASTER login"),
    ("FORTS_REFDATA_REPL", "sma_pre_trade_check", "SMA login pre-trade verification settings"),
    ("FORTS_REFDATA_REPL", "clearing_members", "Clearing Members"),
    ("FORTS_REFDATA_REPL", "instr2matching_map", "Instrument binding to Matching ID"),
    (
        "FORTS_REFDATA_REPL",
        "fut_exec_orders",
        "Exercise requests of daily futures contracts with auto-prolongation",
    ),
    ("FORTS_REFDATA_REPL", "discrete_auction", "Parameters of assigned opening auctions"),
    (
        "FORTS_REFDATA_REPL",
        "discrete_auction_base_contract",
        "Underlying contracts assigned to the opening auction",
    ),
    ("FORTS_REFDATA_REPL", "trade_periods", "Trading period parameters"),
    ("FORTS_REFDATA_REPL", "session", "Information about a trading day"),
    (
        "FORTS_REFDATA_REPL",
        "brokers_base_contracts_params",
        "Individual coefficient of IM in the context of the underlying contract and BF",
    ),
    ("FORTS_REFDATA_REPL", "sys_events", "table of events"),
    (
        "FORTS_MM_REPL",
        "mm_agreement_filter",
        "Table numbers and types of contracts for the provision of market-making services",
    ),
    ("FORTS_MM_REPL", "fut_MM_info", "MM's obligations in futures"),
    ("FORTS_MM_REPL", "opt_MM_info", "MM's obligations in options"),
    ("FORTS_MM_REPL", "cs_mm_rule", "Instruments for recalculating the central strike price."),
    ("FORTS_CLR_REPL", "money_clearing", "Balances for clients and brokerage firms"),
    ("FORTS_CLR_REPL", "money_clearing_sa", "Balances by Settlement Account"),
    ("FORTS_CLR_REPL", "clr_rate", "Currency and Index rates"),
    ("FORTS_CLR_REPL", "fut_pos", "Positional state in futures as a result of clearing session"),
    ("FORTS_CLR_REPL", "opt_pos", "Positional state in options as a result of clearing session"),
    ("FORTS_CLR_REPL", "fut_sess_settl", "Futures settlement prices"),
    ("FORTS_CLR_REPL", "opt_sess_settl", "Options settlement prices"),
    ("FORTS_CLR_REPL", "pledge_details", "Pledgs details table"),
    ("FORTS_CLR_REPL", "fut_pos_sa", "Positional state of SA on futures as a result of clearing session"),
    ("FORTS_CLR_REPL", "opt_pos_sa", "Positional state of SA on options as a result of clearing session"),
    ("FORTS_CLR_REPL", "option_series_settl", "Settlement prices for option series"),
    ("FORTS_CLR_REPL", "sys_events", "table of events"),
    ("RTS_INDEX_REPL", "rts_index", "Indices"),
    ("FORTS_VM_REPL", "fut_vm", "Variation margin on futures by positions of clients"),
    (
        "FORTS_VM_REPL",
        "opt_vm",
        "Variation margin and premium on options in the context of client positions",
    ),
    ("FORTS_VM_REPL", "fut_vm_sa", "Variation margin on futures in the context of SA positions"),
    ("FORTS_VM_REPL", "opt_vm_sa", "Variation margin for options"),
    ("FORTS_VM_REPL", "sys_events", "Table of events"),
    ("FORTS_VOLAT_REPL", "volat", "Volatility"),
    ("FORTS_VOLAT_REPL", "sys_events", "Table of events"),
    ("FORTS_RISKINFOBLACK_REPL", "volat_coeff", "Risk parameters for Black-Scholes model"),
    ("FORTS_RISKINFOBACH_REPL", "volat_coeff", "Risk parameters for Bachelier model"),
    ("FORTS_INFO_REPL", "currency_params", "FX parameters"),
    ("FORTS_INFO_REPL", "base_contracts_params", "Base contracts parameters"),
    ("FORTS_INFO_REPL", "futures_params", "Futures parameters"),
    ("FORTS_INFO_REPL", "option_series_params", "Parameters for series of options"),
    ("FORTS_INFO_REPL", "options_params", "Options parameters"),
    ("FORTS_INFO_REPL", "investor", "Clients directory"),
    ("FORTS_INFO_REPL", "dealer", "Companies directory"),
    ("FORTS_INFO_REPL", "multileg_dictionary", "Multileg instruments dictionary"),
    ("FORTS_INFO_REPL", "common_params", "Collateral calculation parameters"),
    (
        "FORTS_INFO_REPL",
        "brokers_base_contracts_params",
        "Individual coefficient of IM in the context of the underlying contract and BF",
    ),
    ("FORTS_INFO_REPL", "sys_events", "Table of events"),
    ("FORTS_TNPENALTY_REPL", "fee_tn", "Detailed information on the number of incorrect transaction"),
    ("FORTS_TNPENALTY_REPL", "fee_all", "Information on the number of points accrued"),
    ("FORTS_TNPENALTY_REPL", "heartbeat", "Server times table"),
    ("MOEX_RATES_REPL", "curr_online", "Currency rates values"),
    ("FORTS_FORECASTIM_REPL", "part_sa_forecast", "Free funds for SA volume forecast"),
    ("FORTS_FORECASTIM_REPL", "sys_events", "Table of events"),
    ("FORTS_USER_REPL", "user", "System users"),
    ("FORTS_USER_REPL", "sma_master", "SMA login binding to MASTER login"),
    ("FORTS_USER_REPL", "sma_pre_trade_check", "SMA login pre-trade verification settings"),
    ("FORTS_USER_REPL", "sys_events", "Table of events"),
    ("FORTS_REJECTEDORDERS_REPL", "rejected_orders", "Register of orders rejected during the clearing"),
    (
        "FORTS_RMT_REPL",
        "rmt_im",
        "Collateral without orders and current operational risk in the context of clients",
    ),
    ("FORTS_RMT_REPL", "sys_events", "table of events"),
    ("FORTS_SESSIONSTATE_REPL", "session_state", "Current trading day status"),
    ("FORTS_SESSIONSTATE_REPL", "sys_events", "table of events"),
    ("FORTS_INSTRUMENTSTATE_REPL", "instrument_state", "Statuses of instruments for the current trading day"),
    ("FORTS_INSTRUMENTSTATE_REPL", "sys_events", "table of events"),
    (
        "FORTS_SECURITYGROUPSTATE_REPL",
        "security_group_state",
        "Group status of instruments for the current trading day",
    ),
    ("FORTS_SECURITYGROUPSTATE_REPL", "sys_events", "table of events"),
]

LEGACY_PROTOCOL_ITEMS = [
    {
        "protocol_item_id": "plaza2_router_split",
        "artifact_id": "plaza_docs",
        "name": "PLAZA II split into repl/trade/router/scheme",
        "kind": "architecture",
    },
    {
        "protocol_item_id": "plaza2_scheme_lock",
        "artifact_id": "cgate_docs",
        "name": "Installed scheme lock and generated bindings fingerprint",
        "kind": "artifact_lock",
    },
    {
        "protocol_item_id": "plaza2_twime_profile_neutrality",
        "artifact_id": "plaza_docs",
        "name": "Profile-neutral plaza2_twime composition",
        "kind": "profile",
    },
]

PRIVATE_CORE_STREAMS = {
    "FORTS_INFO_REPL",
    "FORTS_INSTRUMENTSTATE_REPL",
    "FORTS_PART_REPL",
    "FORTS_POS_REPL",
    "FORTS_REFDATA_REPL",
    "FORTS_SECURITYGROUPSTATE_REPL",
    "FORTS_SESSIONSTATE_REPL",
    "FORTS_TRADE_REPL",
    "FORTS_USERORDERBOOK_REPL",
}

PRIVATE_AUXILIARY_STREAMS = {
    "FORTS_CLR_REPL",
    "FORTS_FEERATE_REPL",
    "FORTS_FORECASTIM_REPL",
    "FORTS_PROHIBITION_REPL",
    "FORTS_REJECTEDORDERS_REPL",
    "FORTS_RMT_REPL",
    "FORTS_USER_REPL",
    "FORTS_VM_REPL",
}

PUBLIC_COMMON_STREAMS = {
    "FORTS_COMMON_REPL",
    "FORTS_DEALS_REPL",
}

PUBLIC_AGGR_STREAMS = {
    "FORTS_AGGR_REPL",
}

PUBLIC_ORDLOG_STREAMS = {
    "FORTS_ORDLOG_REPL",
    "FORTS_ORDBOOK_REPL",
}

DEFERRED_STREAMS = {
    "FORTS_BROKER_FEE_PARAMS_REPL",
    "FORTS_BROKER_FEE_REPL",
    "FORTS_FEE_REPL",
    "FORTS_MM_REPL",
    "FORTS_RISKINFOBACH_REPL",
    "FORTS_RISKINFOBLACK_REPL",
    "FORTS_TNPENALTY_REPL",
    "FORTS_VOLAT_REPL",
    "MOEX_RATES_REPL",
    "RTS_INDEX_REPL",
}

LOGIN_SUBTYPES = {
    "main": [
        "FORTS_CLR_REPL",
        "FORTS_FEERATE_REPL",
        "FORTS_BROKER_FEE_PARAMS_REPL",
        "FORTS_BROKER_FEE_REPL",
        "FORTS_FEE_REPL",
        "FORTS_PROHIBITION_REPL",
        "FORTS_REFDATA_REPL",
        "FORTS_TRADE_REPL",
        "FORTS_MM_REPL",
        "FORTS_USERORDERBOOK_REPL",
        "FORTS_FORECASTIM_REPL",
        "FORTS_INFO_REPL",
        "FORTS_PART_REPL",
        "FORTS_POS_REPL",
        "FORTS_TNPENALTY_REPL",
        "FORTS_VM_REPL",
        "FORTS_DEALS_REPL",
        "FORTS_COMMON_REPL",
        "FORTS_VOLAT_REPL",
        "MOEX_RATES_REPL",
        "RTS_INDEX_REPL",
        "FORTS_RISKINFOBLACK_REPL",
        "FORTS_RISKINFOBACH_REPL",
        "FORTS_USER_REPL",
        "FORTS_REJECTEDORDERS_REPL",
        "FORTS_RMT_REPL",
        "FORTS_SESSIONSTATE_REPL",
        "FORTS_INSTRUMENTSTATE_REPL",
        "FORTS_SECURITYGROUPSTATE_REPL",
        "FORTS_AGGR_REPL",
        "FORTS_ORDLOG_REPL",
        "FORTS_ORDBOOK_REPL",
    ],
    "viewing": [
        "FORTS_CLR_REPL",
        "FORTS_FEERATE_REPL",
        "FORTS_BROKER_FEE_PARAMS_REPL",
        "FORTS_BROKER_FEE_REPL",
        "FORTS_FEE_REPL",
        "FORTS_PROHIBITION_REPL",
        "FORTS_REFDATA_REPL",
        "FORTS_TRADE_REPL",
        "FORTS_MM_REPL",
        "FORTS_USERORDERBOOK_REPL",
        "FORTS_FORECASTIM_REPL",
        "FORTS_INFO_REPL",
        "FORTS_PART_REPL",
        "FORTS_POS_REPL",
        "FORTS_TNPENALTY_REPL",
        "FORTS_VM_REPL",
        "FORTS_DEALS_REPL",
        "FORTS_COMMON_REPL",
        "FORTS_VOLAT_REPL",
        "MOEX_RATES_REPL",
        "RTS_INDEX_REPL",
        "FORTS_RISKINFOBLACK_REPL",
        "FORTS_RISKINFOBACH_REPL",
        "FORTS_USER_REPL",
        "FORTS_REJECTEDORDERS_REPL",
        "FORTS_RMT_REPL",
        "FORTS_SESSIONSTATE_REPL",
        "FORTS_INSTRUMENTSTATE_REPL",
        "FORTS_SECURITYGROUPSTATE_REPL",
        "FORTS_AGGR_REPL",
        "FORTS_ORDLOG_REPL",
        "FORTS_ORDBOOK_REPL",
    ],
    "transactional": [
        "FORTS_CLR_REPL",
        "FORTS_FEERATE_REPL",
        "FORTS_BROKER_FEE_PARAMS_REPL",
        "FORTS_BROKER_FEE_REPL",
        "FORTS_FEE_REPL",
        "FORTS_PROHIBITION_REPL",
        "FORTS_REFDATA_REPL",
        "FORTS_TRADE_REPL",
        "FORTS_MM_REPL",
        "FORTS_USERORDERBOOK_REPL",
        "FORTS_FORECASTIM_REPL",
        "FORTS_INFO_REPL",
        "FORTS_PART_REPL",
        "FORTS_POS_REPL",
        "FORTS_TNPENALTY_REPL",
        "FORTS_VM_REPL",
        "FORTS_USER_REPL",
        "FORTS_REJECTEDORDERS_REPL",
        "FORTS_RMT_REPL",
        "FORTS_SESSIONSTATE_REPL",
        "FORTS_INSTRUMENTSTATE_REPL",
        "FORTS_SECURITYGROUPSTATE_REPL",
    ],
}

MATCHING_PARTITIONED_STREAMS = {
    "FORTS_AGGR_REPL",
    "FORTS_COMMON_REPL",
    "FORTS_DEALS_REPL",
    "FORTS_FEE_REPL",
    "FORTS_FORECASTIM_REPL",
    "FORTS_ORDLOG_REPL",
    "FORTS_ORDBOOK_REPL",
    "FORTS_POS_REPL",
    "FORTS_TRADE_REPL",
    "FORTS_USERORDERBOOK_REPL",
    "FORTS_VM_REPL",
    "FORTS_VOLAT_REPL",
}

AGGR_VARIANTS = ["FORTS_AGGR5_REPL", "FORTS_AGGR20_REPL", "FORTS_AGGR50_REPL"]

MESSAGE_SURFACES = [
    {
        "protocol_item_id": "plaza2_message_cg_msg_p2mq_timeout",
        "artifact_id": "plaza_docs",
        "surface_kind": "response_code",
        "message_name": "CG_MSG_P2MQ_TIMEOUT",
        "source_reference": "heartbeat no-reply rule",
        "first_phase": "phase3c_runtime_adapter",
        "notes": "Heartbeat over p2mq must not request reply; otherwise the gateway reports CG_MSG_P2MQ_TIMEOUT.",
    },
    {
        "protocol_item_id": "plaza2_message_cg_msg_p2repl_cleardeleted",
        "artifact_id": "plaza_docs",
        "surface_kind": "replication_cleanup_notification",
        "message_name": "CG_MSG_P2REPL_CLEARDELETED",
        "source_reference": "data cleanup by streams",
        "first_phase": "phase3d_fake_engine",
        "notes": "Table-scoped cleanup by replRev threshold; MAX(int64) means full table replay.",
    },
    {
        "protocol_item_id": "plaza2_message_cg_msg_p2repl_lifenum",
        "artifact_id": "plaza_docs",
        "surface_kind": "replication_life_number_reset",
        "message_name": "CG_MSG_P2REPL_LIFENUM",
        "source_reference": "data cleanup by streams",
        "first_phase": "phase3d_fake_engine",
        "notes": "Stream-scoped reset notification; clients must clear all stream state and rebuild.",
    },
    {
        "protocol_item_id": "plaza2_signal_stream_state_online",
        "artifact_id": "plaza_docs",
        "surface_kind": "stream_state_transition",
        "message_name": "ONLINE",
        "source_reference": "late start and recovery sections",
        "first_phase": "phase3d_fake_engine",
        "notes": "ONLINE indicates snapshot catch-up is complete and subsequent data is live.",
    },
    {
        "protocol_item_id": "plaza2_signal_listener_mode_snapshot_online",
        "artifact_id": "plaza_docs",
        "surface_kind": "listener_open_mode",
        "message_name": "mode=snapshot+online",
        "source_reference": "order-book resume example",
        "first_phase": "phase3c_runtime_adapter",
        "notes": "Phase 3C listeners must support snapshot+online opens for restart-safe recovery.",
    },
    {
        "protocol_item_id": "plaza2_signal_listener_resume_lifenum_rev",
        "artifact_id": "plaza_docs",
        "surface_kind": "listener_resume_parameters",
        "message_name": "lifenum and rev.<table> resume parameters",
        "source_reference": "order-book resume example",
        "first_phase": "phase3e_private_state",
        "notes": "Persisted lifenum and per-table rev values are part of safe restart and resume.",
    },
    {
        "protocol_item_id": "plaza2_signal_stream_partition_suffix",
        "artifact_id": "plaza_docs",
        "surface_kind": "stream_name_rule",
        "message_name": "_MATCH${id}",
        "source_reference": "matching partitioning",
        "first_phase": "phase3h_public_ordlog",
        "notes": "Matching-partitioned stream names must be resolved dynamically; no single-partition assumption is allowed.",
    },
    {
        "protocol_item_id": "plaza2_signal_aggr20_default_variant",
        "artifact_id": "plaza_docs",
        "surface_kind": "stream_variant_rule",
        "message_name": "FORTS_AGGR20_REPL",
        "source_reference": "aggregated order-book streams",
        "first_phase": "phase3g_public_aggr",
        "notes": "AGGR20 is the default lightweight aggregated-book target for the approved public profile.",
    },
]


def project_root_from_script() -> Path:
    return Path(__file__).resolve().parents[1]


def stream_protocol_item_id(stream_name: str) -> str:
    return stable_id("plaza2", "stream", stream_name)


def table_protocol_item_id(stream_name: str, table_name: str) -> str:
    return stable_id("plaza2", "table", stream_name, table_name)


def stream_scope_bucket(stream_name: str) -> str:
    if stream_name in PRIVATE_CORE_STREAMS:
        return "private_core"
    if stream_name in PRIVATE_AUXILIARY_STREAMS:
        return "private_auxiliary"
    if stream_name in PUBLIC_COMMON_STREAMS:
        return "public_common"
    if stream_name in PUBLIC_AGGR_STREAMS:
        return "public_aggr"
    if stream_name in PUBLIC_ORDLOG_STREAMS:
        return "public_ordlog"
    if stream_name in DEFERRED_STREAMS:
        return "deferred"
    raise ValueError(f"unclassified stream: {stream_name}")


def table_scope_bucket(stream_name: str) -> str:
    return stream_scope_bucket(stream_name)


def build_stream_items() -> list[dict]:
    items = list(LEGACY_PROTOCOL_ITEMS)
    for stream_name, stream_type, title in STREAMS:
        item = {
            "protocol_item_id": stream_protocol_item_id(stream_name),
            "artifact_id": "plaza_docs",
            "kind": "stream",
            "stream_name": "FORTS_AGGR##_REPL" if stream_name == "FORTS_AGGR_REPL" else stream_name,
            "stream_anchor_name": stream_name,
            "stream_type": stream_type,
            "title": title,
            "scheme_filename": "forts_scheme.ini",
            "scheme_section": "FORTS_AGGR##_REPL" if stream_name == "FORTS_AGGR_REPL" else stream_name,
            "scope_bucket": stream_scope_bucket(stream_name),
            "login_subtypes": [name for name, streams in LOGIN_SUBTYPES.items() if stream_name in streams],
            "matching_partitioned": stream_name in MATCHING_PARTITIONED_STREAMS,
            "notes": "",
        }
        if stream_name == "FORTS_AGGR_REPL":
            item["stream_variants"] = AGGR_VARIANTS
            item["default_variant"] = "FORTS_AGGR20_REPL"
        items.append(item)
    return items


def build_table_items() -> list[dict]:
    items = []
    for stream_name, table_name, title in TABLES:
        items.append(
            {
                "protocol_item_id": table_protocol_item_id(stream_name, table_name),
                "artifact_id": "plaza_docs",
                "kind": "table",
                "stream_protocol_item_id": stream_protocol_item_id(stream_name),
                "stream_name": "FORTS_AGGR##_REPL" if stream_name == "FORTS_AGGR_REPL" else stream_name,
                "table_name": table_name,
                "title": title,
                "scope_bucket": table_scope_bucket(stream_name),
                "notes": "",
            }
        )
    return items


def build_message_items() -> list[dict]:
    return [dict(item) for item in MESSAGE_SURFACES]


def load_doc_manifest(project_root: Path, relative_path: str) -> dict:
    return load_json_yaml(project_root / relative_path)


def build_doc_artifacts(project_root: Path) -> list[dict]:
    artifacts = []
    for relative_manifest_path in [
        "spec-lock/prod/plaza2/docs/manifest.json",
        "spec-lock/prod/plaza2/cgate_docs/manifest.json",
    ]:
        manifest = load_doc_manifest(project_root, relative_manifest_path)
        for row in manifest["artifacts"]:
            role_info = DOC_ARTIFACT_ROLES[row["relative_path"]]
            artifacts.append(
                {
                    "artifact_id": row["artifact_id"],
                    "root_artifact_id": row["root_artifact_id"],
                    "source_manifest": relative_manifest_path,
                    "relative_path": row["relative_path"],
                    "canonical_url": row["canonical_url"],
                    "sha256": row["sha256"],
                    "size": row["size"],
                    "upstream_modified": row.get("upstream_modified", ""),
                    "display_name": role_info["display_name"],
                    "role": role_info["role"],
                    "version_marker": role_info["version_marker"],
                }
            )
    return artifacts


def build_profile_matrix() -> dict:
    private_core_ids = [stream_protocol_item_id(name) for name in sorted(PRIVATE_CORE_STREAMS)]
    private_aux_ids = [stream_protocol_item_id(name) for name in sorted(PRIVATE_AUXILIARY_STREAMS)]
    public_common_ids = [stream_protocol_item_id(name) for name in sorted(PUBLIC_COMMON_STREAMS)]
    public_aggr_ids = [stream_protocol_item_id(name) for name in sorted(PUBLIC_AGGR_STREAMS)]
    public_ordlog_ids = [stream_protocol_item_id(name) for name in sorted(PUBLIC_ORDLOG_STREAMS)]
    return {
        "test_profiles": [
            {
                "profile_id": "plaza2_private_offline",
                "mode": "offline_only",
                "required_stream_protocol_item_ids": list(private_core_ids),
                "candidate_stream_protocol_item_ids": list(private_aux_ids),
                "external_runtime_required": False,
            },
            {
                "profile_id": "plaza2_private_test",
                "mode": "external_test_gated",
                "required_stream_protocol_item_ids": list(private_core_ids),
                "candidate_stream_protocol_item_ids": list(private_aux_ids),
                "external_runtime_required": True,
                "arming_flags_planned": ["--armed-test-network", "--armed-test-session", "--armed-test-plaza2"],
            },
        ],
        "prod_profiles": [
            {
                "profile_id": "plaza2_private_prod",
                "mode": "private_state_default",
                "required_stream_protocol_item_ids": list(private_core_ids),
                "candidate_stream_protocol_item_ids": list(private_aux_ids),
                "explicit_opt_in_stream_protocol_item_ids": list(public_common_ids + public_aggr_ids + public_ordlog_ids),
            },
            {
                "profile_id": "plaza2_private_plus_aggr20",
                "mode": "private_plus_light_public",
                "inherits_profile_id": "plaza2_private_prod",
                "required_stream_protocol_item_ids": list(public_common_ids + public_aggr_ids),
                "default_public_stream_variant": "FORTS_AGGR20_REPL",
            },
            {
                "profile_id": "plaza2_private_plus_ordlog",
                "mode": "private_plus_heavy_public",
                "inherits_profile_id": "plaza2_private_prod",
                "required_stream_protocol_item_ids": list(public_common_ids + public_ordlog_ids),
                "required_table_protocol_item_ids": [
                    table_protocol_item_id("FORTS_REFDATA_REPL", "instr2matching_map"),
                ],
            },
        ],
    }


def build_runtime_expectations(environment: str) -> list[dict]:
    if environment == "prod":
        filenames = [
            ("forts_scheme.ini", "local_cgate_scheme"),
            ("links_public.prod.ini", "production_public_link_profile"),
            ("links_public.rezerv.ini", "reserve_public_link_profile"),
            ("prod.ini", "production_auth_profile"),
            ("auth_client.ini", "client_credentials_overlay"),
            ("client_router.ini", "client_router_binding"),
            ("router.ini", "local_router_config"),
        ]
    else:
        filenames = [
            ("forts_scheme.ini", "local_cgate_scheme"),
            ("links_public.t0.ini", "test_t0_public_link_profile"),
            ("links_public.t1.ini", "test_t1_public_link_profile"),
            ("links_public.game.ini", "game_public_link_profile"),
            ("links_public.custom.ini", "custom_public_link_profile"),
            ("t0.ini", "test_t0_auth_profile"),
            ("t1.ini", "test_t1_auth_profile"),
            ("game.ini", "game_auth_profile"),
            ("auth_client.ini", "client_credentials_overlay"),
            ("client_router.ini", "client_router_binding"),
            ("router.ini", "local_router_config"),
        ]

    scheme_stream_ids = [stream_protocol_item_id(name) for name in sorted(PRIVATE_CORE_STREAMS | PUBLIC_COMMON_STREAMS | PUBLIC_AGGR_STREAMS | PUBLIC_ORDLOG_STREAMS)]
    expectations = []
    for filename, role in filenames:
        entry = {
            "filename": filename,
            "role": role,
            "committed": False,
            "hash_state": "pending_local_lock",
        }
        if filename == "forts_scheme.ini":
            entry["expected_lock_manifest"] = f"spec-lock/{environment}/plaza2/scheme/manifest.json"
            entry["required_stream_protocol_item_ids"] = scheme_stream_ids
        expectations.append(entry)
    return expectations


def build_manifest(project_root: Path, environment: str) -> dict:
    profiles = build_profile_matrix()
    return {
        "version": 1,
        "phase": "phase3a_plaza2_repl_spec_lock",
        "environment": environment,
        "protocol": "plaza2_repl",
        "transport": "cgate",
        "documentation_artifacts": build_doc_artifacts(project_root),
        "scheme_and_runtime_expectations": build_runtime_expectations(environment),
        "inventory_outputs": [
            "matrix/protocol_inventory/plaza2_streams.yaml",
            "matrix/protocol_inventory/plaza2_tables.yaml",
            "matrix/protocol_inventory/plaza2_messages.yaml",
        ],
        "profiles": profiles["prod_profiles"] if environment == "prod" else profiles["test_profiles"],
        "notes": [
            "Vendor cache payloads remain gitignored; this phase locks manifest rows, filenames, hashes, and derived inventory only.",
            "Installed CGate runtime .ini files are intentionally not committed and remain pending local lock manifests.",
        ],
    }


def render_outputs(project_root: Path, output_root: Path) -> list[Path]:
    outputs = {
        output_root / "spec-lock/prod/plaza2/manifest.yaml": build_manifest(project_root, "prod"),
        output_root / "spec-lock/test/plaza2/manifest.yaml": build_manifest(project_root, "test"),
        output_root / "matrix/protocol_inventory/plaza2_streams.yaml": {
            "version": 1,
            "items": build_stream_items(),
        },
        output_root / "matrix/protocol_inventory/plaza2_tables.yaml": {
            "version": 1,
            "items": build_table_items(),
        },
        output_root / "matrix/protocol_inventory/plaza2_messages.yaml": {
            "version": 1,
            "items": build_message_items(),
        },
    }
    for path, payload in outputs.items():
        dump_yaml(payload, path)
    return sorted(outputs.keys())


def file_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def validate_outputs(project_root: Path, output_root: Path) -> None:
    stream_items = load_json_yaml(output_root / "matrix/protocol_inventory/plaza2_streams.yaml")["items"]
    table_items = load_json_yaml(output_root / "matrix/protocol_inventory/plaza2_tables.yaml")["items"]
    message_items = load_json_yaml(output_root / "matrix/protocol_inventory/plaza2_messages.yaml")["items"]
    prod_manifest = load_json_yaml(output_root / "spec-lock/prod/plaza2/manifest.yaml")
    test_manifest = load_json_yaml(output_root / "spec-lock/test/plaza2/manifest.yaml")

    stream_ids = {item["protocol_item_id"] for item in stream_items}
    table_ids = {item["protocol_item_id"] for item in table_items}
    expected_required_streams = {
        stream_protocol_item_id("FORTS_REFDATA_REPL"),
        stream_protocol_item_id("FORTS_INFO_REPL"),
        stream_protocol_item_id("FORTS_PART_REPL"),
        stream_protocol_item_id("FORTS_POS_REPL"),
        stream_protocol_item_id("FORTS_TRADE_REPL"),
        stream_protocol_item_id("FORTS_USERORDERBOOK_REPL"),
        stream_protocol_item_id("FORTS_SESSIONSTATE_REPL"),
        stream_protocol_item_id("FORTS_INSTRUMENTSTATE_REPL"),
        stream_protocol_item_id("FORTS_SECURITYGROUPSTATE_REPL"),
        stream_protocol_item_id("FORTS_AGGR_REPL"),
        stream_protocol_item_id("FORTS_ORDLOG_REPL"),
        stream_protocol_item_id("FORTS_ORDBOOK_REPL"),
    }
    missing_required_streams = sorted(expected_required_streams - stream_ids)
    if missing_required_streams:
        raise ValueError(f"missing required Phase 3A streams: {missing_required_streams}")

    expected_required_tables = {
        table_protocol_item_id("FORTS_REFDATA_REPL", "instr2matching_map"),
        table_protocol_item_id("FORTS_REFDATA_REPL", "session"),
        table_protocol_item_id("FORTS_POS_REPL", "position"),
        table_protocol_item_id("FORTS_PART_REPL", "part"),
        table_protocol_item_id("FORTS_TRADE_REPL", "orders_log"),
        table_protocol_item_id("FORTS_TRADE_REPL", "user_deal"),
        table_protocol_item_id("FORTS_USERORDERBOOK_REPL", "orders"),
        table_protocol_item_id("FORTS_AGGR_REPL", "orders_aggr"),
        table_protocol_item_id("FORTS_INSTRUMENTSTATE_REPL", "instrument_state"),
        table_protocol_item_id("FORTS_SECURITYGROUPSTATE_REPL", "security_group_state"),
    }
    missing_required_tables = sorted(expected_required_tables - table_ids)
    if missing_required_tables:
        raise ValueError(f"missing required Phase 3A tables: {missing_required_tables}")

    message_ids = {item["protocol_item_id"] for item in message_items}
    required_message_ids = {
        "plaza2_message_cg_msg_p2mq_timeout",
        "plaza2_message_cg_msg_p2repl_cleardeleted",
        "plaza2_message_cg_msg_p2repl_lifenum",
        "plaza2_signal_stream_state_online",
        "plaza2_signal_listener_mode_snapshot_online",
        "plaza2_signal_listener_resume_lifenum_rev",
        "plaza2_signal_stream_partition_suffix",
        "plaza2_signal_aggr20_default_variant",
    }
    missing_required_messages = sorted(required_message_ids - message_ids)
    if missing_required_messages:
        raise ValueError(f"missing required Phase 3A message surfaces: {missing_required_messages}")

    for manifest in [prod_manifest, test_manifest]:
        for artifact in manifest["documentation_artifacts"]:
            if len(str(artifact["sha256"])) != 64:
                raise ValueError(f"invalid sha256 in {manifest['environment']} manifest for {artifact['artifact_id']}")

    for profile in prod_manifest["profiles"] + test_manifest["profiles"]:
        for stream_id in profile.get("required_stream_protocol_item_ids", []):
            if stream_id not in stream_ids:
                raise ValueError(f"profile {profile['profile_id']} references missing stream {stream_id}")
        for stream_id in profile.get("candidate_stream_protocol_item_ids", []):
            if stream_id not in stream_ids:
                raise ValueError(f"profile {profile['profile_id']} references missing candidate stream {stream_id}")
        for stream_id in profile.get("explicit_opt_in_stream_protocol_item_ids", []):
            if stream_id not in stream_ids:
                raise ValueError(f"profile {profile['profile_id']} references missing opt-in stream {stream_id}")
        for table_id in profile.get("required_table_protocol_item_ids", []):
            if table_id not in table_ids:
                raise ValueError(f"profile {profile['profile_id']} references missing table {table_id}")

    for expectation in prod_manifest["scheme_and_runtime_expectations"] + test_manifest["scheme_and_runtime_expectations"]:
        if expectation["filename"] == "forts_scheme.ini" and "required_stream_protocol_item_ids" not in expectation:
            raise ValueError("forts_scheme.ini expectation must carry required stream coverage")


def main() -> int:
    parser = argparse.ArgumentParser(description="Materialize Phase 3A PLAZA II repl manifests and inventories.")
    parser.add_argument("--project-root", default=str(project_root_from_script()))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()
    if args.check:
        with tempfile.TemporaryDirectory(prefix="plaza2-phase3a-") as temp_dir:
            temp_root = Path(temp_dir)
            generated_paths = render_outputs(project_root, temp_root)
            validate_outputs(project_root, temp_root)
            mismatches = []
            for generated_path in generated_paths:
                relative_path = generated_path.relative_to(temp_root)
                repo_path = project_root / relative_path
                if not repo_path.exists() or file_text(generated_path) != file_text(repo_path):
                    mismatches.append(relative_path.as_posix())
            if mismatches:
                raise SystemExit(f"Phase 3A outputs are stale: {mismatches}")
        return 0

    render_outputs(project_root, project_root)
    validate_outputs(project_root, project_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
