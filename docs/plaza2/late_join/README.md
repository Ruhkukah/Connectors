# AGGR20 late-join support packet

Prepared 2026-09-18. Draft only; nothing sent to MOEX.

- [Support request](SUPPORT_REQUEST_RU.md): concise technical question for MOEX.
- [Verified sources](SOURCES.md): exact 9.9 sections and provenance limitations.
- [Architecture notes](ARCHITECTURE.md): proposed authority, observer, Game and ORDLOG work.
- [Compact evidence](evidence.json): UTF-8 derivative, not a replacement for sealed evidence.
- [External manifest](external_manifest.json): immutable evidence pointers and verified hashes.

The two 120-second observations show no current-session readiness event after ONLINE.
They do not prove that such events are never emitted, or that the proposed late-join
criterion is accepted by MOEX. The evidence does not establish fresh SESSIONSTATE and
INSTRUMENTSTATE corroboration. `LateJoinCorroboratedSnapshot` is therefore a proposed
read-only policy, not an achieved state for this run or an exchange-approved barrier.

Order authority remains blocked on a precise MOEX interpretation or another documented
equivalent synchronization mechanism, plus independent trading safety gates. Read-only
engineering can proceed. This packet does not claim that all local alternatives were
exhausted, that the full foundation is resolved, or that current CI is green.

The sealed experiment used source `2c5464a96f2014b3d53f3f32015a4d4a7dbc47d5`.
Architecture notes describe the inspected September 18 working-tree implementation
on that baseline, distinguishing implemented behavior from pending operational proof.

No raw logs, executable, source archive, credentials, or sealed bundle are included.
PR #64 is closed as superseded; its external evidence is preserved.

Validation performed: strict UTF-8 decoding of all six packet files; both JSON
documents parse and roundtrip; selected attempt/witness facts match the externally
held source; the exact name bytes decode to the recorded Cyrillic text; all 24
LOCAL_SHA256SUMS entries and each external_manifest.json file hash match.
`unicode_guard.py`, `repo_style_check.py --paths` and `source_style_check.py` pass
for this directory. Full repository CI and implementation validation are not
established by this packet.
