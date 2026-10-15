"""Oct16 TEST scope over an immutable, previously reviewed native runtime.

The deployed package supplies frozen_runtime.py; imports do not start anything.
Native provenance and review/CI provenance deliberately have separate pins.
"""
from pathlib import Path
import http.client
import hashlib
import ssl
import json
import re
import time
import urllib.request
import urllib.error

import short_scope as scope

import frozen_runtime as base

AUTHORIZED_DATE = '2026-10-16'
base.AUTHORIZED_DATE = AUTHORIZED_DATE
HEAD, BINARY, BOOT = base.HEAD, base.BINARY, base.BOOT
CI_RUN_ID = base.CI_RUN_ID
REVIEW_ONLY = False
JOBS = {'connector-validation', 'component-sanitizers'}


def __getattr__(name):
    return getattr(base, name)


def fetch(suffix):
    request = urllib.request.Request(
        'https://api.github.com/repos/Ruhkukah/Connectors/' + suffix,
        headers={'User-Agent': 'MOEX-TEST-qualification'})
    with urllib.request.urlopen(request, timeout=20) as result:
        return json.load(result)


def successful_run(run, pin, sha):
    return (type(pin) is int and run.get('id') == pin and run.get('head_sha') == sha
            and run.get('status') == 'completed' and run.get('conclusion') == 'success')


def _verify_ci_once(root, cfg):
    require = base.require
    review = cfg.get('review_git_sha', '')
    review_ci = cfg.get('review_ci_run_id')
    require(re.fullmatch('[0-9a-f]{40}', review) is not None,
            'short_review_exact_head_missing')
    require(cfg.get('ci_run_id') == CI_RUN_ID and cfg.get('source_git_sha') == HEAD
            and cfg.get('binary_sha256') == BINARY,
            'short_native_ci_pin_changed')
    native_run = fetch('actions/runs/' + str(CI_RUN_ID))
    require(successful_run(native_run, CI_RUN_ID, HEAD), 'short_native_exact_ci_not_green')
    review_run = fetch('actions/runs/' + str(review_ci))
    require(successful_run(review_run, review_ci, review), 'short_review_exact_ci_not_green')
    pr = fetch('pulls/68')
    require(pr.get('state') == 'open' and pr.get('draft') is True
            and pr['head']['sha'] == review, 'short_protected_review_head_changed')
    comparison = fetch('compare/' + HEAD + '...' + review)
    files = comparison.get('files', [])
    require(comparison.get('merge_base_commit', {}).get('sha') == HEAD
            and comparison.get('status') in ('ahead', 'identical')
            and type(comparison.get('total_commits')) is int
            and comparison['total_commits'] <= 250 and len(files) < 300,
            'short_native_review_comparison_incomplete')
    require(all(row.get('status') != 'renamed' and
                (row.get('filename', '').startswith('scripts/test_rehearsal/')
                 or row.get('filename') in ('CERTIFICATION.md', 'CERTIFICATION_DAY_RUNBOOK.md'))
                for row in files), 'short_review_contains_native_or_unapproved_changes')
    checks = fetch('commits/' + review + '/check-runs')['check_runs']
    latest = {}
    for item in checks:
        if item.get('name') in JOBS:
            latest.setdefault(item['name'], item)
    require(set(latest) == JOBS and all(row['status'] == 'completed'
            and row['conclusion'] == 'success' and row.get('head_sha') == review
            and type(row.get('id')) is int and row['id'] > 0 for row in latest.values()),
            'short_review_both_ci_jobs_not_green')
    summary = lambda row: {key: row.get(key) for key in ('id', 'head_sha', 'status', 'conclusion')}
    paths = [{key: row.get(key) for key in ('filename', 'status')} for row in files]
    receipt = dict(schema='MOEX_SHORT_CI_SEAL_V1', source_git_sha=HEAD, binary_sha256=BINARY,
        native_ci_run_id=CI_RUN_ID, review_git_sha=review, review_ci_run_id=review_ci,
        native_run=summary(native_run), review_run=summary(review_run),
        protected_pr=dict(number=68, state=pr['state'], draft=pr['draft'], head_sha=pr['head']['sha']),
        comparison=dict(merge_base_sha=comparison['merge_base_commit']['sha'],
            status=comparison['status'], total_commits=comparison['total_commits'], files=paths,
            files_sha256=hashlib.sha256(json.dumps(paths,sort_keys=True,separators=(',',':')).encode()).hexdigest()),
        scripts_and_documentation_only=True,
        jobs={name: row['conclusion'] for name, row in latest.items()},
        review_checks={name: {key: row.get(key) for key in ('id','head_sha','name','status','conclusion')}
                       for name,row in latest.items()}, checked_utc_ns=time.time_ns())
    base.atomic(Path(root) / 'ci-public.json', receipt)
    return receipt


