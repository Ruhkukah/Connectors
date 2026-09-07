# Raw ORDLOG throughput, D0

The standalone `plaza2_ordlog_benchmark` target drives the fake shared library's wire-message callback into the actual runtime
scheme dispatch, complete raw decode, transaction/revision checks, mandatory output and optional output queues. It never loads
the real vendor library or creates an exchange connection. Each run validates exact received/consumed counts on both queues.

Run the Release target with the fake runtime library path as its only argument; `--smoke` runs small unpaced fixtures for CTest.
The standard run uses 10 seconds each at offered rates of 100k and 200k, an unpaced 2-million-record burst, 4,096 instruments,
and a cycling million-ID public-order population above 2^53. Both regular and multileg tables are present, plus heartbeats/events.
The ID population is generator coverage; C1 has no active-order map. It is not evidence of million-order L3 memory consumption.

Local results on Apple M4 Pro / Darwin arm64, Apple clang 17 Release:

| Profile | Records | Seconds | Observed msg/s | Queue high-water |
| --- | ---: | ---: | ---: | ---: |
| Offered 100k | 1,000,000 | 10.0022 | 99,977.8 | 1,000 |
| Offered 200k | 2,000,000 | 10.0010 | 199,980 | 1,000 |
| Burst | 2,000,000 | 0.119432 | 16,746,000 | 32,768 |
| 4,096 instruments | 2,000,000 | 10.0008 | 199,983 | 1,000 |
| Million-ID population | 2,000,000 | 10.0011 | 199,979 | 1,000 |

All queues ended empty; decode/revision failures and dropped mandatory/optional records were zero. Peak process RSS reached
35,586,048 bytes (cumulative process high-water, not incremental allocator usage). Maximum sampled oldest queued-event age was
6,141,291 ns during the burst. Queue age includes transaction staging and is sampled before draining each batch.
Paced thresholds allow 2% scheduler overhead; the exact measured rates above are retained without rounding them up to a strict pass.

The machine-readable report with binary hashes is `docs/review/plaza2_offline_20260907/raw_d0.json`.
This is a short local engineering benchmark with synthetic callbacks, no networking, no vendor processing and no L3 mutation.
It is provisional raw performance only. Linux deployment hardware, longer endurance, actual CGate receive cost and complete
C2 L3 mutation must be qualified before any certification performance claim. The output explicitly sets that claim to false.
