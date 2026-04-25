#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from moex_phase0_common import dump_yaml, stable_id


FORTS_MESSAGES_SHA256 = "9ab7a54f59c9266c5d14bf8f6286cce228be1c6e7babd494909c5d3aa3f34d5b"
FORTS_SCHEME_SHA256 = "cc3ab53b792eb1354b17615612abf173158e2f9dd78604bc47a121badf54b1c2"
CGATE_DOC_SHA256 = "71ab9be53f8ae3c06e87598b284ee53fbec4eae3c41568870ab996592be163ca"
P2GATE_HTML_SHA256 = "ac1b5cb3d316049985a3ef40249f57f7aa07d3dec88fca1131bdb67a02d78f90"
SEND_SAMPLE_SHA256 = "405ccee59439ea87ae7ac4ef9c1d675b323313b93767a4fd75569955665ed22d"

SOURCE_ARTIFACTS = [
    {
        "artifact_id": "installed_cgate_docs_cgate_en_pdf",
        "artifact_id_ref": "cgate_docs",
        "relative_path": "docs/cgate_en.pdf",
        "runtime_path": "/opt/moex/cgate/docs/cgate_en.pdf",
        "sha256": CGATE_DOC_SHA256,
        "status": "locked_hash_only",
        "role": "CGate publisher and callback API reference",
    },
    {
        "artifact_id": "installed_plaza_docs_p2gate_en_html",
        "artifact_id_ref": "plaza_docs",
        "relative_path": "docs/p2gate_en.html",
        "runtime_path": "/opt/moex/cgate/docs/p2gate_en.html",
        "sha256": P2GATE_HTML_SHA256,
        "status": "locked_hash_only",
        "role": "PLAZA II order and replication reference",
    },
    {
        "artifact_id": "installed_forts_messages_ini",
        "artifact_id_ref": "cgate_docs",
        "relative_path": "scheme/latest/forts_messages.ini",
        "runtime_path": "/opt/moex/cgate/scheme/latest/forts_messages.ini",
        "sha256": FORTS_MESSAGES_SHA256,
        "status": "locked_hash_only",
        "role": "PLAZA II transactional command and reply scheme",
        "markers": {
            "spectra_release": "SPECTRA93",
            "dds_version": "93.1.6.39534",
            "target_polygon": "prod",
        },
    },
    {
        "artifact_id": "installed_forts_scheme_ini",
        "artifact_id_ref": "plaza_docs",
        "relative_path": "scheme/latest/forts_scheme.ini",
        "runtime_path": "/opt/moex/cgate/scheme/latest/forts_scheme.ini",
        "sha256": FORTS_SCHEME_SHA256,
        "status": "locked_hash_only",
        "role": "PLAZA II private replication confirmation scheme",
        "markers": {
            "spectra_release": "SPECTRA93",
        },
    },
    {
        "artifact_id": "installed_sample_send_c",
        "artifact_id_ref": "cgate_docs",
        "relative_path": "samples/c/basic/send.c",
        "runtime_path": "/opt/moex/cgate/samples/c/basic/send.c",
        "sha256": SEND_SAMPLE_SHA256,
        "status": "locked_hash_only",
        "role": "official CGate publisher sample for AddOrder and replies",
    },
]

