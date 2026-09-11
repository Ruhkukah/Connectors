# Proposed sequential merge — no execution authorized

After review of #60 and #61, verify the exact approved heads and green full CI for all five drafts. Merge #57
into current main with a merge commit, preserving its source-equivalence receipt and original live identities.
Then retarget #58 to main and inspect its remaining diff; validate the exact merge result before merging.
Repeat in order for #59, #60 and #61. Preserve commit history; do not relabel a new merge SHA as the live PR56
source. Any conflict or changed diff requires review and the relevant complete validation before that merge.

After the final merge, run the complete Release and sanitizer suites on the actual merged main (including
Linux leak detection and guards), verify all five approved heads are ancestors, and record that durable source
identity. Only then seek separate approval to prepare a dated first-order harness from this single main.
No merge, dated harness, T1 connection or order is authorized by this document.

The unchanged performance baseline remains AGGR 40k single-row ~331.6 us observer OFF / ~847.0 us ON and private
20k single-row ~11.095 ms, ~139,999 allocations / ~15.61 MB on the recorded local machine. No optimization or new
capacity claim is made. Performance refactors wait until the first ordinary live lifecycle is proven.
