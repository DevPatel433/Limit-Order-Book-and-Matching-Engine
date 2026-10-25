# Phase 6: order types, amend, and polish

Phase 6 adds the optional extensions from the roadmap **one at a time, each with explicit semantics
and tests**, plus the review-facing material (CSV output, an interactive shell, lifecycle and
limitation docs, interview notes).

## What was added, and why these three
| Feature | Why it is worth having |
|---|---|
| **IOC / FOK time in force** | Exchanges are full of them; FOK forces an *atomic* all-or-nothing decision (availability check before any execution). |
| **Market orders** | Modelled as "an IOC/FOK order with no price limit", not a separate code path. |
| **Amend / replace** | The classic priority question: when may a modification keep its queue position? |

**Deliberately not added:** multiple instruments, self-trade prevention, post-only/stop/iceberg,
concurrency. Each needs plumbing or state that would dilute the matching story; what each would take
is written down in [limitations.md](limitations.md).

## Design decisions
* **No new matching path for market/IOC/FOK.** `submit` and `amend` share one routine
  (`match_and_dispose`): sweep the opposite side within an effective limit, then rest (GTC) or expire
  (everything else). A market order is just a limit of "infinity". Fewer paths, fewer bugs.
* **`Expired` is its own event** rather than reusing `Cancelled`. `Cancelled` means "a client asked to
  remove a resting order"; `Expired` means "the rules discarded unfilled quantity". Keeping the meanings
  apart keeps the conservation law (`accepted = 2 x traded + cancelled + expired + resting ...`) readable.
* **FOK is accepted, then expired** rather than rejected: the order was valid, it just could not fill.
  Nothing trades and the book is untouched.
* **Amend keeps priority iff price is unchanged and quantity does not increase.** The quantity is the
  *open* quantity. Priority loss re-enters the order as a fresh GTC order with the same ID, so it can trade
  as a taker; it is validated *before* removal so a rejection can never lose the original order.
* **`Order` stays 32 bytes.** `tif` and `type` live in what used to be padding (members reordered, a
  constructor added), pinned by a `static_assert`, because node size drives memory and allocator behaviour
  (Phase 5).
* **Backward compatibility is tested, not assumed.** All 158 pre-existing tests passed unchanged;
  golden outputs and regression files are untouched; the generator draws no new random numbers when the
  new options are 0, and a test pins a hash of a generated file against the Phase 5 generator.

## How the features were verified
1. Example tests for each rule, as typed tests on both engines (`tests/test_extensions.cpp`).
2. A second **naive reference engine** (flat vector, linear scans) covering the whole semantics,
   compared event-for-event on random sequences with IOC/FOK/market/amend mixed in.
3. The independent `ConsistencyChecker` extended to understand `Expired` and `Amended` and the new
   obligations, with 14 hand-made illegal scenarios that it must reject.
4. Property tests with two extension-heavy profiles on both engines, plus the generalised conservation law.
5. A hand-traced sample file (`data/sample/orders.txt`) and two regression files with golden output.
6. **Mutation testing** of the engine (below).

### Mutation testing found two real problems
Seven bugs were injected one at a time into a scratch copy and replayed with `--check`:

| Injected bug | Detected |
|---|---|
| FOK availability check removed | yes: "FOK order was partially filled" |
| IOC remainder rests instead of expiring | yes: "neither filled nor expired" |
| amend always keeps priority | yes: "claims priority kept but the rules say lost" |
| FOK counts only the first level | yes: "expired although it was fully fillable" |
| amend decrease also moves the order to the back | yes: ledger/queue-position mismatch |
| `Expired` reports the original quantity | yes: "wrong remaining quantity" |
| **market order's price limit removed** | **no, at first** |

The survivor was a genuine finding: the checker and my naive test model both called the same
`effective_limit()` helper as the engine, so a bug there was invisible to them, contradicting the claim
that the checker shares no matching logic. Both now compute the market limit independently. After the
fix the same mutant is killed by four layers (the hand-written market tests, the naive-model comparison,
the checker, and the property tests).

*A harness mistake worth recording:* my first mutation script restored only the file each mutation
touched, so one mutation leaked into the next and produced a misleading "caught" for the fourth. The
suspicious identical error message is what gave it away; the script now restores everything per mutation.

Also found while testing: the generator could emit a market order with a junk price (its "bad order"
branch overwrote the canonical 0), which broke the text round trip. Fixed; covered by a round-trip test.

## Cost of the extensions
The extensions make `submit` more general, so the hot path does slightly more work. Measured two ways
against the saved Phase 5 build:

**1. Deterministic (callgrind instructions per measured command, `Ir`):**

| workload | engine | Phase 5 | Phase 6 | change |
|---|---|---:|---:|---:|
| non_crossing | baseline | 832.9 | 849.9 | +17.0 (+2.0%) |
| non_crossing | pooled | 448.4 | 465.4 | +17.0 (+3.8%) |
| cancel_heavy | baseline | 513.6 | 522.1 | +8.5 (+1.7%) |
| cancel_heavy | pooled | 346.0 | 354.5 | +8.5 (+2.5%) |
| crossing_heavy | baseline | 656.6 | 672.6 | +16.0 (+2.4%) |
| crossing_heavy | pooled | 418.4 | 434.4 | +16.0 (+3.8%) |
| many_levels | baseline | 573.2 | 581.8 | +8.6 (+1.5%) |
| many_levels | pooled | 397.0 | 405.5 | +8.5 (+2.1%) |
| bursty | baseline | 647.1 | 658.4 | +11.3 (+1.7%) |
| bursty | pooled | 403.2 | 414.5 | +11.3 (+2.8%) |

(The same absolute delta on both engines shows the cost is in the shared logic, not the allocator.
Splitting `submit` into two functions initially cost +23 instr/op on `crossing_heavy`; forcing
`match_and_dispose` inline recovered 7.)

**2. Wall clock** (`scripts/compare_builds.py`, 6 interleaved rounds x 3 reps, with an A/A control: the
old binary run twice). The noise floor on this shared VM is large: the A/A arm alone gives median ratios
from 0.93 to 1.05 with IQRs up to 0.8-1.23. The new/old ratios (0.87-1.07) are mostly inside that band,
with `crossing_heavy` consistently on the low side. **So the wall-clock A/B cannot resolve a 2-4% effect
here; the deterministic instruction counts are the trustworthy number.** Expectation from them: a low
single-digit-percent slowdown on instruction-bound paths, none on cache-bound `many_levels`. Re-run
`scripts/compare_builds.py <old> <new>` on a quiet machine to measure it directly.

## Polish items
* `lob_replay --csv` (pure event log), `lob_shell` (interactive terminal view, shares `apply_command`
  with replay so the two cannot disagree), `lob_gen` flags for the new options.
* Docs: [order_lifecycle.md](order_lifecycle.md), [limitations.md](limitations.md),
  [interview_notes.md](interview_notes.md), [milestones.md](milestones.md); `CHANGELOG.md`.
* `scripts/quickstart.sh` (build, test, demo in one command), `CMakePresets.json`, issue templates.
* Removed a stray `{include` directory that an early shell mishap (`mkdir` with brace expansion under
  `/bin/sh`) had been shipping in every zip.