COMMANDS = [
    {
        "command_id": "plaza2_trade_command_add_order",
        "command_name": "AddOrder",
        "msgid": 474,
        "operation_type": "add_order",
        "command_family": "plain_order",
        "reply_ids": ["plaza2_trade_reply_forts_msg179", "plaza2_trade_error_forts_msg99", "plaza2_trade_error_forts_msg100"],
        "private_confirmation_tables": [
            "FORTS_TRADE_REPL.orders_log",
            "FORTS_USERORDERBOOK_REPL.orders",
            "FORTS_REJECTEDORDERS_REPL.rejected_orders",
        ],
        "correlation_keys": ["order_id from FORTS_MSG179", "ext_id", "isin_id", "client_code"],
        "dependencies": [
            "trading_session",
            "instrument_isin_id",
            "client_code",
            "broker_code",
            "limits_for_optional_precheck",
            "price_step_and_lot_rules",
        ],
        "notes": "Official AddOrder request. Phase 5A records only; no command builder is emitted.",
    },
    {
        "command_id": "plaza2_trade_command_iceberg_add_order",
        "command_name": "IcebergAddOrder",
        "msgid": 475,
        "operation_type": "add_iceberg_order",
        "command_family": "iceberg_order",
        "reply_ids": ["plaza2_trade_reply_forts_msg180", "plaza2_trade_error_forts_msg99", "plaza2_trade_error_forts_msg100"],
        "private_confirmation_tables": [
            "FORTS_TRADE_REPL.orders_log",
            "FORTS_USERORDERBOOK_REPL.orders",
            "FORTS_REJECTEDORDERS_REPL.rejected_orders",
        ],
        "correlation_keys": ["iceberg_order_id from FORTS_MSG180", "ext_id", "isin_id", "client_code"],
        "dependencies": [
            "trading_session",
            "instrument_isin_id",
            "client_code",
            "broker_code",
            "limits_for_optional_precheck",
            "price_step_and_lot_rules",
        ],
        "notes": "Iceberg add request surface found in forts_messages.ini.",
    },
    {
        "command_id": "plaza2_trade_command_del_order",
        "command_name": "DelOrder",
        "msgid": 461,
        "operation_type": "cancel_order",
        "command_family": "plain_order",
        "reply_ids": ["plaza2_trade_reply_forts_msg177", "plaza2_trade_error_forts_msg99", "plaza2_trade_error_forts_msg100"],
        "private_confirmation_tables": ["FORTS_TRADE_REPL.orders_log", "FORTS_USERORDERBOOK_REPL.orders"],
        "correlation_keys": ["order_id", "isin_id", "client_code"],
        "dependencies": ["active_order_id", "instrument_isin_id", "client_code", "broker_code"],
        "notes": "Plain order cancellation request.",
    },
    {
        "command_id": "plaza2_trade_command_iceberg_del_order",
        "command_name": "IcebergDelOrder",
        "msgid": 464,
        "operation_type": "cancel_iceberg_order",
        "command_family": "iceberg_order",
        "reply_ids": ["plaza2_trade_reply_forts_msg182", "plaza2_trade_error_forts_msg99", "plaza2_trade_error_forts_msg100"],
        "private_confirmation_tables": ["FORTS_TRADE_REPL.orders_log", "FORTS_USERORDERBOOK_REPL.orders"],
        "correlation_keys": ["order_id", "isin_id"],
        "dependencies": ["active_order_id", "instrument_isin_id", "broker_code"],
        "notes": "Iceberg order cancellation request.",
    },
    {
        "command_id": "plaza2_trade_command_move_order",
        "command_name": "MoveOrder",
        "msgid": 476,
        "operation_type": "replace_or_move_order",
        "command_family": "plain_order",
        "reply_ids": ["plaza2_trade_reply_forts_msg176", "plaza2_trade_error_forts_msg99", "plaza2_trade_error_forts_msg100"],
        "private_confirmation_tables": ["FORTS_TRADE_REPL.orders_log", "FORTS_USERORDERBOOK_REPL.orders"],
        "correlation_keys": ["order_id1", "order_id2", "ext_id1", "ext_id2", "isin_id", "client_code"],
        "dependencies": [
            "active_order_id",
            "instrument_isin_id",
            "client_code",
            "broker_code",
            "limits_for_optional_precheck",
            "price_step_and_lot_rules",
        ],
        "notes": "MoveOrder can modify one or two orders according to the official field surface.",
    },
    {
        "command_id": "plaza2_trade_command_iceberg_move_order",
        "command_name": "IcebergMoveOrder",
        "msgid": 477,
        "operation_type": "replace_or_move_iceberg_order",
        "command_family": "iceberg_order",
        "reply_ids": ["plaza2_trade_reply_forts_msg181", "plaza2_trade_error_forts_msg99", "plaza2_trade_error_forts_msg100"],
        "private_confirmation_tables": ["FORTS_TRADE_REPL.orders_log", "FORTS_USERORDERBOOK_REPL.orders"],
        "correlation_keys": ["order_id", "ext_id", "isin_id"],
        "dependencies": [
            "active_order_id",
            "instrument_isin_id",
            "broker_code",
            "limits_for_optional_precheck",
            "price_step_and_lot_rules",
        ],
        "notes": "Iceberg move/change request surface found in forts_messages.ini.",
    },
    {
        "command_id": "plaza2_trade_command_del_user_orders",
        "command_name": "DelUserOrders",
        "msgid": 466,
        "operation_type": "mass_cancel_by_mask",
        "command_family": "mass_cancel",
        "reply_ids": ["plaza2_trade_reply_forts_msg186", "plaza2_trade_error_forts_msg99", "plaza2_trade_error_forts_msg100"],
        "private_confirmation_tables": ["FORTS_TRADE_REPL.orders_log", "FORTS_USERORDERBOOK_REPL.orders"],
        "correlation_keys": ["code", "base_contract_code", "isin_id", "ext_id"],
        "dependencies": [
            "client_code",
            "broker_code",
            "base_contract_code",
            "instrument_isin_id",
            "instrument_mask_semantics",
        ],
        "notes": "Mass delete surface; exact mask semantics are deferred to Phase 5B documentation and offline codec tests.",
    },
    {
        "command_id": "plaza2_trade_command_del_orders_by_bf_limit",
        "command_name": "DelOrdersByBFLimit",
        "msgid": 419,
        "operation_type": "mass_cancel_by_broker_firm_limit",
        "command_family": "mass_cancel",
        "reply_ids": ["plaza2_trade_reply_forts_msg172", "plaza2_trade_error_forts_msg99", "plaza2_trade_error_forts_msg100"],
        "private_confirmation_tables": ["FORTS_TRADE_REPL.orders_log", "FORTS_USERORDERBOOK_REPL.orders"],
        "correlation_keys": ["broker_code"],
        "dependencies": ["broker_code", "broker_firm_limit_state"],
        "notes": "NCC check/delete-by-BF-limit request; inventory only, not runnable.",
    },
    {
        "command_id": "plaza2_trade_command_cod_heartbeat",
        "command_name": "CODHeartbeat",
        "msgid": 10000,
        "operation_type": "cancel_on_disconnect_heartbeat",
        "command_family": "system_heartbeat",
        "reply_ids": [],
        "private_confirmation_tables": [],
        "correlation_keys": ["seq_number"],
        "dependencies": ["publisher_session_state", "cancel_on_disconnect_policy"],
        "notes": "Cancel-on-disconnect heartbeat must remain non-order-sending support surface until later phases.",
    },
]

