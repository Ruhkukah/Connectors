"""Isolated q2..10 committed-deal observer; no native actions.

Adapted from the reviewed 14 October aggressive observer; ordinary q2 evidence
code remains unchanged. Journal cursor copied from repository native_evidence.
Source SHA256: 8ff426a82ca00910bcf8d37836748925639695291ac400f885b32b052d1702ba
Cursor source SHA256: 8ca198dd0fe44ce6fabc6078f5ce41d7941b92c264e879cb83678ce702a986bb
"""
import json
import os
from pathlib import Path
from .scope import HarnessStop, require, positive

TRADE = 'FORTS_TRADE_REPL'

class JournalCursor:
    def __init__(self, path, offset):
        self.path, self.offset = Path(path), offset
        stat = self.path.stat()
        self.identity = (stat.st_dev, stat.st_ino)

    def events(self):
        if self.path.stat().st_size < self.offset:
            raise HarnessStop('native_journal_truncated')
        with self.path.open('rb') as source:
            stat = os.fstat(source.fileno())
            if (stat.st_dev, stat.st_ino) != self.identity:
                raise HarnessStop('native_journal_replaced')
            source.seek(self.offset)
            while source.tell() < stat.st_size:
                line = source.readline()
                if not line or not line.endswith(b'\n'):
                    return  # Retry a buffered final record on the next update.
                try:
                    event = json.loads(line)
                except (ValueError, UnicodeError) as error:
                    raise HarnessStop('native_journal_invalid_record') from error
                if not isinstance(event, dict) or not isinstance(event.get('data', {}), dict):
                    raise HarnessStop('native_journal_invalid_record')
                self.offset = source.tell()
                yield event


def fields(data):
    return {x['name']: x.get('signed', x.get('unsigned')) for x in data.get('fields', [])}


def purge_floor(data):
    value = data.get('signed_value')
    return value if type(value) is int and 0 <= value < 9223372036854775807 else None


def positive(value):
    return type(value) is int and value > 0


class GeneralCommittedDealObserver:
    """One credited execution per committed own-deal identity, quantity <= q.

    The caller binds order IDs from a fresh owned status. A TRADE history reset
    or deletion invalidates this observation; cached partial state is never
    promoted across reconnect or a changed source epoch.
    """
    def __init__(self, path, offset, client, order_ids, isin, quantity, lifenum, side='buy'):
        require(type(quantity) is int and 1 <= quantity <= 10 and positive(lifenum) and side in {'buy', 'sell'}, 'partial_observer_bounds_invalid')
        require(order_ids and all(positive(value) for value in order_ids), 'partial_order_ids_invalid')
        self.cursor = JournalCursor(path, offset)
        self.client, self.order_ids, self.isin = client, set(order_ids), isin
        self.quantity, self.lifenum = quantity, lifenum
        self.pending, self.committed, self.credits, self.revisions = {}, {}, {}, {}
        self.credit_keys, self.begun = set(), False
        self.clear_floor = 0
        self.side = side

    def update(self):
        for event in self.cursor.events():
            name, data = event.get('event'), event.get('data', {})
            if name in ('link_lost', 'private_history_gap'):
                raise HarnessStop('partial_native_history_interrupted')
            if data.get('stream') == TRADE:
                if name == 'clear_deleted' and data.get('table') not in (None, 'user_deal'):
                    continue
                if name == 'clear_deleted' and data.get('table') == 'user_deal':
                    floor = purge_floor(data)
                    require(floor is not None, 'partial_clear_floor_unproved')
                    self.clear_floor = max(self.clear_floor, floor)
                    require(all(revision >= self.clear_floor for revision in self.revisions.values()), 'partial_deal_history_purged')
                    continue
                if name in ('listener_open', 'listener_close', 'lifenum', 'clear_deleted'):
                    raise HarnessStop('partial_native_source_epoch_changed')
                if name == 'transaction_begin':
                    require(not self.begun, 'partial_nested_transaction')
                    self.pending, self.begun = {}, True
                elif name == 'stream_row' and data.get('table') == 'user_deal' and self.begun:
                    row = fields(data)
                    ids = {row.get('public_order_id_' + self.side), row.get('private_order_id_' + self.side)}
                    if row.get('isin_id') != self.isin or not ids.intersection(self.order_ids):
                        continue
                    require(type(row.get('replAct')) is int and row['replAct'] == 0,
                            'partial_own_deal_deleted_or_action_missing')
                    deal, quantity, revision = row.get('id_deal'), row.get('xamount'), row.get('replRev')
                    require(positive(deal) and positive(quantity) and positive(revision), 'partial_native_deal_invalid')
                    if revision < self.clear_floor:
                        continue
                    previous = self.pending.get(deal, self.committed.get(deal))
                    require(previous is None or previous == quantity, 'partial_native_deal_quantity_changed')
                    self.revisions[deal], self.pending[deal] = revision, quantity
                elif name == 'transaction_commit':
                    if self.begun:
                        self.committed.update(self.pending)
                    self.pending, self.begun = {}, False
            elif name == 'trade' and data.get('client_order_id') == self.client:
                deal, quantity, order = data.get('deal_id'), data.get('quantity'), data.get('order_id')
                require(positive(deal) and positive(quantity) and order in self.order_ids, 'partial_credit_identity_invalid')
                require(deal not in self.credit_keys, 'partial_credit_not_once')
                self.credit_keys.add(deal)
                self.credits[deal] = quantity
        require(sum(self.credits.values()) <= self.quantity and sum(self.committed.values()) <= self.quantity, 'partial_accounting_exceeds_order_quantity')

    def proved(self, executed):
        return (type(executed) is int and 0 <= executed <= self.quantity and
                self.credits == self.committed and sum(self.credits.values()) == executed)

    def partial(self, executed):
        return type(executed) is int and 0 < executed < self.quantity and self.proved(executed)


    def move_proved(self, offset, old_id, new_id):
        return _native_move(self.cursor.path, offset, self.client, old_id, new_id)

def _native_move(journal, offset, client, old_id, new_id):
    require(positive(old_id) and positive(new_id) and old_id != new_id,
            'partial_move_order_alias_invalid')
    cursor, commands, replied = JournalCursor(journal, offset), set(), False
    for event in cursor.events():
        name, data = event.get('event'), event.get('data', {})
        if name == 'command' and data.get('name') == 'MoveOrder' and data.get('client_order_id') == client:
            encoded = data.get('fields', {})
            if encoded.get('order_id1') == old_id:
                require(type(encoded.get('regime')) is int and encoded['regime'] == 3,
                        'partial_move_native_regime_not_three')
                require(positive(data.get('user_id')), 'partial_move_correlation_missing')
                commands.add(data.get('user_id'))
        elif name == 'reply' and data.get('user_id') in commands and data.get('msgid') == 176 and type(data.get('code')) is int and data['code'] == 0 and data.get('order_id1') == new_id:
            replied = True
        elif replied and name == 'transaction_commit' and data.get('stream') == TRADE:
            return True
    return False
