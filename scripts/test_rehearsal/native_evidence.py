"""Incremental, read-only proofs from complete native journal records."""
import json
import os
from pathlib import Path

TRADE = 'FORTS_TRADE_REPL'
RESETS = {'listener_open', 'listener_close', 'lifenum', 'clear_deleted'}


class EvidenceError(RuntimeError):
    pass


class JournalCursor:
    def __init__(self, path, offset):
        self.path, self.offset = Path(path), offset
        stat = self.path.stat()
        self.identity = (stat.st_dev, stat.st_ino)

    def events(self):
        if self.path.stat().st_size < self.offset:
            raise EvidenceError('native_journal_truncated')
        with self.path.open('rb') as source:
            stat = os.fstat(source.fileno())
            if (stat.st_dev, stat.st_ino) != self.identity:
                raise EvidenceError('native_journal_replaced')
            source.seek(self.offset)
            while source.tell() < stat.st_size:
                line = source.readline()
                if not line or not line.endswith(b'\n'):
                    return  # Retry a buffered final record on the next update.
                try:
                    event = json.loads(line)
                except (ValueError, UnicodeError) as error:
                    raise EvidenceError('native_journal_invalid_record') from error
                if not isinstance(event, dict) or not isinstance(event.get('data', {}), dict):
                    raise EvidenceError('native_journal_invalid_record')
                self.offset = source.tell()
                yield event


def fields(data):
    return {x['name']: x.get('signed', x.get('unsigned')) for x in data.get('fields', [])}


def purge_floor(data):
    value = data.get('signed_value')
    return value if type(value) is int and 0 <= value < 9223372036854775807 else None


def positive(value):
    return type(value) is int and value > 0


class CommittedDealObserver:
    def __init__(self, path, offset, client, order_ids, isin):
        self.cursor = JournalCursor(path, offset)
        self.client, self.order_ids, self.isin = client, set(order_ids), isin
        self.pending, self.committed, self.credits = {}, {}, {}
        self.credit_keys = set()
        self.revisions = {}
        self.clear_floor = 0
        self.begun = False

    def update(self):
        for event in self.cursor.events():
            name, data = event.get('event'), event.get('data', {})
            if data.get('stream') == TRADE:
                if name == 'clear_deleted' and data.get('table'):
                    if data['table'] != 'user_deal':
                        continue
                    floor = purge_floor(data)
                    if floor is not None:
                        self.clear_floor = max(self.clear_floor, floor)
                        for deal, revision in list(self.revisions.items()):
                            if revision < self.clear_floor:
                                self.pending.pop(deal, None)
                                self.committed.pop(deal, None)
                                self.credits.pop(deal, None)
                                self.revisions.pop(deal, None)
                        continue
                if name in RESETS:
                    self.revisions.clear()
                    self.clear_floor = 0
                    self.pending.clear()
                    self.committed.clear()
                    self.credits.clear()
                    self.begun = False
                elif name == 'transaction_begin':
                    self.pending.clear()
                    self.begun = True
                elif name == 'stream_row' and data.get('table') == 'user_deal' and self.begun:
                    row = fields(data)
                    ids = {row.get('public_order_id_buy'), row.get('private_order_id_buy')}
                    if row.get('isin_id') != self.isin or not ids.intersection(self.order_ids):
                        continue
                    if row.get('replAct', 0) != 0:
                        # Deletion/rewrites cannot retain prior positive proof.
                        self.committed.pop(row.get('id_deal'), None)
                        self.pending.pop(row.get('id_deal'), None)
                        continue
                    deal, quantity, revision = row.get('id_deal'), row.get('xamount'), row.get('replRev')
                    if not positive(deal) or not positive(quantity) or not positive(revision):
                        raise EvidenceError('native_deal_quantity_or_identity_invalid')
                    if revision < self.clear_floor:
                        continue
                    self.revisions[deal] = revision
                    previous = self.pending.get(deal, self.committed.get(deal))
                    if previous is not None and previous != quantity:
                        raise EvidenceError('native_deal_quantity_changed')
                    self.pending[deal] = quantity
                elif name == 'transaction_commit':
                    if self.begun:
                        self.committed.update(self.pending)
                    self.pending.clear()
                    self.begun = False
            elif name == 'trade' and data.get('client_order_id') == self.client:
                deal, quantity, order = data.get('deal_id'), data.get('quantity'), data.get('order_id')
                if not positive(deal) or not positive(quantity) or order not in self.order_ids:
                    raise EvidenceError('credited_trade_identity_invalid')
                if deal in self.credit_keys:
                    raise EvidenceError('fill_credit_not_once')
                self.credit_keys.add(deal)
                self.credits[deal] = quantity
        if sum(self.credits.values()) > 2 or sum(self.committed.values()) > 2:
            raise EvidenceError('fill_accounting_exceeds_execution')

    def proved(self, executed):
        return (positive(executed) and sum(self.credits.values()) == executed
                and sum(self.committed.values()) == executed
                and self.credits == self.committed)