FIELDS = {
    "AddOrder": [
        ("broker_code", "c4", True, "broker_or_firm"),
        ("isin_id", "i4", True, "instrument"),
        ("client_code", "c3", True, "client"),
        ("dir", "i4", True, "side"),
        ("type", "i4", True, "order_type"),
        ("amount", "i4", True, "quantity"),
        ("price", "c17", True, "price"),
        ("comment", "c20", False, "external_comment"),
        ("broker_to", "c20", False, "broker_to"),
        ("ext_id", "i4", False, "client_external_id"),
        ("is_check_limit", "i4", False, "precheck_flag"),
        ("date_exp", "c8", False, "time_in_force_expiry"),
        ("dont_check_money", "i4", False, "risk_check_flag"),
        ("match_ref", "c10", False, "matching_reference"),
        ("ncc_request", "i1", False, "ncc_request_flag"),
        ("compliance_id", "c1", False, "compliance"),
    ],
    "IcebergAddOrder": [
        ("broker_code", "c4", True, "broker_or_firm"),
        ("isin_id", "i4", True, "instrument"),
        ("client_code", "c3", True, "client"),
        ("dir", "i4", True, "side"),
        ("type", "i4", True, "order_type"),
        ("disclose_const_amount", "i4", True, "iceberg_disclosed_quantity"),
        ("iceberg_amount", "i4", True, "iceberg_total_quantity"),
        ("variance_amount", "i4", False, "iceberg_variance"),
        ("price", "c17", True, "price"),
        ("comment", "c20", False, "external_comment"),
        ("ext_id", "i4", False, "client_external_id"),
        ("is_check_limit", "i4", False, "precheck_flag"),
        ("date_exp", "c8", False, "time_in_force_expiry"),
        ("dont_check_money", "i4", False, "risk_check_flag"),
        ("ncc_request", "i1", False, "ncc_request_flag"),
        ("compliance_id", "c1", False, "compliance"),
    ],
    "DelOrder": [
        ("broker_code", "c4", True, "broker_or_firm"),
        ("order_id", "i8", True, "order_id"),
        ("ncc_request", "i1", False, "ncc_request_flag"),
        ("client_code", "c3", True, "client"),
        ("isin_id", "i4", True, "instrument"),
    ],
    "IcebergDelOrder": [
        ("broker_code", "c4", True, "broker_or_firm"),
        ("order_id", "i8", True, "order_id"),
        ("isin_id", "i4", True, "instrument"),
        ("ncc_request", "i1", False, "ncc_request_flag"),
    ],
    "MoveOrder": [
        ("broker_code", "c4", True, "broker_or_firm"),
        ("regime", "i4", True, "move_regime"),
        ("order_id1", "i8", True, "order_id"),
        ("amount1", "i4", True, "quantity"),
        ("price1", "c17", True, "price"),
        ("ext_id1", "i4", True, "client_external_id"),
        ("order_id2", "i8", True, "second_order_id"),
        ("amount2", "i4", True, "second_quantity"),
        ("price2", "c17", True, "second_price"),
        ("ext_id2", "i4", True, "second_client_external_id"),
        ("is_check_limit", "i4", False, "precheck_flag"),
        ("ncc_request", "i1", False, "ncc_request_flag"),
        ("client_code", "c3", True, "client"),
        ("isin_id", "i4", True, "instrument"),
        ("compliance_id", "c1", False, "compliance"),
    ],
    "IcebergMoveOrder": [
        ("broker_code", "c4", True, "broker_or_firm"),
        ("order_id", "i8", True, "order_id"),
        ("isin_id", "i4", True, "instrument"),
        ("price", "c17", True, "price"),
        ("ext_id", "i4", True, "client_external_id"),
        ("ncc_request", "i1", False, "ncc_request_flag"),
        ("is_check_limit", "i4", False, "precheck_flag"),
        ("compliance_id", "c1", False, "compliance"),
    ],
    "DelUserOrders": [
        ("broker_code", "c4", True, "broker_or_firm"),
        ("buy_sell", "i4", True, "side_mask"),
        ("non_system", "i4", True, "non_system_mask"),
        ("code", "c3", True, "client"),
        ("base_contract_code", "c25", True, "base_contract"),
        ("ext_id", "i4", False, "client_external_id"),
        ("isin_id", "i4", True, "instrument"),
        ("instrument_mask", "i1", True, "instrument_mask"),
    ],
    "DelOrdersByBFLimit": [
        ("broker_code", "c4", True, "broker_or_firm"),
    ],
    "CODHeartbeat": [
        ("seq_number", "i4", False, "heartbeat_sequence"),
    ],
}