def preflight_deadline(cfg, now=None):
    now = time.time() if now is None else now
    return min(now + 1800, cfg['product_start_latest_utc'])


def transient_network(error):
    if isinstance(error, urllib.error.HTTPError):
        headers = error.headers or {}
        rate_limited = (error.code == 403 and (headers.get('X-RateLimit-Remaining') == '0'
                        or headers.get('Retry-After') is not None))
        return rate_limited or error.code == 429 or 500 <= error.code <= 599
    return isinstance(error, (urllib.error.URLError, TimeoutError, ConnectionError,
                              http.client.IncompleteRead, ssl.SSLError))


def verify_ci_for_seal(root, cfg, tick=None):
    # Only a transport failure may retry. A real pin, CI, schema or review
    # refusal remains fatal. No native product is created by this loop.
    deadline = preflight_deadline(cfg)
    while True:
        try:
            return _verify_ci_once(root, cfg)
        except Exception as error:
            if not transient_network(error) or time.time() + 30 >= deadline:
                raise
            if tick is not None:
                tick()
            time.sleep(min(30, max(0, deadline - time.time())))


def verify_ci(root, cfg, tick=None):
    """Preflight uses only the exact manifest-pinned seal receipt, never GitHub."""
    require = base.require
    root = Path(root)
    manifest = json.loads((root / 'manifest.json').read_text())
    receipt_path = root / 'ci-public.json'
    pin = manifest.get('files', {}).get('ci-public.json')
    require(isinstance(pin,str) and re.fullmatch('[0-9a-f]{64}',pin) is not None
            and base.sha(receipt_path) == pin, 'short_sealed_ci_receipt_unpinned_or_changed')
    receipt = json.loads(receipt_path.read_text())
    review, review_ci = cfg.get('review_git_sha'), cfg.get('review_ci_run_id')
    require(isinstance(review,str) and re.fullmatch('[0-9a-f]{40}',review) is not None
            and type(review_ci) is int
            and receipt.get('schema') == 'MOEX_SHORT_CI_SEAL_V1'
            and receipt.get('source_git_sha') == cfg.get('source_git_sha') == HEAD
            and receipt.get('binary_sha256') == cfg.get('binary_sha256') == BINARY
            and receipt.get('native_ci_run_id') == cfg.get('ci_run_id') == CI_RUN_ID
            and receipt.get('review_git_sha') == review and receipt.get('review_ci_run_id') == review_ci
            and type(receipt.get('checked_utc_ns')) is int and receipt['checked_utc_ns'] > 0,
            'short_sealed_ci_receipt_pin_mismatch')
    require(successful_run(receipt.get('native_run',{}),CI_RUN_ID,HEAD)
            and successful_run(receipt.get('review_run',{}),review_ci,review),
            'short_sealed_ci_exact_runs_not_green')
    pr = receipt.get('protected_pr',{})
    require(pr.get('number') == 68 and pr.get('state') == 'open' and pr.get('draft') is True
            and pr.get('head_sha') == review, 'short_sealed_ci_protected_pr_unproved')
    comparison = receipt.get('comparison',{})
    paths = comparison.get('files',[])
    require(comparison.get('merge_base_sha') == HEAD and comparison.get('status') in ('ahead','identical')
            and type(comparison.get('total_commits')) is int and comparison['total_commits'] <= 250
            and isinstance(paths,list) and len(paths) < 300
            and comparison.get('files_sha256') == hashlib.sha256(json.dumps(paths,sort_keys=True,separators=(',',':')).encode()).hexdigest(),
            'short_sealed_ci_comparison_incomplete')
    require(receipt.get('scripts_and_documentation_only') is True and all(
            row.get('status') != 'renamed' and (row.get('filename','').startswith('scripts/test_rehearsal/')
            or row.get('filename') in ('CERTIFICATION.md','CERTIFICATION_DAY_RUNBOOK.md')) for row in paths),
            'short_sealed_ci_native_or_unapproved_changes')
    checks = receipt.get('review_checks',{})
    require(receipt.get('jobs') == {name:'success' for name in JOBS} and set(checks) == JOBS
            and all(row.get('name') == name and row.get('status') == 'completed'
                    and row.get('conclusion') == 'success' and row.get('head_sha') == review
                    and type(row.get('id')) is int and row['id'] > 0 for name,row in checks.items()),
            'short_sealed_ci_required_jobs_unproved')
    return receipt


def load_cfg(root):
    # Scope validation precedes selecting the one owner-authorized date; the
    # immutable base then verifies every manifest file and all native pins.
    candidate = json.loads((Path(root) / 'configuration-private.json').read_text())
    scope.window(candidate)
    base.AUTHORIZED_DATE = candidate['authorized_date']
    return base.load_cfg(root)


def day_check(cfg, entry=False):
    scope.window(cfg)
    base.AUTHORIZED_DATE = cfg['authorized_date']
    return base.day_check(cfg, entry=entry)


base.verify_ci = verify_ci
