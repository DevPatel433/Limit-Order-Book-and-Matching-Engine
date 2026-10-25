# Milestones, issues and defect log

This is the project's development history in issue-tracker form: one milestone per phase, each with its
issues and acceptance criteria, and the defects found along the way. It was **reconstructed from the phase
deliverables** (the repository was built phase by phase), not exported from a tracker. The
`.github/ISSUE_TEMPLATE/` files are provided so the same structure can be used on GitHub.

| Milestone | Goal | Exit evidence |
|---|---|---|
| M0 Scaffold | reproducible build, CI, rules written down | clean-clone build; CI config |
| M1 Book | tick-price data model, levels, snapshots, invariants | `test_order_book`, `validate()` |
| M2 Matching | price-time matching, fills, cancel, events | `test_matching_engine`, naive-model test |
| M3 Replay & properties | deterministic replay, checker, property tests | `test_replay`, `test_property`, regression dir |
| M4 Measure | benchmarks, percentiles, profiling | [results_baseline.md](results_baseline.md) |
| M5 Optimise | one measured optimisation vs a kept baseline | [results_phase5.md](results_phase5.md) |
| M6 Polish | extensions, docs, review-readiness | [phase6_design.md](phase6_design.md) |
| M7 Scale-out | multiple instruments, concurrent input | [phase7_design.md](phase7_design.md), [results_phase7.md](results_phase7.md) |
| M8 Durability | write-ahead log, crash recovery | this phase; [phase8_design.md](phase8_design.md), [wal_format.md](wal_format.md), [results_phase8.md](results_phase8.md) |

## Issues (all closed)
**M0**
- [x] CMake with C++20, `-Wall -Wextra -Wpedantic`, sanitizer option, GoogleTest. *Done when:* fresh clone builds from the documented commands.
- [x] CI: configure, build, test on each push (gcc/clang, Debug/Release, sanitizers). *Done when:* workflow present and green.
- [x] Written matching specification. *Done when:* `docs/matching_rules.md` states crossing, FIFO, partial-fill and cancel semantics.

**M1**
- [x] Tick-integer price/quantity types with range and overflow guards.
- [x] `PriceLevel` FIFO with O(1) erase by handle; `OrderBook` with ID index.
- [x] Sorted, deterministic snapshots; best bid/ask/spread/depth.
- [x] Debug invariants: every live order in exactly one level and in the index.

**M2**
- [x] Matching loop with price-time priority; trade at maker price; partial fills; residual resting.
- [x] Cancel; duplicate-ID and unknown-cancel behaviour defined and tested.
- [x] Event stream (`Accepted`, `Trade`, `Cancelled`, `Rejected`).

**M3**
- [x] Replay file format + strict parser; deterministic output; `lob_replay`.
- [x] Seeded generator, portable across standard libraries (`mt19937_64` + modulo).
- [x] Independent consistency checker (event-sourced ledger); tested with fabricated violations.
- [x] Property tests with conservation law; regression-case workflow.

**M4**
- [x] Five workloads (non-crossing, cancel-heavy, crossing-heavy, many-levels, bursty).
- [x] Throughput + latency percentiles (sample-size gated) + allocation tracking; CSV; plots.
- [x] Refuse Debug/sanitizer benchmark builds; record environment in every row.
- [x] Profile hot paths (callgrind; `perf` script for real hardware).

**M5**
- [x] Choose one target from evidence (allocation churn).
- [x] Policy templates so baseline stays selectable; `SlabArena`.
- [x] Identical correctness suites on both engines + differential tests.
- [x] Paired, interleaved before/after with sign tests; design note with trade-offs.

**M6**
- [x] IOC/FOK, market orders, amend, each with explicit semantics and tests.
- [x] CSV output and an interactive terminal view.
- [x] Lifecycle, complexity and limitations docs; interview notes.
- [x] Mutation-test the checker; fix what it finds.
- [x] Quickstart script and CMake presets so a reviewer can build and run in minutes.
- [ ] *Not done by design:* multiple instruments, self-trade prevention, concurrency (see [limitations.md](limitations.md)).

**M7**
- [x] Multi-instrument format and router; per-symbol IDs; isolation tested against the single-book engine.
- [x] Lock-free SPSC queue; sharded pipeline with deterministic merge; failure handling that cannot deadlock.
- [x] Differential tests against the sequential run (shards 1-16, both engines, tiny queues, skew, repeats).
- [x] ThreadSanitizer preset and CI job; mutation tests of the pipeline.
- [x] Scaling benchmark with a correctness gate, partition bound, oversubscription flag.
- [ ] *Not done:* measured speedup (one-CPU machine), streaming output, load-aware sharding, venue-wide IDs.

### Mutation testing of the Phase 7 pipeline
Ten injected bugs, each applied alone to a clean copy, the relevant tests run, the source restored afterwards.

| # | Injected bug | Result |
|---|---|---|
| M1 | route commands by sequence number instead of symbol | killed (10 tests) |
| M2 | no merge: concatenate shard outputs | killed (9) |
| M3 | merge picks the *largest* head sequence number | killed (7) |
| M4 | worker exits instead of draining after a failure | killed (deadlock/timeout) |
| M5 | router ignores the symbol (one shared engine) | killed |
| M6 | SNAPSHOT does not consume a sequence number | killed (1) |
| M7 | queue full-check off by one (overwrites an unread slot) | killed (7) |
| M8 | `collect_events=false` skips tallying | killed (1) |
| M9 | parser silently routes an unprefixed line to a symbol | killed (2) |
| M10 | queue publishes `tail` with relaxed instead of release ordering | **survives the normal build; killed only by ThreadSanitizer** (race report) |

