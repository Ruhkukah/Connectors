# PR #30 live send-disabled qualification — 2026-09-06

Implementation source: b499edc6f5e2e99f50a3376b88dbf2bdb5790699.
CGate: matching 9.9.1853 client/schemes and the existing T1 router.
One continuously pumped execution host; a preceding REFDATA-only lookup
confirmed current session 11700 and closed before the host started.

Result: LIVE_TEST_PRE_SEND_PASS. No actual TEST order was submitted.

- Fresh candidate: CRU6 / isin 4433036, SELL LIMIT quantity 1 at 12.92100.
- Plan SHA-256: 171b54f4c74eb2326c9e895d0869a7449185a9880ef56bb2620aba4bac8d29b3.
- Execution receipt SHA-256: a4570f73287b80c54a69220e6f3fba18b168aa58fb5c279dbd407770316caede.
- Generation BBO: 12.918 / 12.920; execution-time BBO: 12.918 / 12.919.
- Execution quote age: 1062 ms; distance: 2 ticks; allowed maximum: 4 ticks.
- All target provenance, anchored replay, flat-position, private-stream,
  UOB periodic consistency, publisher/reply, session/instrument status,
  passive price, and quantity-one gates passed in the v3 receipt.
- Intent install and bind succeeded. The normal transport post path returned
  SEND_DISABLED_PRE_SEND_PHASE / DefinitelyNotSent / post_invoked=false.
- Close completed with code 0. No host remains awaiting authorization.

Authorization basis: the user authorized all TEST exercises and then explicitly
requested the fix and actual testing. That authorization was applied only to
this new physically send-disabled candidate, not inherited from a retired plan
and not used to enable actual publisher posting.

Zero application cg_pub_msgnew/cg_pub_post calls and zero orders follow from the
verified LiveTestPreSend return-before-publisher path and the observed barrier;
these are not independently instrumented vendor-library call counters. CGate
internal replication acknowledgments are outside application order posting.

Validation: 158/158 full CTest on unchanged-code sequential rerun; changed-target
ASan/UBSan 5/5; diff/style/no-send checks passed. The initial parallel CTest hit
the unrelated TWIME establish-timeout test. Exact implementation-head GitHub
workflow 34028121182 passed connector-validation and component-sanitizers.

Replay confirmation: the fixed compiled projector replayed the raw 120-second
CGate target capture (52 target commits including snapshot, 50 after ONLINE)
and retained 40 levels, matching the independent row-ID reconstruction, rather
than the old 41. The two-commit synthetic false-cross fixture now retains two
levels with BBO 12.927 / 12.928, not 12.936 / 12.928.

The runtime log has only its local run-directory paths redacted. Canonical plan
and execution receipt bytes are unchanged. Old plans 122d0b1e..., 401b0b1c...,
and 70a099cc... remain retired. This evidence supports final PR #30 review;
it is not proof of order submission, cancellation, fills, or certification.