REPLIES = [
    (
        "plaza2_trade_reply_forts_msg176",
        "FORTS_MSG176",
        176,
        "move_order_result",
        ["code", "message", "order_id1", "order_id2"],
        "MoveOrder",
    ),
    (
        "plaza2_trade_reply_forts_msg177",
        "FORTS_MSG177",
        177,
        "delete_order_result",
        ["code", "message", "amount"],
        "DelOrder",
    ),
    (
        "plaza2_trade_reply_forts_msg179",
        "FORTS_MSG179",
        179,
        "add_order_result",
        ["code", "message", "order_id"],
        "AddOrder",
    ),
    (
        "plaza2_trade_reply_forts_msg180",
        "FORTS_MSG180",
        180,
        "iceberg_add_order_result",
        ["code", "message", "iceberg_order_id"],
        "IcebergAddOrder",
    ),
    (
        "plaza2_trade_reply_forts_msg181",
        "FORTS_MSG181",
        181,
        "iceberg_move_order_result",
        ["code", "message", "order_id"],
        "IcebergMoveOrder",
    ),
    (
        "plaza2_trade_reply_forts_msg182",
        "FORTS_MSG182",
        182,
        "iceberg_delete_order_result",
        ["code", "message", "amount"],
        "IcebergDelOrder",
    ),
    (
        "plaza2_trade_reply_forts_msg186",
        "FORTS_MSG186",
        186,
        "mass_delete_result",
        ["code", "message", "num_orders"],
        "DelUserOrders",
    ),
    (
        "plaza2_trade_reply_forts_msg172",
        "FORTS_MSG172",
        172,
        "delete_by_bf_limit_result",
        ["code", "message", "num_orders"],
        "DelOrdersByBFLimit",
    ),
]

