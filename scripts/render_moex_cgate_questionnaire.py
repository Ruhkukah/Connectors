#!/usr/bin/env python3
"""Render the review attachment from the machine-readable questionnaire register."""

from __future__ import annotations

import argparse
import json
import textwrap
from pathlib import Path


REGISTER_REL = Path("docs/review/moex_cgate_questionnaire_register_9_9_20260919.json")
OUTPUT_REL = Path("docs/review/moex_cgate_questionnaire_draft_9_9_20260919.md")


def md(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", "<br>")


def wrapped(value: object, *, initial: str = "", width: int = 110) -> list[str]:
    return textwrap.wrap(
        str(value),
        width=width,
        initial_indent=initial,
        subsequent_indent="  ",
        break_long_words=False,
        break_on_hyphens=False,
    ) or [initial.rstrip()]


def render(root: Path) -> str:
    register = json.loads((root / REGISTER_REL).read_text(encoding="utf-8"))
    lines = [
        "# MOEX CGate questionnaire — review draft (SPECTRA 9.9)",
        "",
        "> Draft only; not a completed or submitted questionnaire. User-owned, legal, consent, distribution,",
        "> and deployment answers remain visible as unresolved. No credentials or private account data belong here.",
        "",
        "## Snapshot and identity",
        "",
        f"- As of: `{register['as_of']}`.",
        (
            f"- Structure preserved: **{register['form_structure']['answer_count']} answer fields**, "
            f"including **{register['form_structure']['printed_stream_checkbox_count']} printed stream checkboxes**."
        ),
        f"- PR #66 baseline at review start: `{register['source_identity']['source_head_at_review_start']}`.",
        *wrapped(
            f"- CI run [{register['source_identity']['ci_run']}]"
            f"({register['source_identity']['ci_url']}) at the exact head: "
            "`connector-validation` and `component-sanitizers` both **SUCCESS**."
        ),
        *wrapped(f"- Deployable Linux binary SHA-256: **not claimed**. {register['source_identity']['binary_note']}"),
        *wrapped(
            f"- Historical receipt source prefix `{register['source_identity']['historical_receipt_source_prefix']}` "
            "is from another snapshot and is not evidence for this head."
        ),
        "",
        "## Product and demonstration boundary",
        "",
        f"- Certificate target: {register['scope']['certificate_target']}",
        f"- Current demonstration: {register['scope']['current_demonstration']}",
        f"- Trading: {register['scope']['trading_scope']}",
        f"- FullOrderLog: {register['scope']['full_order_log']}",
        "",
        "## Effective receive-scheme policy",
        "",
        register["scheme_policy"]["rule"],
        "",
        "| Profile | Service | Scheme alias | Effective policy |",
        "| --- | --- | --- | --- |",
    ]
    for profile_name, label in (
        ("four_stream_read_only_profile", "Four-stream read-only"),
        ("eight_stream_connector_profile", "Eight-stream ConnectorHost"),
    ):
        profile = register["scheme_policy"][profile_name]
        for stream in profile["streams"]:
            lines.append(
                f"| {label} | `{stream['service']}` | `{stream['scheme_alias'] or 'server default'}` | `{stream['policy']}` |"
            )
        lines.append(f"| {label} | *initial open arguments* | — | `{md(profile['initial_open_settings'])}` |")
    alias = register["scheme_policy"]["ordbook_alias_review"]
    alias_prose = f"{alias['finding']} {alias['disposition']}"
    lines += [
        "",
        *wrapped(
            "The effective-profile guard derives listener policy from configured URLs: four-stream profile = "
            "2 explicit / 2 server-default; eight-stream profile = 6 explicit / 2 server-default. No URL changed."
        ),
        "",
        "### OrdBook alias limitation",
        "",
        *wrapped(alias_prose),
        "",
        "## Current scheme inventory, requested streams, and consumed tables",
        "",
        *wrapped(
            "Supported-table inventory uses the pinned SPECTRA 9.9 scheme and excludes two tables documented as removed."
        ),
        "It is reference inventory only: it does not assert a table-filtered request or an actual listener OPEN.",
        "CGate URLs request streams without a per-table filter; exact negotiated OPEN tables remain pending.",
        "",
        "### Supported current scheme tables",
        "",
    ]
    for stream in register["stream_inventory"]:
        lines.extend(
            wrapped(
                ", ".join(stream["supported_current_scheme_tables"]),
                initial=f"- `{stream['service']}` tables: ",
                width=108,
            )
        )
    lines += [
        "",
        "### Stream-level request and product consumption",
        "",
        "No current-candidate OPEN receipt exists for these streams.",
        "",
        "| Service | Scheme policy | Stream request | Product-consumed tables |",
        "| --- | --- | --- | --- |",
    ]
    for stream in register["stream_inventory"]:
        lines.append(
            "| `{service}` | `{policy}` | {requested} | {consumed} |".format(
                service=stream["service"],
                policy=stream["scheme_policy"],
                requested=md(stream["requested_tables"]),
                consumed=md(", ".join(stream["consumed_tables"])),
            )
        )
    historical = register["historical_compatibility_only"]
    lines += [
        "",
        "### Historical compatibility names",
        "",
        *wrapped(
            f"`{historical['tables'][0]}` and `{historical['tables'][1]}` are **historical compatibility-only** "
            f"for this draft. {historical['repository_lock_note']}"
        ),
        "",
        f"Official source: [{historical['current_9_9_source']}]({historical['current_9_9_source']}).",
        "",
        "## 77-field answer register",
        "",
        "Each row resolves its source, automated test, and live evidence through the evidence reference(s). `NOT_RUN` is not a pass.",
        "",
    ]
    catalog = register["evidence_catalog"]
    for field in register["answers"]:
        lines += [
            f"### {field['id']} — {field['question']}",
            "",
            *wrapped(field["answer"], initial="- Proposed answer: "),
            f"- Status: `{field['status']}`",
        ]
        for ref in field["evidence_refs"]:
            evidence = catalog[ref]
            sources = ", ".join(f"`{path}`" for path in evidence["source_files"]) or "None; user-owned answer."
            lines += [
                *wrapped(sources, initial="- Source: "),
                *wrapped(evidence["automated_test"], initial="- Automated test/evidence: "),
                *wrapped(evidence["live_evidence"], initial="- Live evidence: "),
            ]
        lines.append("")
    lines += [
        "## Remaining limits before submission or trading-profile qualification",
        "",
        *wrapped(
            "Capture the exact current-candidate OPEN and negotiated scheme for each configured listener, "
            "especially both server-scheme status streams.",
            initial="- ",
        ),
        *wrapped(
            "Resolve the `OrdBook` versus USERORDERBOOK layout mismatch from an exact OPEN/client-scheme "
            "receipt before claiming eight-stream qualification. Do not block the four-stream observer.",
            initial="- ",
        ),
        *wrapped(
            "Confirm legal identity, certificate holder, release version, business use, distribution, contacts, "
            "intended sessions, and consent with the user.",
            initial="- ",
        ),
        *wrapped(
            "Do not infer `conn_process` cadence from sleeps; collect a measured active/idle/recovery sample.",
            initial="- ",
        ),
        *wrapped(
            "No exchange order, support email, automated questionnaire submission, or live T1 claim is part "
            "of this draft.",
            initial="- ",
        ),
        "",
        "Historical references:",
        "[MOEX CGate client manual — data scheme policy](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/Game/docs/cgate_en.pdf)",
        "[MOEX SPECTRA 9.9 gateway documentation](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html).",
        "",
        f"Machine-readable source: `{REGISTER_REL.as_posix()}`.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("repo_root", nargs="?", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--check", action="store_true", help="fail if the attachment is stale")
    args = parser.parse_args()
    root = args.repo_root.resolve()
    output = root / OUTPUT_REL
    expected = render(root)
    if args.check:
        if not output.is_file() or output.read_text(encoding="utf-8") != expected:
            raise SystemExit(f"generated questionnaire attachment is stale: {OUTPUT_REL}")
        return 0
    output.write_text(expected, encoding="utf-8")
    print(f"wrote {OUTPUT_REL}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
