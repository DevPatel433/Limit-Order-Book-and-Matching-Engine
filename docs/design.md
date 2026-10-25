# Design notes — data model and book (Phase 1), matching primitives (Phase 2)

## Scope
Phase 1 *stores* and *reports* resting orders. It does not match. `OrderBook::add`
inserts whatever it is given, so a crossed book is representable (and tested).
Phase 2's `MatchingEngine` will wrap the book and make crossed resting states
unreachable.

## Types
| Type | Definition | Why |
|---|---|---|
| `Price` | `int64_t` ticks | No floating-point keys; exact comparisons |
| `Quantity` | `uint64_t` | Remaining quantity can never be negative |
| `OrderId` | `uint64_t` | Caller-supplied, unique among *live* orders |

Valid prices: `[1, INT64_MAX/2]`. The cap guarantees `ask - bid` cannot overflow.

## Data structures and complexity
| Structure | Choice | Cost |
|---|---|---|
| Bids | `std::map<Price, PriceLevel, greater>` | best = `begin()`, O(1); insert/erase level O(log L) |
| Asks | `std::map<Price, PriceLevel, less>` | same |
| Level queue | `std::list<Order>` + cached total | append O(1); erase-by-handle O(1) |
| ID index | `unordered_map<OrderId, Locator>` | lookup O(1) average |

`Locator = {side, price, list::iterator}`. `std::list` iterators stay valid
until their own element is erased, so the index can hold them safely while
other orders are added/cancelled around them. Cancel is therefore:
index lookup → erase from list → drop level if empty → drop index entry.

These are deliberately simple baselines. Node-based containers are cache-unfriendly;
Phase 4 will profile before anything is replaced (Phase 5).

## Add semantics (checked in this order; failure = book unchanged)
1. `quantity == 0` → `InvalidQuantity`
2. price outside `[kMinPrice, kMaxPrice]` → `InvalidPrice`
3. ID already live (either side) → `DuplicateId`
4. level aggregate would overflow `uint64_t` → `QuantityOverflow`

An ID may be reused after the order is cancelled. (Phase 2 will decide whether
IDs of fully-filled orders are reusable; the same rule — "unique among live
orders" — is the current plan.)

## Cancel semantics
`cancel(id)` returns `true` if a live order was removed, `false` otherwise.
Empty levels are always erased immediately.

## Invariants (`OrderBook::validate`)
* No empty levels; level key == level price; prices in range.
* Every order: quantity > 0, on the correct side, in the correct level.
* Level `total_quantity` and `order_count` equal the sum/count of its queue.
* Every live order has an index entry whose handle points at that exact node,
  with matching side and price.
* Index size == number of live orders (no dangling or duplicate entries).

`add()` and `cancel()` `assert(validate())` when `NDEBUG` is not defined. That is
O(n) per operation — fine for Debug/tests, absent in Release.

## Testing
* Empty, bids-only, asks-only, multi-level, depth-limited snapshots.
* Same-price FIFO (arrival order, not ID order), FIFO preserved across cancels.
* Cancel: last-in-level, unknown, double-cancel, ID reuse.
* Rejections leave the book unchanged (zero qty, bad price, duplicate, overflow).
* Seeded random add/cancel sequences compared against an independent
  reference model after every step, with `validate()` after every step.
* Determinism: identical input → identical snapshot.

## Known limitations (intentional for now)
* No matching, fills, or events (Phase 2).
* Single-threaded, single instrument.
* `std::list`/`std::map` allocation per order/level.

## Phase 2 additions to the book
`OrderBook` gained the minimum needed by the engine, keeping rules out of the book:
* `precheck(order)` — the validation `add()` already did, now reusable.
* `best_order(side)` — copy of the front order at the best price, O(1).
* `execute_front(side, qty)` — reduce the best order; partial fills keep queue
  position, exhausted orders are removed from queue, ID index, and (if empty) level.
  Handles of other orders stay valid, so the ID index never goes stale.
* `is_crossed()` — used in the engine's debug assertions.

`add()` is still allowed to build a crossed book (so Phase 1 tests remain valid);
`MatchingEngine` is the only intended caller in production paths.

## Phase 3: verification design
* **Replay** (`commands`, `replay`): a strict text parser and a runner that writes
  deterministic output. The engine itself does no I/O.
* **Generator**: `std::mt19937_64` + modulo for cross-platform reproducibility.
* **ConsistencyChecker**: deliberately shares no matching code with the engine.
  It consumes only public events, keeps a ledger of live orders (with arrival
  sequence numbers), validates each event against the ledger, and then compares
  the engine's book to the ledger order-for-order. Because it is event-sourced it
  also proves the event stream alone is sufficient to reconstruct the book.
* **Testing the tester**: `test_checker.cpp` feeds hand-made illegal event
  batches (wrong price, overfill, priority/FIFO violations, unjustified
  rejections, ledger/book divergence) and requires each to be flagged. The
  checker was also validated by injecting bugs into a scratch copy of the engine
  (trade at taker's price, LIFO queue, strict-inequality crossing); each was
  detected within the first 30 commands of a generated workload.
* **Cost**: `validate()` (asserted in Debug) and the checker are O(n) per command,
  so property tests use a few thousand commands per seed; run `lob_replay --check`
  on larger generated files in Release.

## Phase 4: measurement design
* **Pre-built operation lists** (`BenchOp`) so generation and parsing are never timed; workloads are
  built by running the real engine untimed, so cancels always hit live orders and sweeps are
  sized relative to the live touch.
* **Separate passes** (throughput / latency / allocation) because per-op clock reads (~23 ns each
  here) and allocation counting both perturb what they measure.
* **Honest statistics**: nearest-rank percentiles gated on tail sample count; clock overhead
  and run-to-run spread reported alongside results; environment recorded in every CSV row;
  Debug/sanitizer builds refused or flagged.
* **Profiling**: the measured loop is a non-inlined function (`lob_measured_segment`) so
  perf/callgrind can target exactly the timed code.
* **Tested**: `tests/test_bench.cpp` covers percentile math (including the 0.999 x 1000
  floating-point boundary), sample-size gating, histogram bins, and that every workload is
  deterministic, reject-free, shaped as documented, and passes the independent consistency checker.

## Phase 5: allocation policy
`BasicPriceLevel/OrderBook/MatchingEngine<Policy>` are templates; `StdPolicy` is the baseline and
`PoolPolicy` supplies a per-book `SlabArena`. The old names alias the baseline. Books are now
non-copyable/non-movable (the old implicit copy silently aliased the original's list nodes).
Design, trade-offs and evidence: [phase5_design.md](phase5_design.md), [results_phase5.md](results_phase5.md).
