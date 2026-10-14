"""Committed native session/instrument authority; no commands or processes.

This observer is an additional provenance check. Callers must retain the
host's complete reconstruction, REFDATA membership, account and quote gates.
"""
import copy

from native_evidence import EvidenceError, JournalCursor, positive, purge_floor

SESSION = 'FORTS_SESSIONSTATE_REPL'
INSTRUMENT = 'FORTS_INSTRUMENTSTATE_REPL'
TABLES = {SESSION: {'session_state'}, INSTRUMENT: {'instrument_state', 'sys_events'}}
MAX_REVISION = 9223372036854775807


def integer(row, name, default=None):
    field = row.get(name)
    if field is None:
        return default
    if field.get('kind') != 1 or type(field.get('signed')) is not int:
        raise EvidenceError('admission_native_integer_kind_unproved')
    return field['signed']


class CommittedAdmissionObserver:
    def __init__(self, path, offset=0):
        self.cursor = JournalCursor(path, offset)
        self.pending = {}
        self.sessions, self.instruments, self.markers = {}, {}, {}
        self.identities = {'session_state': {}, 'instrument_state': {}}
        self.marker = None
        self.marker_revision = 0
        self.status_epoch = 1
        self.failed = False
        self.streams = {name: dict(opened=False, online=False, lifenum=None, generation=0) for name in TABLES}
        self.floors = {name: {table: 0 for table in tables} for name, tables in TABLES.items()}

    def reset(self, stream, *, lifecycle=False):
        self.pending.pop(stream, None)
        if stream == SESSION:
            self.sessions.clear()
            self.identities['session_state'].clear()
        else:
            self.instruments.clear()
            self.identities['instrument_state'].clear()
            self.markers.clear()
            self.marker = None
            self.marker_revision = 0
            self.status_epoch += 1
        if lifecycle:
            self.streams[stream]['online'] = False
            self.streams[stream]['generation'] += 1
            self.floors[stream] = {table: 0 for table in TABLES[stream]}

    def marker_reset(self, *, new_revision_epoch=False):
        self.marker = None
        self.status_epoch += 1
        if new_revision_epoch:
            self.marker_revision = 0

    def apply_row(self, stream, data, row_offset, commit_offset):
        table = data['table']
        fields = {x['name']: x for x in data.get('fields', [])}
        repl_id, revision, deleted = integer(fields, 'replID'), integer(fields, 'replRev'), integer(fields, 'replAct')
        native_revision = data.get('signed_value')
        if native_revision is not None:
            if not positive(native_revision) or (revision is not None and revision != native_revision):
                raise EvidenceError('admission_native_revision_mismatch')
            revision = native_revision
        if not positive(repl_id) or not positive(revision) or type(deleted) is not int:
            raise EvidenceError('admission_native_service_identity_unproved')
        if revision < self.floors[stream][table]:
            return
        proof = dict(repl_id=repl_id, repl_rev=revision, row_offset=row_offset, commit_offset=commit_offset,
                     lifenum=self.streams[stream]['lifenum'], generation=self.streams[stream]['generation'])
        if table == 'sys_events':
            # This matches the projector's stream-wide marker highwater, even
            # for tombstones. Historical markers cannot relabel current rows.
            if revision <= self.marker_revision:
                return
            if deleted:
                known = self.markers.pop(repl_id, None)
                if known and self.marker and self.marker['repl_id'] == repl_id:
                    self.marker_revision = revision
                    self.marker_reset()
                return  # Unknown/nonlatest sparse tombstones are no-ops.
            sess_id = integer(fields, 'sess_id')
            if not positive(sess_id):
                raise EvidenceError('admission_native_marker_session_unproved')
            old_session = self.marker['sess_id'] if self.marker else None
            if old_session is not None and old_session != sess_id and self.streams[stream]['online']:
                self.status_epoch += 1
            proof.update(sess_id=sess_id, status_epoch=self.status_epoch)
            self.markers[repl_id] = proof
            self.marker, self.marker_revision = proof, revision
            return
        target = self.sessions if table == 'session_state' else self.instruments
        identities = self.identities[table]
        if deleted:
            key = identities.get(repl_id)
            current = target.get(key)
            if current and current['repl_id'] == repl_id and revision > current['repl_rev']:
                target.pop(key)
                identities.pop(repl_id)
            return
        key = integer(fields, 'sess_id' if table == 'session_state' else 'isin_id')
        state = integer(fields, 'public_state')
        if not positive(key) or type(state) is not int or state < 0:
            raise EvidenceError('admission_native_status_identity_unproved')
        previous_key = identities.get(repl_id)
        previous = target.get(previous_key)
        if previous and previous_key != key:
            if revision <= previous['repl_rev']:
                return
            target.pop(previous_key)
            identities.pop(repl_id)
        current = target.get(key)
        if current and revision <= current['repl_rev']:
            return
        if current:
            identities.pop(current['repl_id'], None)
        proof.update(public_state=state)
        if table == 'instrument_state':
            proof['status_epoch'] = self.status_epoch
        target[key], identities[repl_id] = proof, key

    def apply_purge(self, stream, data):
        table = data.get('table')
        if table and table not in TABLES[stream]:
            return
        floor = purge_floor(data)
        if not table or floor is None:
            if table == 'sys_events':
                self.markers.clear()
                self.marker_reset(new_revision_epoch=data.get('signed_value') == MAX_REVISION)
                if data.get('signed_value') == MAX_REVISION:
                    self.floors[stream][table] = 0
            elif table in ('session_state', 'instrument_state'):
                target = self.sessions if table == 'session_state' else self.instruments
                target.clear()
                self.identities[table].clear()
                if data.get('signed_value') == MAX_REVISION:
                    self.floors[stream][table] = 0
            else:
                highwater = self.marker_revision
                self.reset(stream)
                if stream == INSTRUMENT and data.get('signed_value') != MAX_REVISION:
                    self.marker_revision = highwater
                if data.get('signed_value') == MAX_REVISION:
                    self.floors[stream] = {name: 0 for name in TABLES[stream]}
            return
        self.floors[stream][table] = max(self.floors[stream][table], floor)
        floor = self.floors[stream][table]
        if table == 'sys_events':
            retired_latest = self.marker is not None and self.marker['repl_rev'] < floor
            self.markers = {key: row for key, row in self.markers.items() if row['repl_rev'] >= floor}
            if retired_latest:
                self.marker_reset()  # Retain highwater; ordinary purges do not revive history.
        else:
            target = self.sessions if table == 'session_state' else self.instruments
            for key, proof in list(target.items()):
                if proof['repl_rev'] < floor:
                    target.pop(key)
                    self.identities[table].pop(proof['repl_id'], None)

    def update(self):
        try:
            self._update()
        except Exception:
            self.failed = True
            raise

    def _update(self):
        before = self.cursor.offset
        for event in self.cursor.events():
            after = self.cursor.offset
            name, data = event.get('event'), event.get('data', {})
            stream = data.get('stream')
            if name in ('link_lost', 'private_history_gap'):
                for watched in TABLES:
                    self.reset(watched, lifecycle=True)
                    self.streams[watched]['opened'] = False
            if stream not in TABLES:
                before = after
                continue
            if name in ('listener_open', 'listener_close', 'lifenum'):
                self.reset(stream, lifecycle=True)
                if name == 'lifenum':
                    value = data.get('value')
                    if not positive(value):
                        raise EvidenceError('admission_native_lifenum_notice_invalid')
                    self.streams[stream]['lifenum'] = value
                else:
                    self.streams[stream].update(opened=name == 'listener_open', lifenum=None)
            elif name == 'listener_online':
                self.streams[stream]['online'] = True
            elif name == 'transaction_begin':
                if stream in self.pending:
                    raise EvidenceError('admission_nested_native_transaction')
                self.pending[stream] = []
            elif name == 'stream_row' and data.get('table') in TABLES[stream]:
                if stream not in self.pending:
                    raise EvidenceError('admission_row_without_native_transaction')
                self.pending[stream].append(('row', copy.deepcopy(data), before))
            elif name == 'clear_deleted':
                operation = ('purge', copy.deepcopy(data), before)
                if stream in self.pending:
                    self.pending[stream].append(operation)
                else:
                    self.apply_purge(stream, data)
            elif name == 'transaction_commit':
                if stream not in self.pending:
                    raise EvidenceError('admission_commit_without_native_transaction')
                for kind, values, offset in self.pending.pop(stream, []):
                    if kind == 'row':
                        self.apply_row(stream, values, offset, after)
                    else:
                        self.apply_purge(stream, values)
            before = after

    def native_open(self, sess_id, isin_ids):
        ids = set(isin_ids)
        current = self.sessions.get(sess_id)
        return (positive(sess_id) and bool(ids) and all(positive(isin) for isin in ids)
                and not self.failed and not self.pending
                and all(x['opened'] and x['online'] for x in self.streams.values())
                and current is not None and current['public_state'] == 1
                and current['generation'] == self.streams[SESSION]['generation']
                and current['lifenum'] == self.streams[SESSION]['lifenum']
                and self.marker is not None and self.marker['sess_id'] == sess_id
                and self.marker['generation'] == self.streams[INSTRUMENT]['generation']
                and self.marker['lifenum'] == self.streams[INSTRUMENT]['lifenum']
                and all((row := self.instruments.get(isin)) is not None and row['public_state'] == 1
                        and row['status_epoch'] == self.status_epoch
                        and row['generation'] == self.streams[INSTRUMENT]['generation']
                        and row['lifenum'] == self.streams[INSTRUMENT]['lifenum'] for isin in ids))

    def proof(self):
        return copy.deepcopy(dict(journal_offset=self.cursor.offset, failed=self.failed, streams=self.streams,
                                  session_states=self.sessions, instrument_marker=self.marker,
                                  marker_highwater=self.marker_revision, status_epoch=self.status_epoch,
                                  table_floors=self.floors, instrument_states=self.instruments))
