# Interview discussion notes

Short, honest answers to the roadmap's discussion prompts and some likely follow-ups, each pointing at
evidence in this repository. Where something was *not* measured or not done, the answer says so.

## 1. Why these data structures, and what are their complexities?
* **Prices: `std::map<Price, Level>` per side** (bids descending, asks ascending). Best price is
  `begin()` in O(1); inserting or erasing a level is O(log L). Chosen first because it is simple and
  obviously correct, and the roadmap says to optimise only with evidence.
* **Per-level FIFO: `std::list<Order>`.** O(1) append, O(1) erase given an iterator, and iterators stay
  valid when *other* elements are erased, which is what lets cancel be O(1).
* **ID index: `std::unordered_map<OrderId, {side, price, list iterator}>`** gives O(1) average cancel/amend.
* Integer tick prices (no floating-point keys) and `uint64` quantities with explicit overflow guards.
* Complexity table: [limitations.md](limitations.md). **What it cost:** node-based containers mean one
  heap allocation per order per container, and pointer chasing; both showed up in profiles (below).

## 2. How does the ID index speed up cancellation, and what consistency issues can it introduce?
Without it, cancel must find the order by scanning levels and queues: O(n). With it: hash lookup ->
list iterator -> erase. The risk is **two structures that must agree**: every live order must be in
exactly one queue *and* in the index, with an iterator that points at that exact node.
* `OrderBook::validate()` checks this (no dangling or duplicate entries, locator side/price correct,
  iterator addresses the right node) and runs after every mutation in Debug builds.
* The independent checker compares the engine's book to an event-sourced ledger order-for-order,
  including queue position.
* **A real bug of this class existed:** `OrderBook` was copyable, and a copy's index pointed into the
  *original's* list nodes. It was found in Phase 5, reproduced under ASan, and fixed by deleting copy/move
  ([phase5_design.md](phase5_design.md)).
* Another consequence: a hash-table rehash is O(n), producing latency spikes of ~0.1-3 ms as the book grows
  ([results_baseline.md](results_baseline.md)). Not fixed.

## 3. How do you test price-time priority and partial fills?
At four independent levels, because each catches things the others miss:
1. **Hand-written examples** for each rule: price priority both sides, FIFO by arrival (not by ID),
   partial maker keeps its position, remainder rests at the back (`test_matching_engine.cpp`).
2. **A naive reference engine** (flat vector + linear scans, no shared code) compared event-for-event on
   thousands of random sequences (and a second one for IOC/FOK/market/amend).
3. **An independent checker** that rebuilds the book from events and verifies price priority, FIFO, no
   over-execution, and queue positions after every command.
4. **Property tests** on seeded generators, plus a global conservation law
   (`accepted + amend_added = 2 x traded + cancelled + expired + resting + amend_removed`).
Plus golden files that were traced by hand. **Mutation testing** confirms the checker bites: seven injected
bugs, six caught at once and one that exposed a real gap in the checker's independence
([phase6_design.md](phase6_design.md)); after the fix all seven are caught.

## 4. What did profiling reveal, and what evidence supports the optimisation?
* **Finding:** callgrind put 30-51% of executed instructions in `malloc`/`free` on every workload, at
  1-2 heap allocations per command ([results_baseline.md](results_baseline.md)).
* **Optimisation:** a per-book arena for node allocation, behind a policy template so the baseline stays
  selectable ([phase5_design.md](phase5_design.md)).
* **Evidence:** paired, interleaved runs (20 reps) with a sign test: ~1.2-1.3x throughput on four of
  five workloads; instructions per command -31% to -46%; allocator share 30-51% -> 0-5%.
* **What it did *not* do:** `many_levels` showed no reliable gain (it is cache-miss bound: simulated
  misses barely changed), and rehash stalls are unchanged. It also *costs* memory behaviour: nothing is
  returned until the book is destroyed, and `bursty` used 26% more heap.
* I also report where my first analysis was wrong (a memory metric that hid malloc overhead; an
  inconsistent significance rule), because the corrections are part of the evidence.

## 5. What would change for multiple instruments or concurrent input?
* **Multiple instruments:** the engine is already one-instance-per-book, so it is routing and plumbing: a
  `symbol -> engine` map, a symbol field on commands/events/CSV, an ID policy (global vs per-symbol).
  **Built in Phase 7** ([phase7_design.md](phase7_design.md)): router, per-symbol IDs, `@SYMBOL` format.
