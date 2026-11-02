"""Pure text staging of review-only T0 copies, with no filesystem or execution API."""
from __future__ import annotations

import ast
import hashlib
import re

from t0_preparation import SOURCE, T0_PROFILE


T1_PROFILE = "ff64e85ac33958025c928f7cafd5da1804e35406bf3f016058f13e5826ed597a"
T1_DATES = ("2026-10-16", "2026-10-19")
T0_DATE = "2026-10-21"


def _require(condition, code):
    if not condition:
        raise ValueError(code)


def _sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _tree(text):
    try:
        return ast.parse(text)
    except (SyntaxError, ValueError):
        raise ValueError("t0_stage_python_syntax") from None


def _assignments(text, name):
    values = []
    for node in ast.walk(_tree(text)):
        targets = node.targets if isinstance(node, ast.Assign) else [node.target] if isinstance(node, ast.AnnAssign) else []
        if any(isinstance(target, ast.Name) and target.id == name for target in targets):
            values.append(node.value)
    return values


def stage_sources(sources, replacements):
    """Return ``(copies, audit)`` without changing inputs, files, or authorization.

    The explicit path replacement dictionary is an allowlist. Paths and source
    content stay out of the audit. Originals, including executed bundles, must
    be preserved by the caller. These copies require independent composition
    review and fresh authorization; they are not an installable job package.
    """
    _require(type(sources) is dict and bool(sources), "t0_stage_sources")
    for name, text in sources.items():
        _require(type(name) is str and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*\.py", name)
                 is not None and type(text) is str, "t0_stage_python_sources_only")
        _tree(text)
    joined = "\n".join(sources.values())
    _require(SOURCE in joined, "t0_stage_native_source_pin_missing")
    _require(T1_PROFILE in joined, "t0_stage_original_profile_pin_missing")
    _require(type(replacements) is dict, "t0_stage_path_allowlist")
    keys, targets = list(replacements), list(replacements.values())
    for old, new in replacements.items():
        _require(type(old) is str and type(new) is str
                 and all(path.startswith("/") and len(path) > 1 and not path.endswith("/")
                         and not any(char in path for char in "\r\n\0") for path in (old, new)),
                 "t0_stage_absolute_paths_only")
        _require(old != new and old in joined and new not in joined, "t0_stage_path_noop_or_missing")
    _require(len(set(targets)) == len(targets), "t0_stage_path_target_collision")
    _require(not any(a in b for i, a in enumerate(keys) for j, b in enumerate(keys) if i != j)
             and not any(old in new for old in keys for new in targets), "t0_stage_ambiguous_paths")
    copies, rows, locks = {}, [], 0
    substitutions = {T1_PROFILE: T0_PROFILE, "links_public.t1.ini": "links_public.t0.ini",
                     "client_t1.ini": "client_t0.ini", **replacements}
    pattern = re.compile("|".join(re.escape(key) for key in sorted(substitutions, key=len, reverse=True)))
    for name, original in sources.items():
        for value in _assignments(original, "T1_LINKS_SHA"):
            _require(isinstance(value, ast.Constant) and value.value == T1_PROFILE,
                     "t0_stage_wrong_profile_pin")
        for value in _assignments(original, "REVIEW_ONLY"):
            _require(isinstance(value, ast.Constant) and type(value.value) is bool,
                     "t0_stage_review_lock_not_literal")
            locks += 1
        for value in _assignments(original, "AUTHORIZED_DATE"):
            _require(isinstance(value, ast.Constant) and value.value in T1_DATES,
                     "t0_stage_unexpected_calendar_scope")
        text, count = pattern.subn(lambda match: substitutions[match.group()], original)
        text, lock_count = re.subn(r"(?m)^(\s*REVIEW_ONLY\s*=\s*)False\b", r"\g<1>True", text)
        text, date_count = re.subn(r"(?m)^(\s*AUTHORIZED_DATE\s*=\s*)(['\"])2026-10-(?:16|19)\2",
                                  r"\g<1>'2026-10-21'", text)
        _tree(text)
        _require(all(isinstance(value, ast.Constant) and value.value is True
                     for value in _assignments(text, "REVIEW_ONLY")), "t0_stage_review_lock_failed")
        _require(all(isinstance(value, ast.Constant) and value.value == T0_DATE
                     for value in _assignments(text, "AUTHORIZED_DATE")), "t0_stage_calendar_failed")
        _require(not any(token in text for token in (T1_PROFILE, "links_public.t1.ini", "client_t1.ini")),
                 "t0_stage_t1_routing_remains")
        _require(not any(old in text for old in keys), "t0_stage_old_paths_remain")
        copies[name] = text
        rows.append(dict(file=name, original_sha256=_sha(original), staged_sha256=_sha(text),
                         substitutions=count + lock_count + date_count))
    _require(locks > 0, "t0_stage_review_lock_missing")
    _require(SOURCE in "\n".join(copies.values()), "t0_stage_native_source_pin_changed")
    return copies, dict(status="REVIEW_ONLY_UNARMED", can_arm=False, can_trade=False,
                        execution_composition_review_required=True, source_files_untouched=True,
                        native_source=SOURCE, t0_profile_sha256=T0_PROFILE, date=T0_DATE,
                        files=rows, path_changes=[dict(index=index, replacements=joined.count(old))
                                                for index, old in enumerate(keys)])
