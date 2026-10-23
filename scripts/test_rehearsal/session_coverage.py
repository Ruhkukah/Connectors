"""Report-only evening/final-clearing evidence check; never grants order entry.

T1 evening precedes final clearing. It need not begin after that clearing.
This reader neither edits an executed verdict nor computes overall qualification.
The caller supplies source epochs verified from the same process's journal.
"""
import math

SESSION = 'FORTS_SESSIONSTATE_REPL'
TRADE = 'FORTS_TRADE_REPL'
PART = 'FORTS_PART_REPL'


def _positive(value):
    return type(value) is int and value > 0


def _range(case):
    start, end = case.get('journal_begin'), case.get('journal_end')
    return type(start) is int and start >= 0 and type(end) is int and end > start


def evaluate_session_coverage(evening, clearing, *, session_id, source_epochs=None,
                              observed_epochs=None, same_process_verified=False):
    """Check independent positive phases, not a schedule inequality.

    Inputs use the existing public scenario/clearing-public receipt shapes.
    ``same_process_verified`` is established by the existing native verifier,
    including source/session provenance. Optional existing ``source_epochs`` /
    ``observed_epochs`` maps, if supplied, must corroborate all three streams;
    this report does not manufacture or require a new epoch receipt format.
    """
    reasons = []
    evening_ok = (_range(evening) and evening.get('status') == 'PASS' and
                  {'AddDay', 'Move', 'Del'} <= set(evening.get('commands', [])) and
                  set(evening.get('symbols', [])) == {'ALRS-12.26', 'CNY-12.26'})
    if not evening_ok:
        reasons.append('evening_commands_not_proved')
    epochs_ok = (source_epochs is None and observed_epochs is None) or (
        isinstance(source_epochs, dict) and isinstance(observed_epochs, dict) and
        all(_positive(source_epochs.get(stream)) and observed_epochs.get(stream) == source_epochs[stream]
            for stream in (SESSION, TRADE, PART)))
    identity_ok = (same_process_verified is True and
                   clearing.get('same_product_observed_clearing') is True and
                   _positive(session_id) and epochs_ok)
    if not identity_ok:
        reasons.append('same_process_session_or_epochs_unproved')
    boundary = clearing.get('native_clearing_utc')
    boundary_ok = (type(boundary) in (int, float) and math.isfinite(boundary) and boundary > 0)
    def committed(row, stream, table):
        fields = row.get('native_fields', {})
        offset, commit = row.get('row_offset'), row.get('commit_offset')
        utc = row.get('committed_utc')
        return (row.get('stream') == stream and row.get('table') == table and
                type(offset) is int and type(commit) is int and
                clearing['journal_begin'] <= offset < commit <= clearing['journal_end'] and
                type(utc) in (int, float) and math.isfinite(utc) and utc >= boundary and
                fields.get('sess_id') == session_id and type(fields.get('replAct')) is int and
                fields['replAct'] == 0 and _positive(fields.get('replRev')))
    transitions, matches = [], set()
    clearing_ok = _range(clearing) and boundary_ok and clearing.get('status') == 'PASS'
    if clearing_ok:
        transitions = [r for r in clearing.get('committed_session_transitions', [])
                       if committed(r, SESSION, 'session_state') and
                       type(r['native_fields'].get('public_state')) is int and
                       r['native_fields']['public_state'] >= 0 and r['native_fields']['public_state'] != 1]
        events = {}
        for stream in (TRADE, PART):
            events[stream] = {tuple(r['native_fields'][k] for k in ('event_id', 'event_type', 'server_time'))
                for r in clearing.get('committed_system_events', [])
                if committed(r, stream, 'sys_events') and
                all(_positive(r['native_fields'].get(k)) for k in ('event_id', 'event_type', 'server_time')) and
                r['native_fields']['server_time'] >= boundary}
        matches = events[TRADE] & events[PART]
    clearing_ok = bool(clearing_ok and transitions and matches)
    if not clearing_ok:
        reasons.append('committed_final_clearing_boundary_not_proved')
    return {'status': 'PASS' if evening_ok and identity_ok and clearing_ok else 'INCOMPLETE',
            'evening_commands': 'PASS' if evening_ok else 'INCOMPLETE',
            'final_clearing': 'PASS' if clearing_ok else 'INCOMPLETE',
            'matched_system_events': sorted(matches), 'reason_codes': reasons,
            'evening_after_final_clearing_required': False,
            'overall_qualification_unchanged': True}
