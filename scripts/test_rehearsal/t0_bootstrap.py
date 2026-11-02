"""Exact candidate build validation for fresh held T0 bootstrap copies."""
import ast
from t0_preparation import SOURCE, BINARY

ARCHIVE = '90c31fad83a5e83becb6858b99a326fe27f27fefa4988f538fcd940d29e766b5'
BUILD = 'oct16-ownrow-0cbc6693-build-a1'


def verify_build(receipt, inputs, actual_binary_sha256):
    """Verify the original 39-test receipt; never manufacture a new build result."""
    tests = receipt.get('release_tests', {})
    if not (receipt.get('source_git_sha') == SOURCE and receipt.get('state') == 'BUILD_PASS'
            and receipt.get('candidate_binary_sha256') == BINARY == actual_binary_sha256
            and receipt.get('source_archive_sha256') == ARCHIVE
            and inputs.get('attempt') == BUILD and inputs.get('binary_sha256') == BINARY
            and inputs.get('archive_sha256') == ARCHIVE
            and all(type(tests.get(k)) is int and tests[k] == v
                    for k, v in [('passed',39),('tests',39),('failed',0),('skipped',0)])
            and receipt.get('trading_armed') is False):
        raise ValueError('t0_exact_candidate_build_unproved')
    return True


def stage_bootstrap(text):
    """Replace only the inherited build gate; all native work remains held."""
    old = "receipt=json.loads((attempt/'build-receipt.json').read_text());binary=attempt/'production/apps/moexctl'"
    gate = "if receipt['source_git_sha']!=HEAD or receipt['state']!='complete' or receipt['release_tests']['passed']!=37 or sha(binary)!=cfg['binary_sha256'] or receipt['production_binary_sha256']!=cfg['binary_sha256'] or receipt['source_archive_sha256']!=cfg['archive_sha256']:raise Stop('exact_build_identity_unverified')"
    if text.count(old) != 1 or text.count(gate) != 1:
        raise ValueError('t0_bootstrap_predecessor_changed')
    text = text.replace(old, "receipt=json.loads((attempt/'build-public.json').read_text());binary=attempt/'production/apps/moexctl'")
    text = text.replace(gate, "from t0_bootstrap import verify_build\n verify_build(receipt,cfg,sha(binary))")
    ast.parse(text)
    return text