ERRORS = [
    (
        "plaza2_trade_error_forts_msg99",
        "FORTS_MSG99",
        99,
        "flood_control",
        ["queue_size", "penalty_remain", "message"],
    ),
    ("plaza2_trade_error_forts_msg100", "FORTS_MSG100", 100, "system_error", ["code", "message"]),
]


def command_items() -> list[dict]:
    items = []
    for row in COMMANDS:
        item = {
            "protocol_item_id": row["command_id"],
            "artifact_id": "cgate_docs",
            "surface_kind": "plaza2_transaction_command",
            "command_name": row["command_name"],
            "msgid": row["msgid"],
            "operation_type": row["operation_type"],
            "command_family": row["command_family"],
            "publisher_service": "FORTS_SRV",
            "publisher_category": "FORTS_MSG",
            "publisher_scheme": "message",
            "source_artifact": "scheme/latest/forts_messages.ini",
            "source_sha256": FORTS_MESSAGES_SHA256,
            "request": True,
            "safety_classification": "spec_only",
            "runnable": False,
            "first_planned_implementation_phase": "phase5b_offline_builder",
            "reply_ids": row["reply_ids"],
            "dependencies": row["dependencies"],
            "notes": row["notes"],
        }
        items.append(item)
    return items


def command_field_items() -> list[dict]:
    commands_by_name = {row["command_name"]: row for row in COMMANDS}
    items = []
    for command_name in sorted(FIELDS):
        command = commands_by_name[command_name]
        for ordinal, (field_name, type_token, required, semantic_role) in enumerate(FIELDS[command_name]):
            items.append(
                {
                    "protocol_item_id": stable_id("plaza2_trade_field", command_name, field_name),
                    "artifact_id": "cgate_docs",
                    "surface_kind": "plaza2_transaction_command_field",
                    "command_id": command["command_id"],
                    "command_name": command_name,
                    "field_name": field_name,
                    "type_token": type_token,
                    "ordinal": ordinal,
                    "required": required,
                    "semantic_role": semantic_role,
                    "source_artifact": "scheme/latest/forts_messages.ini",
                    "source_sha256": FORTS_MESSAGES_SHA256,
                    "safety_classification": "spec_only",
                }
            )
    return items


def reply_items() -> list[dict]:
    items = []
    for reply_id, name, msgid, kind, fields, command_name in REPLIES:
        items.append(
            {
                "protocol_item_id": reply_id,
                "artifact_id": "cgate_docs",
                "surface_kind": "plaza2_transaction_reply",
                "reply_name": name,
                "msgid": msgid,
                "reply_kind": kind,
                "fields": fields,
                "correlates_to_command": command_name,
                "source_artifact": "scheme/latest/forts_messages.ini",
                "source_sha256": FORTS_MESSAGES_SHA256,
                "expected_handling_phase": "phase5c_fake_session_later",
            }
        )
    return items


def error_items() -> list[dict]:
    items = []
    for error_id, name, msgid, kind, fields in ERRORS:
        items.append(
            {
                "protocol_item_id": error_id,
                "artifact_id": "cgate_docs",
                "surface_kind": "plaza2_transaction_error",
                "error_name": name,
                "msgid": msgid,
                "error_kind": kind,
                "fields": fields,
                "source_artifact": "scheme/latest/forts_messages.ini",
                "source_sha256": FORTS_MESSAGES_SHA256,
                "expected_handling_phase": "phase5c_fake_session_later",
            }
        )
    return items


