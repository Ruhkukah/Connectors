# Durable PLAZA integration

Base main: 82707d4e00b1bb6bf703be800ff31988990116df.
Reference functional tree: PR55 e004251ae932a2d15736ecf3bc07d03fcf63d160.

Ported the durable effects of PR47 (participant identity, effective health, wire
forensics), PR48 (bounded transport recovery), PR50 (participant diagnostics and
signal support), PR52 (session terms), PR53 (process cause policy) and PR55
(proven-identity reopen). No production implementation was rewritten.

`equivalence.json` inventories every runtime C++ source/header in the PR55 tree,
including unchanged unrelated modules. All retained files are IDENTICAL. The
dated AGGR qualification executable is EXCLUDED_HARNESS_ONLY and removed from
the build together with its date-specific launcher and two executable-only tests.
Reusable forensic/private diagnostic/signal helpers and tests are retained exactly.
The historical executable remains accessible by its old immutable PR/source SHA.
No PR49/54/56 authorization date is imported as permanent production semantics.

There are no unexplained runtime differences. New send-gate and recovered-cancel
work will be separately reviewable commits/PRs on this base. CI runs the complete
Release suite including preflight, and the existing Linux ASan/UBSan/LSan suite.
No VPS mutation, T1 connection, exchange order, merge, or performance optimization.

## Receipt scope in follow-on branches

The equivalence manifest describes clean integration commit
0263bb15fbf96235216903c1209aa33a8be84ac7 (PR57), not the later price-gate or
recovered-cancel implementations. Those are intentional separately reviewed
runtime differences on descendants of the receipt's candidate.
