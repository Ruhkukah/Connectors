"""Pure composition of held T0 review copies; never imports or executes them."""
from __future__ import annotations

import ast
import copy
import hashlib
import re

from t0_preparation import BINARY, SOURCE, T0_PROFILE

T1_PROFILE = 'ff64e85ac33958025c928f7cafd5da1804e35406bf3f016058f13e5826ed597a'
REQUIRED = frozenset(('router_recovery.py', 'upstream_recovery.py',
    'working_fault_core.py', 'short_fault_helpers.py', 'short_session.py'))
GUARD = '''\n_T0_COMPOSITION_HELD = True
def _t0_composition_guard():
    if _T0_COMPOSITION_HELD:
        raise RuntimeError('t0_composition_held_unarmed')
\n'''
STRICT_RATE = '''def burst_rate_metrics(posts):
    if any(type(stamp) is not int or stamp <= 0 for stamp in posts):
        raise base.DayStop('t0_burst_exact_native_post_timestamps_unproved')
    posts = sorted(posts)
    left = maximum = 0
    for right, stamp in enumerate(posts):
        while stamp - posts[left] >= 1_000_000_000:
            left += 1
        maximum = max(maximum, right - left + 1)
    six = min((posts[index + 5] - posts[index] for index in range(len(posts) - 5)), default=None)
    return dict(max_posts_per_rolling_second=maximum,
                max_posts_per_strict_ns_second=maximum, rate_window_slack_ns=0,
                minimum_six_post_span_ns=six,
                rate_semantics='strict_native_post_1000000000ns_zero_slack')
'''
PREENTRY_GATE = '''def _t0_verify_preentry(root, cfg, runtime):
    from t0_admission_scope import validate_preentry
    name = cfg.get('prearm_flatness_receipt')
    if not isinstance(name, str) or Path(name).name != name or not name:
        raise RuntimeError('t0_preentry_receipt_path_unproved')
    path = Path(root) / name
    if path.is_symlink() or not path.is_file():
        raise RuntimeError('t0_preentry_receipt_path_unproved')
    validate_preentry(json.loads(path.read_text()), cfg, runtime.sha(path), time.time_ns())


'''


def require(value, code):
    if not value:
        raise ValueError(code)


def sha(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def tree(text):
    try:
        result = ast.parse(text)
        compile(result, '<held-t0-review-copy>', 'exec')
        return result
    except (SyntaxError, ValueError, TypeError):
        raise ValueError('t0_composition_invalid_python') from None


def _values(text, name):
    result = []
    for node in ast.walk(tree(text)):
        targets = node.targets if isinstance(node, ast.Assign) else []
        if any(isinstance(target, ast.Name) and target.id == name for target in targets):
            require(isinstance(node.value, ast.Constant) and isinstance(node.value.value, str),
                    't0_composition_dynamic_protected_pin')
            result.append(node.value.value)
    return result


def _replace_function(text, name, replacement):
    nodes = [node for node in tree(text).body if isinstance(node, ast.FunctionDef) and node.name == name]
    require(len(nodes) == 1, 't0_composition_rate_function_not_unique')
    node = nodes[0]
    lines = text.splitlines(keepends=True)
    return ''.join(lines[:node.lineno-1]) + replacement + ''.join(lines[node.end_lineno:])


def _held(text):
    """Guard every entry before its first effect, preserving existing source text."""
    parsed = tree(text)
    require(not any(isinstance(node, ast.Name) and node.id.startswith('_t0_composition_')
                    for node in ast.walk(parsed)), 't0_composition_already_held')
    lines = text.splitlines(keepends=True)
    starts, offset = [], 0
    for line in lines:
        starts.append(offset)
        offset += len(line)
    inserts = []
    for node in ast.walk(parsed):
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) or node.name not in ('main', 'run', 'load_cfg', 'setup', 'start'):
            continue
        body = node.body
        first = body[0]
        if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant) and isinstance(first.value.value, str):
            if len(body) == 1:
                at = starts[first.end_lineno-1] + first.end_col_offset
                inserts.append((at, '\n' + ' ' * first.col_offset + '_t0_composition_guard()'))
                continue
            first = body[1]
        at = starts[first.lineno-1] + first.col_offset
        same_line = node.lineno == first.lineno
        inserts.append((at, '_t0_composition_guard(); ' if same_line else
                        '_t0_composition_guard()\n' + ' ' * first.col_offset))
    for at, insertion in sorted(inserts, reverse=True):
        text = text[:at] + insertion + text[at:]
    parsed = tree(text)
    prefix_end = 0
    for index, node in enumerate(parsed.body):
        is_doc = index == 0 and isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant) and isinstance(node.value.value, str)
        if is_doc or isinstance(node, ast.ImportFrom) and node.module == '__future__':
            prefix_end = node.end_lineno
        else:
            break
    lines = text.splitlines(keepends=True)
    text = ''.join(lines[:prefix_end]) + GUARD + ''.join(lines[prefix_end:])
    tree(text)
    return text, len(inserts)