* **Concurrent input:** keep the matching core single-threaded and deterministic and put a **sequencer** in
  front (one total order of inputs per book); scale by sharding books across cores, with SPSC queues in and
  an event queue out. Determinism is what makes the replay/checker tooling still work. The arena has no
  shared state across books. **Built in Phase 7**, with one caveat I state up front: it was developed on a one-CPU
  machine, so I verified correctness (differential tests, TSan, mutation tests) and measured *overhead* (8-18%),
  but I do **not** claim a speedup. What I can say from the workload alone: one book is the unit of parallelism, so with
  80% of flow on one symbol no shard count beats about 1.2x.

## Follow-ups worth being ready for
**How does recovery work, and how do you know it is right?** The engine is deterministic, so I log the *sequenced input* and
replay it; no second code path. Rightness is tested as a property: cut a real log at every byte offset and the recovered
books must equal the exact state after some prefix of the commands; flip every bit and it must be detected unless it falls
in the last record, which is indistinguishable from a torn write (I state that boundary instead of hiding it).

**What is the difference between a torn tail and corruption, and why does it matter?** A crash mid-append truncates or
garbles only the *last* record: discard it and carry on. Bad data with more data after it is not a crash artefact;
silently dropping the rest would lose acknowledged commands, so it is reported and never auto-repaired.

**What does durability cost?** On the dev VM: about 2x to write the log at all, about 4.8x with `fsync` every 1000 records,
about 1000x with `fsync` per record (about 190 microseconds each). The first is my code, unprofiled; the last two are the
storage, and the figure does not transfer to other disks. The fix is group commit, which I did not build.

**Why are order IDs per symbol rather than global?** Each book is already keyed by ID internally, so per-symbol IDs
need no cross-book coordination and let books live on different threads with no shared state. The cost is that
cancels must name their symbol; venue-wide IDs would need a global ID-to-symbol index ([phase7_design.md](phase7_design.md)).

**How do you know the threaded output is right?** It is defined as "equal to the sequential run", and that is
tested: byte-identical output for 1-16 shards on both engines, with tiny queues, skewed load and repeated runs;
each symbol also matches the original single-book engine. Then I broke the pipeline on purpose ten ways; nine were
caught by ordinary tests, and the tenth (a relaxed instead of release store in the queue) is invisible on x86
and only ThreadSanitizer caught it, which is why CI runs a TSan job.

**What would you do first with a multi-core machine?** Run `lob_scale` pinned and idle, publish the table with the
partition bound beside it, and expect the single-threaded producer and output merge to be what limits it; then
replace the offline merge with a streaming one using a completed-sequence watermark.

**Why is the pipeline slower than sequential in your results?** Because the machine had one CPU: the threads
time-share it, so the queueing and hand-off are pure cost. I report it as overhead (8-18%), not as a scaling result.

**Why is a market order modelled as an IOC order with an infinite limit?** One matching path instead of
two; the checker verifies it independently (and a mutation that broke it was caught).

**When does an amend keep time priority?** Only if the price is unchanged and the quantity does not
increase; otherwise it is removed and re-entered (it can trade as a taker). It is validated *before*
removal so a rejection never loses the order ([matching_rules.md](matching_rules.md)).

**Why is FOK "accepted then expired" rather than rejected?** The order was valid; it just could not fill.
It must be atomic: availability is computed before any execution so a killed FOK leaves no trace.

**How do you know your benchmark numbers mean anything?** Three separate passes (throughput, latency,
allocation), warm-up excluded, percentiles reported only when the sample size supports them, clock
overhead reported, environment recorded in every row, Debug builds refused, comparisons paired and
interleaved, an A/A control to show the noise floor. And the numbers are from a noisy shared VM, which the
docs say repeatedly ([benchmarking.md](benchmarking.md)).

**What is the cost of the Phase 6 features?** +1.5% to +3.8% instructions per command, measured
deterministically; not resolvable in wall-clock on this machine ([phase6_design.md](phase6_design.md)).

**What would you do differently?** Start with an intrusive order pool (index-based FIFO) instead of
`std::list` + `unordered_map` and measure that against the arena; use a realistic order-flow model for the
benchmarks; get hardware counters (`perf stat`) for the cache claims; and build the checker's independence
test (mutation testing) in Phase 3 rather than Phase 6.
