# One MOEX TEST Add / cancel: successful

Run start: 2026-09-06T12:20:31Z. Source: ffc5df7f00000f1babc2a2df9cc5d9c950cdb5c1.
CGate 9.9, existing T1 router PID 3031619 unchanged before/after.
One continuously pumped host, PID 3375388, stopped normally after the lifecycle.

The user explicitly delegated fresh TEST price/plan selection without another
exact-SHA approval round. The temporary driver's delegated-authorization flag
used the freshly generated canonical SHA; no previous authorization was reused.
The legacy authorization-request wording in observation.log is informational,
not evidence of a separate human approval/file. All transport gates remained.

CRU6, isin 4433036, session 11700: SELL LIMIT 1 at 12.91600.
Generation BBO 12.90900 / 12.91500; one tick above ask, age 2491 ms.

- Plan SHA: d530759ff6e8f8d5f987999739512bf3fe187a3156b7ac817f8dbe8db4a35819
- Add payload: 2622b4c15e8feac5a98c7d8781e799f8eb55f9636dd8e6982d3a6991bd2a9f4e
- Recovery payload: 99e010f32f53e688a919253a3c1e53c076cd876d16c97f7bc3e0512f01995404
- Receipt SHA: 02896364c813b9d19e5149f7402dc5ccf5aac5d7d91eda4fee9db0af1c6b3f46

Canonical receipt bytes are in execution_safety_v4.canonical.bin; the JSON
sibling is a lossless pretty-print. Plan and journal are original text.

Add was Posted, correlated reply accepted, order ID 2087468678403610015.
TRADE independently observed Working, then Cancelled after one DelOrder.
Final quantities: original=1, remaining=0, executed=0. No fill/compensating order.
UOB was independently current and empty in pre-send census, not reconciled with
TRADE. Final UOB census was not separately captured; terminal proof is TRADE.

Actual application function-entry counts: cg_pub_msgnew=2, cg_pub_post=2,
corresponding to one Add and one DelOrder. Recovery post=false. No second Add.
Add allocation/post/free runtime codes were all zero; runtime and encoded
payload sizes were both 128. Raw correlated reply code/text were not included
in the summary; accepted status/order ID and reply observation are journaled.

Final: Cancelled, ok=true, market_safe_terminal=true, evidence_consistent=true,
journal_ok=true, journal_degraded=false. Journal finished normally; inspection
found only the finished journal file and no remaining identifier lock files.
This qualifies this bounded TEST lifecycle, not production deployment or full
MOEX certification. Stop for PR review; no additional orders were submitted.
