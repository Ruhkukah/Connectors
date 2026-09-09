#!/usr/bin/env python3
"""Read-only binary trace verification, descriptor-driven derivation and C2 oracle import."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

HEADER = struct.Struct('<IIQQQQIIQqqQII')
LIMIT = 64 * 1024 * 1024
NAMES = ['kind', 'listener', 'ordinal', 'poll', 'monotonic_ns', 'epoch', 'native_type', 'message_id',
         'index', 'revision', 'owner_id', 'user_id', 'null_count', 'payload_length']


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def frames(path):
    """Streaming strict framing and footer validation; never modifies source."""
    prefix = hashlib.sha256()
    with path.open('rb') as f:
        magic = f.read(8)
        if magic != b'P2CAP001':
            raise ValueError('unknown format/endian version')
        prefix.update(magic)
        finished = False
        count = 0
        while length_bytes := f.read(4):
            offset = f.tell() - 4
            if finished or len(length_bytes) != 4:
                raise ValueError('trailing or incomplete frame')
            length, = struct.unpack('<I', length_bytes)
            if not HEADER.size <= length <= LIMIT + HEADER.size:
                raise ValueError('invalid frame length')
            data = f.read(length)
            if len(data) != length:
                raise ValueError('truncated frame')
            row = dict(zip(NAMES, HEADER.unpack(data[:HEADER.size])))
            if HEADER.size + row['payload_length'] + row['null_count'] != length:
                raise ValueError('inconsistent payload/null lengths')
            row['offset'] = offset
            row['payload'] = data[HEADER.size:HEADER.size + row['payload_length']]
            row['nulls'] = data[HEADER.size + row['payload_length']:]
            if row['kind'] == 5:
                footer = json.loads(row['payload'])
                if footer['prefix_sha256'] != prefix.hexdigest() or footer['metrics']['records_captured'] != count:
                    raise ValueError('footer checksum/count mismatch')
                if footer['metrics']['status'] != 'CAPTURE_COMPLETE':
                    raise ValueError('invalid capture footer')
                finished = True
            else:
                prefix.update(length_bytes + data)
            count += 1
            yield row
        if not finished:
            raise ValueError('missing completed footer: CAPTURE_INVALID')


def decode(table, payload, nulls):
    if len(payload) != table['size'] or (nulls and len(nulls) != len(table['fields'])):
        raise ValueError('descriptor/record mismatch')
    fields = {}
    for index, field in enumerate(table['fields']):
        start, size, typ = field['offset'], field['size'], field['type']
        if start < 0 or size < 0 or start + size > len(payload):
            raise ValueError('invalid descriptor field range')
        raw = payload[start:start + size]
        if nulls and nulls[index]:
            fields[field['name']] = None
        elif typ in ('i1', 'i2', 'i4', 'i8', 'u1', 'u2', 'u4', 'u8') and size == int(typ[1:]):
            fields[field['name']] = int.from_bytes(raw, 'little', signed=typ.startswith('i'))
        elif typ == 'd16.5':
            if len(raw) != 11 or raw[:2] != bytes([5, 16]):
                raise ValueError('invalid qualified decimal')
            n = raw[2] & 127
            if n > 9:
                raise ValueError('invalid decimal digit')
            for digit in raw[3:]:
                digit = 0 if digit == 128 else digit
                if digit > 99:
                    raise ValueError('invalid decimal digit')
                n = n * 100 + digit
            if n % 10:
                raise ValueError('invalid decimal fractional digit')
            fields[field['name']] = (-1 if raw[2] & 128 else 1) * (n // 10)
        else:
            fields[field['name']] = {'raw_hex': raw.hex(), 'type': typ}
    return fields


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def derive(trace: Path, destination: Path, oracle: Path | None = None):
    digest = sha(trace)
    if trace.with_name(trace.name + '.sha256').read_text().strip() != digest:
        raise ValueError('trace SHA-256 mismatch')
    # Validate complete source before emitting fixtures from potentially truncated input.
    for _ in frames(trace):
        pass
    destination.mkdir(exist_ok=False)
    mappings, descriptors, ordinals, sequences, frontiers = {}, [], {}, {}, {}
    outcomes, observations, metadata, metrics = {}, {}, {}, {}
    callbacks = 0
    fixture = destination / 'replay.tsv'
    with fixture.open('w') as replay, (destination / 'controls.jsonl').open('w') as controls, \
            (destination / 'records.jsonl').open('w') as records:
        replay.write(f'# source_sha256={digest}\n')
        for record in frames(trace):
            kind, lid, epoch = record['kind'], record['listener'], record['epoch']
            pair = (lid, epoch)
            if kind == 1:
                metadata = json.loads(record['payload'])
                if metadata['format'] != 1 or metadata['endianness'] != 'little':
                    raise ValueError('unsupported capture environment')
            elif kind == 2:
                descriptor = json.loads(record['payload'])
                descriptor.update(listener=lid, epoch=epoch, source_offset=record['offset'],
                                  descriptor_sha256=hashlib.sha256(record['payload']).hexdigest())
                if pair in mappings:
                    raise ValueError('duplicate descriptor epoch')
                indices = {t['index']: t for t in descriptor['tables']}
                if len(indices) != len(descriptor['tables']):
                    raise ValueError('duplicate composite index')
                mappings[pair] = indices
                descriptors.append(descriptor)
                outcomes[str(pair)] = descriptor['outcome']
            elif kind in (4, 6):
                detail = json.loads(record['payload'])
                controls.write(json.dumps({**{k: v for k, v in record.items() if k not in ('payload', 'nulls')}, **detail}) + '\n')
                if detail.get('operation') == 'listener_close' and detail.get('code') == 0:
                    replay.write(f'X {lid} {epoch} 0\n')
                if detail.get('listener_state') == 1:
                    replay.write(f'E {lid} {epoch} 0\n')
                if 'runtime_version' in detail:
                    metadata['runtime_version'] = detail['runtime_version']
                    metadata['runtime_version_code'] = detail['runtime_version_code']
                if 'outcome' in detail:
                    outcomes[str(pair)] = detail['outcome']
            elif kind == 5:
                metrics = json.loads(record['payload'])['metrics']
            elif kind == 3:
                callbacks += 1
                ordinal = record['ordinal']
                if ordinal != ordinals.get(lid, 0) + 1:
                    raise ValueError('callback ordinal discontinuity')
                ordinals[lid] = ordinal
                row = {k: v for k, v in record.items() if k not in ('payload', 'nulls')}
                row['payload_sha256'] = hashlib.sha256(record['payload']).hexdigest()
                typ = record['native_type']
                event = {0x100: 'O', 0x101: 'X', 0x200: 'B', 0x210: 'C', 0x1112: 'N',
                         0x1110: 'L', 0x1111: 'D', 0x1115: 'T'}.get(typ, 'U')
                args = []
                observations.setdefault(str(pair), {'online_ordinal': None, 'info': [], 'lifenum': [],
                                                    'first_continuation': None, 'data_records': 0, 'log_records': 0})
                observed = observations[str(pair)]
                if typ == 0x120:
                    observed['data_records'] += 1
                    table = mappings.get(pair, {}).get(record['index'])
                    if table is None:
                        row['decode_status'] = 'UNMAPPED_INDEX'
                    else:
                        row['table'] = table['name']
                        try:
                            values = decode(table, record['payload'], record['nulls'])
                            row['fields'] = values
                            name = table['name']
                            if name == 'info':
                                args = [values[n] for n in ('infoID', 'trades_rev', 'trades_lifenum', 'publication_state')]
                                event = 'I'
                                observed['info'].append({'ordinal': ordinal, **dict(zip(
                                    ('infoID', 'trades_rev', 'trades_lifenum', 'publication_state'), args))})
                            elif name in ('orders', 'multileg_orders', 'orders_log', 'multileg_orders_log'):
                                multi = name.startswith('multileg_')
                                args = [int(multi)] + [values[n] for n in ('public_order_id', 'sess_id', 'isin_id', 'dir',
                                        'swap_price' if multi else 'price', 'public_amount_rest', 'xstatus', 'xstatus2')]
                                event = 'R' if name.endswith('_log') else 'S'
                                if event == 'R':
                                    observed['log_records'] += 1
                                    fingerprint = 14695981039346656037
                                    for field in table['fields']:
                                        for byte in record['payload'][field['offset']:field['offset'] + field['size']]:
                                            fingerprint = ((fingerprint ^ byte) * 1099511628211) & ((1 << 64) - 1)
                                    args += [values['replRev'], values['public_amount'], values['public_action'], fingerprint]
                                    if observed['online_ordinal'] and observed['first_continuation'] is None:
                                        observed['first_continuation'] = {'ordinal': ordinal, 'revision': values['replRev']}
                                if values.get('replAct') != 0 or (event == 'R' and values['replRev'] != record['revision']):
                                    event, args = 'U', []
                            if any(not isinstance(x, int) for x in args):
                                event, args = 'U', []
                                row['decode_status'] = 'REQUIRED_FIELD_UNAVAILABLE'
                        except (KeyError, ValueError, TypeError) as exc:
                            row['decode_status'] = str(exc)
                            event, args = 'U', []
                else:
                    row['raw_hex'] = record['payload'].hex()
                    row['nulls_hex'] = record['nulls'].hex()
                    if typ == 0x1110 and len(record['payload']) == 8:
                        args = [struct.unpack_from('<I', record['payload'])[0]]
                        observed['lifenum'].append({'ordinal': ordinal, 'life': args[0]})
                    elif typ == 0x1110:
                        event = 'U'
                    if typ == 0x1112:
                        observed['online_ordinal'] = ordinal
                    controls.write(json.dumps(row) + '\n')
                if typ in (0x120, 0x200, 0x210, 0x1112):
                    sample = sequences.setdefault(str(pair), [])
                    if len(sample) < 128:
                        sample.append({'ordinal': ordinal, 'type': typ, 'table': row.get('table')})
                records.write(json.dumps(row) + '\n')
                replay.write(' '.join(map(str, [event, lid, epoch, ordinal] + args)) + '\n')
        if callbacks != metrics['callbacks_received']:
            raise ValueError('callback/footer accounting mismatch')
    for key, observed in observations.items():
        observed['passive_activity'] = 'OBSERVED' if observed['log_records'] else 'NOT_OBSERVED'
        info = observed['info'][-1] if observed['info'] else None
        continuation = observed['first_continuation']
        frontiers[key] = {'bound': info, 'first_continuation': continuation,
                          'relation': ('AFTER_BOUND' if continuation['revision'] > info['trades_rev'] else 'AT_OR_BEFORE_BOUND')
                          if info and continuation else 'NOT_OBSERVED',
                          'lifenum_controls': observed['lifenum'], 'same_life_observed':
                          (info['trades_lifenum'] == observed['lifenum'][-1]['life']) if info and observed['lifenum'] else None}
    analysis = {'status': 'OBSERVATIONS_ONLY', 'production_c2': 'BLOCKED', 'identity': 'BLOCKED',
                'chronology_sample_limit': 128, 'full_chronology': 'records.jsonl',
                'source_sha256': digest, 'outcomes': outcomes, 'observations': observations, 'chronology': sequences,
                'bound_observations': frontiers, 'equivalence': 'NOT_OBSERVED', 'cross_pair_atomicity': 'NOT_CLAIMED'}
    if oracle:
        result = subprocess.run([str(oracle.resolve()), '--capture-fixture', str(fixture.resolve())],
                                capture_output=True, text=True, check=True)
        analysis['oracle'] = json.loads(result.stdout)
        analysis['equivalence'] = analysis['oracle']['equivalence']
    write_json(destination / 'descriptors.json', descriptors)
    write_json(destination / 'environment.json', metadata)
    write_json(destination / 'metrics.json', metrics)
    write_json(destination / 'analysis.json', analysis)
    if sha(trace) != digest:
        raise ValueError('source changed during import')
    write_json(destination / 'import_manifest.json', {'source_sha256': digest, 'source_immutable': True,
               'format_version': 1, 'importer_sha256': sha(Path(__file__)), 'fixture_sha256': sha(fixture),
               'oracle_sha256': sha(oracle) if oracle else None})
    if sha(trace) != digest:
        raise ValueError('source changed during import')
    return analysis


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--oracle', type=Path)
    args = parser.parse_args()
    derive(args.trace, args.output, args.oracle)


if __name__ == '__main__':
    main()