def confirmation_items() -> list[dict]:
    items = []
    for command in COMMANDS:
        if not command["private_confirmation_tables"]:
            items.append(
                {
                    "protocol_item_id": stable_id("plaza2_trade_confirmation", command["command_name"]),
                    "artifact_id": "plaza_docs",
                    "surface_kind": "plaza2_transaction_confirmation_mapping",
                    "command_id": command["command_id"],
                    "command_name": command["command_name"],
                    "expected_reply_ids": command["reply_ids"],
                    "replication_tables": [],
                    "correlation_keys": command["correlation_keys"],
                    "phase3e_projects_required_tables": False,
                    "phase4a_reconciler_candidate": False,
                    "known_uncertainty": "System heartbeat has no order-state confirmation table.",
                }
            )
            continue
        items.append(
            {
                "protocol_item_id": stable_id("plaza2_trade_confirmation", command["command_name"]),
                "artifact_id": "plaza_docs",
                "surface_kind": "plaza2_transaction_confirmation_mapping",
                "command_id": command["command_id"],
                "command_name": command["command_name"],
                "expected_reply_ids": command["reply_ids"],
                "replication_tables": command["private_confirmation_tables"],
                "correlation_keys": command["correlation_keys"],
                "phase3e_projects_required_tables": True,
                "phase4a_reconciler_candidate": True,
                "known_uncertainty": "Exact rejection and order lifecycle codes remain Phase 5B/5C validation items.",
            }
        )
    return items


def matrices() -> dict[Path, dict]:
    return {
        Path("matrix/protocol_inventory/plaza2_trade_commands.yaml"): {"version": 1, "items": command_items()},
        Path("matrix/protocol_inventory/plaza2_trade_command_fields.yaml"): {
            "version": 1,
            "items": command_field_items(),
        },
        Path("matrix/protocol_inventory/plaza2_trade_replies.yaml"): {"version": 1, "items": reply_items()},
        Path("matrix/protocol_inventory/plaza2_trade_errors.yaml"): {"version": 1, "items": error_items()},
        Path("matrix/protocol_inventory/plaza2_trade_confirmation_map.yaml"): {
            "version": 1,
            "items": confirmation_items(),
        },
        Path("spec-lock/test/plaza2/trade/manifest.yaml"): {
            "version": 1,
            "environment": "test",
            "profile": "t1",
            "scope": "plaza2_transactional_trading_spec_lock",
            "raw_vendor_files_committed": False,
            "artifacts": SOURCE_ARTIFACTS,
            "safety": {
                "all_commands_spec_only": True,
                "live_order_sending": False,
                "command_builders": False,
                "write_side_abi": False,
            },
            "notes": [
                "Raw vendor docs, schemes, samples, credentials, auth files, and endpoints are intentionally not committed.",
                "This manifest records hashes and derived inventory for the installed TEST CGate package only.",
            ],
        },
    }


def materialize(project_root: Path, check: bool) -> int:
    failures = []
    for relative_path, payload in matrices().items():
        target = project_root / relative_path
        if check:
            import tempfile

            with tempfile.TemporaryDirectory(prefix="plaza2-phase5a-") as tmp_raw:
                tmp = Path(tmp_raw) / relative_path.name
                dump_yaml(payload, tmp)
                expected = tmp.read_text(encoding="utf-8")
            actual = target.read_text(encoding="utf-8") if target.exists() else ""
            if actual != expected:
                failures.append(str(relative_path))
        else:
            dump_yaml(payload, target)
    if failures:
        print("Phase 5A trade spec lock outputs are stale:", file=sys.stderr)
        for item in failures:
            print(f"  {item}", file=sys.stderr)
        return 1
    if check:
        print("Phase 5A PLAZA II trade spec lock check passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Materialize deterministic Phase 5A PLAZA II trade spec-lock matrices.")
    parser.add_argument("--project-root", default=".")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    return materialize(Path(args.project_root).resolve(), args.check)


if __name__ == "__main__":
    raise SystemExit(main())