One further candidate (merge takes one event per command instead of the whole run) *survived* and is an
**equivalent mutant**: the outer loop re-selects the same shard, so output is identical; the `while` is an
efficiency choice, not a correctness one. It was replaced by M3. M10 is why CI has a `tsan` job: on x86 a
missing release/acquire pair is invisible to ordinary tests.

**M8**
- [x] Checksummed, versioned WAL format with explicit invariants; torn-tail vs corruption classification.
- [x] Write-ahead ordering (`DurableMultiEngine`; journal hook on the sequencer), writer poisoning on I/O failure, safe resume.
- [x] Recovery equals the uninterrupted run (both engines); sequential and threaded runs write identical logs.
- [x] Crash-safety property tests (every-byte truncation, every-bit flip), a scanner fuzzer under ASan/UBSan, `/dev/full` test.
- [x] Cost and recovery-speed benchmark with correctness gates.
- [ ] *Not done:* checkpoints/rotation, group commit, faster CRC and batched writes (unprofiled overhead), directory `fsync`.

### Mutation testing of the Phase 8 WAL
Twelve injected bugs, each applied alone to a clean copy; the relevant tests run; the source restored afterwards.

| # | Injected bug | Result |
|---|---|---|
| W1 | checksum never verified | killed |
| W2 | mid-log checksum failure treated as a torn tail (silent data loss) | killed |
| W3 | sequence continuity not checked | killed |
| W4 | "tail region" ten times too wide | killed |
| W5 | apply *before* logging (not write-ahead) | killed |
| W6 | resume does not truncate a torn tail | killed |
| W7 | symbol ids not required to be dense | killed |
| W8 | writer not poisoned after a flush failure | killed, **but only after the test was strengthened** (see defect 12) |
| W9 | resume forgets which symbols are already defined | killed |
| W10 | pipeline journals SNAPSHOT commands too | killed |
| W11 | scanner accepts any side value | killed |
| W12 | `create` overwrites an existing log | killed |

## Defect log (found by this project's own verification)
| # | Found in | Defect | Found by | Resolution |
|---|---|---|---|---|
| 1 | M1 (latent until M5) | `OrderBook` implicitly copyable; copy's index aliased the original's list nodes | review during M5, reproduced under ASan | copy/move deleted; `static_assert` test |
| 2 | M4 | Memory metric (`malloc_usable_size`) hid malloc overhead, making engines look identical | suspicious identical numbers | switched to `mallinfo2`; documented |
| 3 | M5 | Verdict rule for "faster" was applied inconsistently across workloads | re-reading the table | single Bonferroni-corrected sign-test rule |
| 4 | M6 | Checker (and naive model) shared `effective_limit()` with the engine; a market-order bug was invisible | mutation test that *survived* | independent computation; mutant now killed by 4 layers |
| 5 | M6 | Generator emitted market orders with a non-canonical price; broke text round trip | round-trip test | canonicalise to 0 |
| 6 | M6 | A test claiming to check "memory is recycled" only checked validity | review | renamed to what it checks; memory reuse asserted at arena level |
| 7 | M6 | Stray `{include` directory in every shipped zip (brace expansion under `/bin/sh`, M1) | listing the tree | removed |
| 8 | M6 | Mutation-test harness leaked one mutation into the next | identical error text for two mutations | harness restores everything per mutation |
| 9 | M7 | `lob_bench` benchmark validity check knew about ASan/UBSan but not TSan, so a TSan build would be labelled valid | review while adding the `tsan` preset | TSan sets the same sanitizer flag |
| 10 | M7 | A mutation-run verdict of "timeout" under TSan was first read as "detected" | the verdict could not distinguish a race report from slowness | re-ran with a focused test and a longer limit until an actual TSan report appeared; the harness now says which |
| 11 | M7 | A mutation that survived was nearly counted as a test gap | reading the mutant | classified as equivalent and replaced with a real one |
| 12 | M8 | The "writer is poisoned after an I/O error" behaviour was effectively untested: with `/dev/full` every later write fails anyway, so removing the poisoning changed nothing observable | reasoning about the mutant before running it | the test now asserts a rejected append leaves the writer's byte and sequence counters untouched; mutant W8 is killed |
| 13 | M8 | Adding a `std::function` member to an options struct made every existing designated-initializer use warn under `-Wextra` (a CI `-Werror` failure) | the compiler, on the first build | default member initializer `{}` |
| 14 | M8 | Two of my own test expectations were wrong about an empty file (expected "Clean iff `valid_bytes == size`"; a zero-byte file is a torn header with `valid_bytes == 0`) | the truncation sweep at `t = 0` | fixed the test, not the scanner: the scanner's behaviour is the intended design and is covered by the resume test |
| 15 | M8 | My results note first said logging costs "about 100 ns/command" and attributed it to CRC and `fwrite` | proofreading against the CSV: it is about 200 ns, and nothing was profiled | corrected; the cause is documented as unknown with the candidates listed |
