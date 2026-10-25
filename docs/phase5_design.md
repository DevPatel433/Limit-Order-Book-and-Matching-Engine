# Phase 5: node pooling (one measured optimisation)

## Target and why
Chosen from the Phase 4 baseline ([results_baseline.md](results_baseline.md)): **allocation churn**.
Callgrind put 30-51% of executed instructions in `malloc`/`free`/`operator new`/`delete` on every
workload, at 1-2 heap allocations per command. Every resting order owns two small nodes (a
`std::list` node in its price level, an `unordered_map` node in the ID index), and every new price
level owns a `std::map` node. Nothing else in the profile was as large or as uniform.

Not chosen (still open): the ID index's rehash stalls (~0.1-3 ms, unchanged by this phase) and the
cache-miss-bound price-level lookups seen in `many_levels`.

## What changed
Memory source is now a **policy** (`include/lob/policy.hpp`); logic is unchanged and shared:

| | `StdPolicy` (baseline) | `PoolPolicy` (optimised) |
|---|---|---|
| node allocator | `std::allocator` -> `operator new` | `ArenaAllocator` -> per-book `SlabArena` |
| classes | `OrderBook`, `MatchingEngine` | `PooledOrderBook`, `PooledMatchingEngine` |

`BasicPriceLevel<P>`, `BasicOrderBook<P>`, `BasicMatchingEngine<P>` are templates over the policy and
are explicitly instantiated for exactly these two policies. The old names (`OrderBook`,
`MatchingEngine`) remain aliases of the baseline, so nothing existing changed meaning and **the
default is still the baseline**. Choose the optimised one explicitly (`PooledMatchingEngine`,
`lob_replay --engine pooled`, `lob_bench --engine pooled|both`).

### `SlabArena` (`include/lob/arena.hpp`, `src/arena.cpp`)
* Requests <= 256 bytes with alignment <= 16 are rounded up to a multiple of 16 and served from a
  per-size-class **LIFO free list**, otherwise carved from the current 64 KiB chunk. No per-block
  header, no locks, no syscalls.
* Freed blocks return to their class's free list and are reused immediately (cache-warm).
* Larger or over-aligned requests (hash bucket arrays) go to `::operator new`.
* Chunks are released only when the book is destroyed.
* Hot path: one load, one compare, one store. Refill (`refill()`, once per 64 KiB) is out of line.

### What is and is not pooled
Pooled: list nodes (48 B), hash nodes (40 B, rounded into the 48 B class), price-level map nodes, and
small hash bucket arrays (<= 256 B, i.e. up to 32 buckets). Not pooled: larger bucket arrays (they
go to `::operator new`, and their rehash cost is untouched) and the caller's `std::vector<Event>`.
(Node sizes verified with a spy allocator on libstdc++ / gcc 13.)

## Correctness strategy ("identical correctness suites before and after")
* **Every existing book and engine test is now a typed test** run against both implementations
  (`OrderBookTyped`, `EngineTyped`), including the randomized comparison with the naive reference model.
* **Differential tests** (`tests/test_pooled.cpp`): both engines run in lock-step on random sequences
  and on every benchmark workload; every event batch and periodic full-book snapshots must be equal.
* The independent `ConsistencyChecker` accepts both engines (templated `observe`); the property
  tests, golden files and regression cases run on both (`ReplayOptions::engine`).
* Arena unit tests: alignment, no overlap, content integrity, LIFO reuse, size-class sharing,
  no growth under steady churn, chunk accounting, large/over-aligned fall-through, use under
  `std::list`/`std::map`/`std::unordered_map` including rehashes.
* All of it passes under ASan + UBSan with `-Werror`.

## Trade-offs (what this costs)
1. **Memory is not returned until the book is destroyed.** Measured: after cancelling every resting
   order, the baseline's heap falls to 0.35-2.9 MiB; the pooled engine stays at its high-water mark
   (2.3-23 MiB). Right for a long-lived engine with stable depth; wrong for one that spikes then idles.
2. **Churny workloads can use more memory.** `bursty` ends with *more* heap on the pooled engine
   (2.3 vs 1.8 MiB): free lists keep the peak, and nodes freed in one size class are never reused by another.
3. **Internal rounding.** 16-byte granularity pads the 40-byte hash node to 48, offsetting part of
   the saving from dropping malloc's 8-byte chunk header. Net: 9-13% less heap per resting order on the
   four steadier workloads, but 26% *more* on `bursty` ([results_phase5.md](results_phase5.md)).
4. **Sanitizers and valgrind lose per-node visibility.** A use-after-free of a pooled node is
   not a use-after-free to ASan: the memory stays valid inside a chunk. Mitigation today: the baseline
   engine remains available and every test runs on it. Possible follow-up: poison freed blocks with
   `ASAN_POISON_MEMORY_REGION` in debug builds.
5. **Complexity.** Templates plus explicit instantiation, a new allocator to maintain, longer build.
6. **Single-threaded only**, like the rest of the engine; books are not copyable or movable.
7. **Does not fix** rehash stalls or cache-miss-bound lookups (measured, see results).

## A bug found on the way
`OrderBook` was implicitly copyable. Its ID index stores list iterators into *its own* queues, so a
copy silently aliased the original's nodes and failed `validate()` on the next cancel (reproduced
under ASan before any change). Nothing in the repo copied a book, so it never fired. Copy and move
are now deleted (arena pointers would dangle on a move too) and `Books.AreNotCopyableOrMovable`
pins it with `static_assert`s.

## Alternatives considered (not benchmarked)
* `std::pmr::unsynchronized_pool_resource`: would likely give similar benefits with less code;
  the custom arena was chosen for a fixed, inspectable size-class layout and zero virtual dispatch.
  A fair comparison is a reasonable future experiment.
* Intrusive order pool + index-based FIFO (drops the `std::list` and hash node entirely): bigger
  redesign, would also change the cancellation-handle story. Worth considering after profiling again.
* `unordered_map::reserve` / flat hash for the ID index: a different candidate (targets rehash stalls).
