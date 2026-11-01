"""Validate sealed fresh T0 pre-entry evidence without performing any I/O."""
import datetime as dt
import re

from t0_preparation import BINARY, DATE, SOURCE, T0_PROFILE


def validate_preentry(receipt, cfg, actual_sha256, now_ns):
    """This gate cannot grant readiness; live native admission still follows."""
    def require(value, code):
        if not value:
            raise ValueError(code)
    require(type(receipt) is dict and type(cfg) is dict, 't0_preentry_receipt_schema')
    require(type(actual_sha256) is str and re.fullmatch('[0-9a-f]{64}', actual_sha256)
            and actual_sha256 == cfg.get('prearm_flatness_receipt_sha256'),
            't0_preentry_receipt_pin_changed')
    require(receipt.get('schema') == 'MOEX_T0_PREENTRY_V1'
            and receipt.get('target_venue') == cfg.get('target_venue') == 'T0'
            and receipt.get('authorized_date') == cfg.get('authorized_date') == DATE
            and receipt.get('native_source') == cfg.get('source_git_sha') == SOURCE
            and receipt.get('binary_sha256') == cfg.get('binary_sha256') == BINARY
            and receipt.get('router_profile_sha256') == cfg.get('router_profile_sha256') == T0_PROFILE,
            't0_preentry_scope_changed')
    require(type(now_ns) is int and now_ns > 0 and type(receipt.get('checked_utc_ns')) is int
            and 0 <= now_ns - receipt['checked_utc_ns'] <= 600_000_000_000,
            't0_preentry_evidence_stale_or_future')
    require(dt.datetime.fromtimestamp(now_ns / 1e9, dt.timezone(dt.timedelta(hours=3))).date().isoformat() == DATE,
            't0_preentry_current_date_changed')
    for key in ('account_mapping_sha256', 'assignment_sha256'):
        value = receipt.get(key)
        require(type(value) is str and re.fullmatch('[0-9a-f]{64}', value)
                and value == cfg.get(key), 't0_preentry_mapping_or_assignment_changed')
    for key in ('same_account_mapping_verified', 'old_products_stopped', 'sole_T0_router_verified',
                'checkpoint_unchanged'):
        require(receipt.get(key) is True, 't0_preentry_ownership_or_account_unproved')
    for key, value in (('complete_replication_streams', 8), ('working_orders', 0),
                       ('nonzero_positions', 0), ('snapshot_exit', 0), ('snapshot_clients_after', 0),
                       ('router_client_count', 0), ('upstream_count', 4), ('publisher_posts', 0)):
        require(type(receipt.get(key)) is int and receipt[key] == value,
                't0_preentry_complete_flatness_or_exit_unproved')
    require(cfg.get('prearm_current_flatness_verified') is True
            and cfg.get('fresh_T0_preentry_verification_required') is True,
            't0_preentry_sealed_requirement_missing')
    return True