class CompactReplayVerifier:
    def __init__(self, path, offset):
        self.cursor = JournalCursor(path, offset)
        self.opened = self.online = self.begun = False
        self.pending, self.first = {}, {}
        self.lifenum = None
        self.clear_floors = {}

    def update(self):
        for event in self.cursor.events():
            name, data = event.get('event'), event.get('data', {})
            if data.get('stream') != TRADE:
                continue
            if name == 'clear_deleted' and data.get('table'):
                table, floor = data['table'], purge_floor(data)
                if table not in ('user_deal', 'heartbeat'):
                    continue
                if floor is not None:
                    self.clear_floors[table] = max(self.clear_floors.get(table, 0), floor)
                    for values in (self.pending, self.first):
                        if values.get(table, floor) < floor:
                            values.pop(table, None)
                    continue
            if name in RESETS:
                self.clear_floors.clear()
                self.pending.clear()
                self.first.clear()
                self.online = self.begun = False
                if name == 'listener_open':
                    self.opened = True
                    self.lifenum = None
                elif name == 'listener_close':
                    self.opened = False
                elif name == 'lifenum':
                    self.lifenum = data.get('value')
            elif self.opened:
                if name == 'transaction_begin':
                    self.pending.clear()
                    self.begun = True
                elif name == 'listener_online':
                    self.online = True
                elif self.begun and name == 'trade_server_time' and data.get('reason') == 'minute':
                    self.pending.setdefault('heartbeat', data.get('repl_rev'))
                elif self.begun and name == 'stream_row' and data.get('table') in ('heartbeat', 'user_deal'):
                    row = fields(data)
                    if row.get('replAct', 0) == 0:
                        self.pending.setdefault(data['table'], row.get('replRev'))
                elif name == 'transaction_commit':
                    if self.begun:
                        for table, revision in self.pending.items():
                            if not positive(revision):
                                raise EvidenceError('replay_revision_missing')
                            if revision >= self.clear_floors.get(table, 0):
                                self.first.setdefault(table, revision)
                    self.pending.clear()
                    self.begun = False

    def result(self, state, saved_anchor):
        replay, pos = state.get('trade_replay') or {}, state.get('pos_anchor') or {}
        saved_anchor = saved_anchor or {}
        anchor = replay.get('used_anchor') or {}
        listeners = state.get('transport', {}).get('listeners', [])
        required = {'FORTS_TRADE_REPL', 'FORTS_USERORDERBOOK_REPL', 'FORTS_POS_REPL',
                    'FORTS_PART_REPL', 'FORTS_REFDATA_REPL', 'FORTS_SESSIONSTATE_REPL',
                    'FORTS_INSTRUMENTSTATE_REPL', 'FORTS_AGGR20_REPL', 'p2mqreply'}
        epoch = anchor.get('trades_lifenum')
        floor = anchor.get('trades_rev')
        ready = (self.opened and self.online and positive(epoch) and positive(floor)
                 and replay.get('anchor_ready') is True
                 and replay.get('accepted_lifenum') == epoch == pos.get('trades_lifenum')
                 and saved_anchor.get('trades_lifenum') == epoch
                 and positive(saved_anchor.get('trades_rev')) and floor >= saved_anchor['trades_rev']
                 and pos.get('trades_rev') == floor and pos.get('online') is True
                 and pos.get('snapshot_complete') is True
                 and state.get('reconstructing') is False and state.get('queued') == 0
                 and not any(state.get(k) for k in ('journal_failed', 'log_error', 'cancel_only',
                                                   'operator_action_required', 'cgate_key_check_failed'))
                 and state.get('transport', {}).get('valid') is True
                 and state.get('transport', {}).get('connection', {}).get('state') == 3
                 and len(listeners) == len(required) and {x.get('stream') for x in listeners} == required
                 and all(x.get('online') is True and x.get('snapshot_complete') is True
                         and x.get('native', {}).get('state') == 3 and x.get('error', {}).get('code') == 0
                         for x in listeners)
                 and (self.lifenum is None or self.lifenum == epoch))
        if ready and any(revision < floor for revision in self.first.values()):
            raise EvidenceError('restart_replay_below_pos_anchor')
        return {'proven': bool(ready and {'heartbeat', 'user_deal'} <= self.first.keys()),
                'first_replay': dict(self.first)}