def _map(value, transform):
    if isinstance(value, dict):
        return {key: _map(item, transform) for key, item in value.items()}
    if isinstance(value, list):
        return [_map(item, transform) for item in value]
    if isinstance(value, tuple):
        return tuple(_map(item, transform) for item in value)
    return transform(value) if isinstance(value, str) else value


def compose(sources, cfg, replacements):
    """Return fresh ``(sources, held_config, public_audit)`` without any I/O.

    Caller supplies copied historical sources plus reviewed T0 scope/preparation
    modules. Authorization and ID allocation are independent of this function.
    Hashes are dependency ordered: router, upstream.PROBE_SHA, upstream, core.
    """
    require(type(sources) is dict and REQUIRED <= sources.keys(), 't0_composition_source_set')
    require(type(cfg) is dict and type(replacements) is dict, 't0_composition_inputs')
    require(cfg.get('source_git_sha') == SOURCE and cfg.get('binary_sha256') == BINARY,
            't0_composition_native_pin_changed')
    require(cfg.get('target_venue') == 'T0' and cfg.get('authorized_date') == '2026-10-21'
            and cfg.get('router_port') == 4101 and cfg.get('router_profile_sha256') == T0_PROFILE
            and cfg.get('t0_profile_sha256', T0_PROFILE) == T0_PROFILE,
            't0_composition_t0_scope_changed')
    require(all(type(cfg.get(key)) is int and cfg[key] == value for key, value in
            (('quantity_limit', 2), ('position_limit', 2), ('open_order_limit', 5), ('rate_limit', 5))),
            't0_composition_risk_limits_changed')
    require(cfg.get('partial_fill_enabled') is False and cfg.get('timer_paused') is True,
            't0_composition_scope_changed')
    require(all(type(old) is str and type(new) is str and old and old != new
                and not any(char in old + new for char in '\r\n\0')
                for old, new in replacements.items()), 't0_composition_replacements')
    require(not any(a in b for a in replacements for b in replacements if a != b),
            't0_composition_ambiguous_replacements')
    for name, text in sources.items():
        require(type(name) is str and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*\.py', name)
                and type(text) is str, 't0_composition_python_sources_only')
        tree(text)
    old_router = set(_values(sources['upstream_recovery.py'], 'PROBE_SHA'))
    old_router.update(_values(sources['working_fault_core.py'], 'ROUTER_HELPER_SHA'))
    old_upstream = set(_values(sources['working_fault_core.py'], 'UPSTREAM_HELPER_SHA'))
    for key, values in (('router_helper_sha256', old_router), ('upstream_helper_sha256', old_upstream)):
        if cfg.get(key):
            values.add(cfg[key])
    require(len(old_router) == len(old_upstream) == 1 and all(re.fullmatch('[0-9a-f]{64}', item)
            for item in old_router | old_upstream), 't0_composition_inconsistent_initial_helper_pins')
    substitutions = {T1_PROFILE: T0_PROFILE, 'links_public.t1.ini': 'links_public.t0.ini',
                     'client_t1.ini': 'client_t0.ini', **replacements}
    pattern = re.compile('|'.join(re.escape(key) for key in sorted(substitutions, key=len, reverse=True)))
    replace = lambda value: pattern.sub(lambda match: substitutions[match.group()], value)
    copies, guarded = {}, {}
    for name, original in sources.items():
        text = replace(original)
        for node in ast.walk(tree(text)):
            targets = node.targets if isinstance(node, ast.Assign) else []
            if any(isinstance(target, ast.Name) and target.id == 'REVIEW_ONLY' for target in targets):
                require(isinstance(node.value, ast.Constant) and type(node.value.value) is bool,
                        't0_composition_dynamic_review_lock')
        text = re.sub(r'(?m)^(\s*REVIEW_ONLY\s*=\s*)False\b', r'\g<1>True', text)
        for node in ast.walk(tree(text)):
            targets = node.targets if isinstance(node, ast.Assign) else []
            if any(isinstance(target, ast.Name) and target.id == 'REVIEW_ONLY' for target in targets):
                require(isinstance(node.value, ast.Constant) and node.value.value is True,
                        't0_composition_review_lock_not_held')
        text = re.sub(r"(['\"])2026-10-(?:16|19)\1", "'2026-10-21'", text)
        if name == 'short_postcheck.py':
            text, count = re.subn(r'dt\.time\(23,\s*40\)', 'dt.time(15, 40)', text)
            require(count == 1 or 'dt.time(15, 40)' in text, 't0_composition_postcheck_deadline_missing')
        if name == 'supervisor.py':
            require('t0_admission_scope.py' in sources, 't0_composition_preentry_verifier_missing')
            old_gate = "cfg.get('predecessor_clean_terminal_verified') is True"
            require(text.count(old_gate) == 1 and text.count('def validate(cfg):') == 1,
                    't0_composition_prior_terminal_gate_changed')
            text = text.replace(old_gate, "cfg.get('fresh_T0_preentry_verification_required') is True")
            text = text.replace('def validate(cfg):', PREENTRY_GATE + 'def validate(cfg):')
            cls = [node for node in tree(text).body if isinstance(node, ast.ClassDef) and node.name == 'Supervisor']
            require(len(cls) == 1, 't0_composition_supervisor_class_changed')
            methods = [node for node in cls[0].body if isinstance(node, ast.FunctionDef) and node.name == 'start']
            require(len(methods) == 1, 't0_composition_start_method_changed')
            first = methods[0].body[0]
            lines = text.splitlines(keepends=True)
            lines.insert(first.lineno - 1, ' ' * first.col_offset +
                         '_t0_verify_preentry(self.root, self.cfg, self.rt)\n')
            text = ''.join(lines)
            start_anchor = 'self.child=ops.subprocess.Popen(self.args,cwd=self.work,env=self.env,'
            if 'class ProtectedProduct(ops.Product):' in text:
                require(text.count(start_anchor) == 1, 't0_composition_native_start_site_changed')
            if start_anchor in text:
                require('t0_start_budget.py' in sources and text.count(start_anchor) == 1,
                        't0_composition_start_budget_missing')
                text = text.replace(start_anchor,
                    "from t0_start_budget import claim_start\n"
                    "                    claim_start(owner.root, owner.cfg, purpose='planned_restart' if owner.restart_used else 'initial', "
                    "binary=self.binary, state=owner.environment['state'], args=self.args)\n"
                    "                    " + start_anchor)
        if name == 'guardian.py' and "cfg = runtime.load_cfg(args.root)" in text:
            anchor = "cfg = runtime.load_cfg(args.root)"
            require(text.count(anchor) == 1, 't0_composition_guardian_load_config_changed')
            text = text.replace(anchor, anchor + "\n    from base_supervisor import module_from\n"
                "    load_helper = lambda filename, logical: module_from(args.root, filename, logical)\n"
                "    load_helper('supervisor.py', 't0_guardian_helper_initializer').load_fault_helpers(load_helper, runtime, cfg)")
            recovery_anchor = '        product.start()\n        self.recovery_child = product.child'
            if 'def recovery(self, meta, public):' in text:
                require(text.count(recovery_anchor) == 1, 't0_composition_guardian_start_site_changed')
            if recovery_anchor in text:
                require('t0_start_budget.py' in sources and text.count(recovery_anchor) == 1,
                        't0_composition_guardian_start_budget_missing')
                text = text.replace(recovery_anchor,
                    "        from t0_start_budget import claim_start\n"
                    "        claim_start(self.root, self.cfg, purpose='guardian_recovery', "
                    "binary=meta['binary'], state=meta['state'], args=args)\n" + recovery_anchor)
        if name == 'short_session.py':
            text = _replace_function(text, 'burst_rate_metrics', STRICT_RATE)
            final = 'self.stage("one_passive_router_restart",lambda:self.fault("one_passive_router_restart","router"))'
            if 'def final_working_SIGTERM(' in text:
                require(text.count(final) == 1, 't0_composition_final_plan_changed')
                text = text.replace(final, 'self.stage("one_passive_upstream_socket_loss",lambda:self.fault("one_passive_upstream_socket_loss","upstream"))\n        ' + final)
        copies[name], guarded[name] = _held(text)
    router_pin = sha(copies['router_recovery.py'])
    for name in copies:
        if name != 'router_recovery.py':
            for previous in old_router:
                copies[name] = copies[name].replace(previous, router_pin)
    upstream_pin = sha(copies['upstream_recovery.py'])
    for name in copies:
        if name != 'upstream_recovery.py':
            for previous in old_upstream:
                copies[name] = copies[name].replace(previous, upstream_pin)
    require(_values(copies['upstream_recovery.py'], 'PROBE_SHA') == [router_pin]
            and _values(copies['working_fault_core.py'], 'ROUTER_HELPER_SHA') == [router_pin]
            and _values(copies['working_fault_core.py'], 'UPSTREAM_HELPER_SHA') == [upstream_pin],
            't0_composition_final_helper_pins')
    for text in copies.values():
        require(T1_PROFILE not in text and 'links_public.t1.ini' not in text,
                't0_composition_t1_routing_remains')
        require(all(value == T0_PROFILE for name in ('T1_LINKS_SHA', 'WHITE')
                    for value in _values(text, name)), 't0_composition_profile_pin_inconsistent')
        tree(text)
    pins = {sha(sources[name]): sha(text) for name, text in copies.items()}
    pins.update({pin: router_pin for pin in old_router})
    pins.update({pin: upstream_pin for pin in old_upstream})
    result = _map(copy.deepcopy(cfg), lambda value: pins.get(value, replace(value)))
    result.update(review_only=True, armed=False, allow_orders=False, owner_authorized=False,
                  execution_authorization_required=True, timer_paused=True,
                  execution_composition_verified=True, strict_rate_window_ns=1_000_000_000,
                  rate_slack_ns=0, router_helper_sha256=router_pin, upstream_helper_sha256=upstream_pin,
                  t0_profile_sha256=T0_PROFILE)
    audit = dict(schema='MOEX_T0_HELD_COMPOSITION_V1', status='VALIDATED_HELD_UNARMED',
        execution_composition_verified=True, can_arm=False, can_trade=False,
        native_source=SOURCE, binary_sha256=BINARY, limits=dict(quantity=2, position_per_instrument=2,
            open_orders=5, rate_per_second=5), strict_rate_window_ns=1_000_000_000, rate_slack_ns=0,
        router_helper_sha256=router_pin, upstream_helper_sha256=upstream_pin,
        files=[dict(file=name, original_sha256=sha(sources[name]), composed_sha256=sha(text),
                    guarded_entry_count=guarded[name]) for name, text in sorted(copies.items())],
        no_import_or_native_execution=True, source_files_untouched=True)
    return copies, result, audit
